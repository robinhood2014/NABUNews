"""NABU news bridge (The Guardian edition) - headlines and story text.

The NABU connects through the Internet Adapter (RetroNET command 0xD0), sends ONE
request line (command 0xD4), and reads the reply, which always ends with a ^Z
(0x1A) byte.

Requests (ASCII, ending in a newline):
    L                    -> the headline list (also sent if no request arrives
                            within 3 seconds, so simple clients still work)
    S<nn> <ed> <page>    -> one screenful (22 lines) of story <nn>, e.g.
                            "S07 3F 2". <ed> is the edition label printed at the
                            end of the headline list ("#3F"), or "--" for the
                            newest edition.

Story replies start with a "page/total" line, then up to 22 text lines. Errors
start with a line reading "ERR", followed by message lines.

Editions: every refresh makes a new edition. The previous edition is kept too, so
a story number picked from a slightly old list still returns the right article.
Older than that, the NABU is told to reload the list.

Setup:
  1. Get a free developer key at https://open-platform.theguardian.com/
     (developer keys are for non-commercial use).
  2. pip install requests
  3. Set the key in the environment, then run:
       Windows (cmd):         set GUARDIAN_API_KEY=your-key
       Windows (PowerShell):  $env:GUARDIAN_API_KEY="your-key"
       Linux / macOS:         export GUARDIAN_API_KEY=your-key
       python nabu_bridge.py

Optional: set GUARDIAN_SECTION (for example "world" or "technology") to restrict
the headlines to one section.
"""
import math, os, re, sys, socketserver, textwrap, threading, time, unicodedata
import requests

API_URL = "https://content.guardianapis.com/search"
COLS = 40
PORT = 2323
REFRESH_SECONDS = 900            # 15 minutes: well within developer-key limits
MAX_ITEMS = 20
MAX_PAGE_BYTES = 2900            # the NABU program buffers 3072 bytes of headlines
PAGE_LINES = 22                  # must match PAGE_ROWS in the NABU program
MAX_STORY_LINES = 440            # about 20 screens per story
REQUEST_TIMEOUT = 3              # seconds to wait for the NABU's request line
CREDIT = "Powered by the Guardian"

SUBS = {"\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"',
        "\u2013": "-", "\u2014": "-", "\u2026": "...", "\u00a0": " ",
        "\u00a3": "GBP ", "\u20ac": "EUR "}
CONTROL = re.compile(r"[\x00-\x1f\x7f]")

def clean(s):
    for k, v in SUBS.items():
        s = s.replace(k, v)
    s = unicodedata.normalize("NFKD", s)            # cafe, not caf<accent>
    s = s.encode("ascii", "ignore").decode()
    return CONTROL.sub(" ", s).strip()

def fetch_results():
    key = os.environ.get("GUARDIAN_API_KEY")
    if not key:
        raise RuntimeError("GUARDIAN_API_KEY is not set")
    params = {"api-key": key, "order-by": "newest", "type": "article",
              "page-size": MAX_ITEMS, "show-fields": "bodyText"}
    section = os.environ.get("GUARDIAN_SECTION")
    if section:
        params["section"] = section
    r = requests.get(API_URL, params=params, timeout=20)
    if r.status_code in (401, 403):
        raise RuntimeError(f"Guardian API rejected the key (HTTP {r.status_code})")
    r.raise_for_status()
    return r.json()["response"]["results"]

def build_story(title, body):
    lines = textwrap.wrap(title, COLS) + [""]
    paragraphs = [clean(p) for p in (body or "").split("\n")]
    paragraphs = [p for p in paragraphs if p]
    if not paragraphs:
        lines += ["(No article text available.)", ""]
    for p in paragraphs:
        lines += textwrap.wrap(p, COLS) + [""]
    if len(lines) > MAX_STORY_LINES:
        lines = lines[:MAX_STORY_LINES] + ["", "[Shortened - read the rest at", "theguardian.com]", ""]
    lines.append(CREDIT)
    return lines

class Edition:
    def __init__(self, ident, list_page, stories):
        self.ident = ident
        self.list_page = list_page
        self.stories = stories          # story number -> list of wrapped lines

def build(ident):
    results = fetch_results()
    credit_line = f"{CREDIT:<{COLS - 5}}#{ident:02X}"
    credit_bytes = len(credit_line) + 2
    lines, stories, size, n = [], {}, 0, 0
    for item in results:
        title = clean(item.get("webTitle", ""))
        if not title:
            continue
        block = textwrap.wrap(title, COLS, initial_indent=f"{n + 1:02d}. ",
                              subsequent_indent="    ") + [""]
        block_bytes = sum(len(l) + 2 for l in block)
        if size + block_bytes + credit_bytes > MAX_PAGE_BYTES:
            break
        lines += block
        size += block_bytes
        n += 1
        stories[n] = build_story(title, item.get("fields", {}).get("bodyText", ""))
    if not lines:
        lines = ["No headlines available.", ""]
    lines.append(credit_line)
    page = ("\r\n".join(lines) + "\r\n\x1a").encode("ascii")
    return Edition(ident, page, stories)

LOADING_PAGE = b"Loading... press R to retry.\r\n\x1a"
current = None
previous = None
lock = threading.Lock()

def err(*messages):
    return ("\r\n".join(["ERR", *messages]) + "\r\n\x1a").encode("ascii")

def find_edition(label):
    with lock:
        for e in (current, previous):
            if e is not None and (label == "--" and e is current
                                  or label.upper() == f"{e.ident:02X}"):
                return e
    return None

def respond(request):
    parts = request.split()
    cmd = parts[0].upper() if parts else "L"
    if cmd == "L":
        with lock:
            e = current
        return e.list_page if e else LOADING_PAGE
    if cmd.startswith("S") and cmd[1:].isdigit() and len(parts) == 3 and parts[2].isdigit():
        e = find_edition(parts[1])
        if e is None:
            return err("That list is out of date.", "Press R to reload the headlines.")
        lines = e.stories.get(int(cmd[1:]))
        if lines is None:
            return err("No such headline.")
        total = max(1, math.ceil(len(lines) / PAGE_LINES))
        page = int(parts[2])
        if not 1 <= page <= total:
            return err("No such page.")
        chunk = lines[(page - 1) * PAGE_LINES: page * PAGE_LINES]
        return ("\r\n".join([f"{page}/{total}"] + chunk) + "\r\n\x1a").encode("ascii")
    return err("Bad request.")

def refresher():
    global current, previous
    next_id = 1
    while True:
        try:
            new = build(next_id)
            with lock:
                previous, current = current, new
            next_id = next_id % 255 + 1
            print(f"edition {new.ident:02X}: {len(new.stories)} stories, "
                  f"list {len(new.list_page)} bytes", flush=True)
        except Exception as ex:
            print("refresh failed:", ex, flush=True)      # keep serving the last edition
        time.sleep(REFRESH_SECONDS)

def read_request(sock):
    sock.settimeout(REQUEST_TIMEOUT)
    buf = b""
    try:
        while len(buf) < 40 and not buf.endswith(b"\n"):
            c = sock.recv(1)
            if not c:
                break
            buf += c
    except OSError:
        pass
    return buf.decode("ascii", "ignore").strip()

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        request = read_request(self.request)
        data = respond(request)
        print("request:", repr(request), "->", len(data), "bytes", flush=True)
        self.request.sendall(data)
        # Don't close first: the adapter may report "disconnected" before the NABU
        # has read everything. Wait for the adapter to close, or time out.
        self.request.settimeout(30)
        try:
            self.request.recv(1)
        except OSError:
            pass

class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

if __name__ == "__main__":
    if not os.environ.get("GUARDIAN_API_KEY"):
        sys.exit("Set GUARDIAN_API_KEY first (see the notes at the top of this file).")
    threading.Thread(target=refresher, daemon=True).start()
    print("listening on port", PORT, flush=True)
    with Server(('127.0.0.1', PORT), Handler) as server:
        print(f"Nabu Bridge listening on '127.0.0.1':{PORT}")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down")
