/*
 * NABU news reader v7 - Guardian headlines and stories.
 *
 * Talks to nabu_bridge.py through the Internet Adapter's RetroNET TCP commands:
 *   0xD0 open   0xD4 write   0xD2 bytes available   0xD3 read   0xD1 close
 *
 * One connection per request. The program sends a single request line and reads
 * until a ^Z (0x1A):
 *   "L\n"               the headline list
 *   "S<nn> <ed> <p>\n"  page p (22 lines) of story nn, from edition <ed>
 *
 * Only hcca_writeByte()/hcca_readByte() are used for the link. Everything that
 * depends on NABU-LIB (init, video, keyboard) is in the PLATFORM LAYER.
 *
 * Keys: GO or ENTER = pick a story by number, N or SPACE = next page,
 *       P = previous page, T = first page, B = back to the headlines,
 *       R = reload, K = key code finder, H = help, Q = quit to CP/M.
 */

#define BIN_TYPE BIN_CPM
#define FONT_STANDARD
#include <stdint.h>
#include <stdbool.h>
#include "..\NABULIB\NABU-LIB.h"
#include "..\NABULIB\patterns.h"

/* Where the bridge runs, as seen from the machine running the Internet Adapter.
   127.0.0.1 works if the bridge is on that same PC. */
#define BRIDGE_HOST "127.0.0.1"
#define BRIDGE_PORT 2323

/* Key code the NABU's GO key sends (found with the K key finder). It is the same
   code as ENTER, so either key works. */
#define KEY_GO      0x0D

#define COLS        40
#define PAGE_ROWS   22             /* rows 1..22; row 0 = header, row 23 = footer */
#define LIST_MAX    3072           /* bytes of headline list kept in RAM */
#define LIST_LINES  160
#define STORY_MAX   1280           /* one story page: header line + 22 lines */
#define STORY_LINES 32
#define CHUNK       64             /* small replies: HCCA has no flow control */
#define IDLE_MAX    20000          /* polls with no data before giving up */

#define RN_TCP_OPEN   0xD0
#define RN_TCP_CLOSE  0xD1
#define RN_TCP_AVAIL  0xD2
#define RN_TCP_READ   0xD3
#define RN_TCP_WRITE  0xD4

#define FETCH_OK       0
#define FETCH_NOCONN   1
#define FETCH_TIMEOUT  2
#define FETCH_DROPPED  3

#define MODE_LIST      0
#define MODE_STORY     1

/* ---------------------------------------------------------------- */
/* PLATFORM LAYER - adjust to your NABU-LIB version                  */
/* ---------------------------------------------------------------- */
static void plat_init(void)
{
    initNABULib();
    vdp_initTextMode(0x0F, 0x04, false);   /* white on dark blue, no autoscroll */
    vdp_clearScreen();
    vdp_loadASCIIFont(ASCII);
}

static void plat_put(uint8_t r, const char *s)     /* s is COLS chars, NUL-terminated */
{
    vdp_setCursor2(0, r);
    vdp_print((char *)s);
}

static uint8_t plat_key(void)                      /* blocking key read */
{
    return getChar();
}

/* ---------------------------------------------------------------- */
/* RetroNET TCP transport                                            */
/* ---------------------------------------------------------------- */
static void tx16(uint16_t v)
{
    hcca_writeByte((uint8_t)(v & 0xFF));
    hcca_writeByte((uint8_t)(v >> 8));
}

static int32_t rx32(void)
{
    uint32_t v;
    uint8_t b0 = hcca_readByte();
    uint8_t b1 = hcca_readByte();
    uint8_t b2 = hcca_readByte();
    uint8_t b3 = hcca_readByte();
    v = (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
    return (int32_t)v;
}

static void tx_name(const char *s)                 /* length-prefixed string */
{
    uint8_t n = 0;
    while (s[n]) n++;
    hcca_writeByte(n);
    for (uint8_t i = 0; i < n; i++) hcca_writeByte((uint8_t)s[i]);
}

static uint8_t tcp_open(const char *host, uint16_t port)
{
    hcca_writeByte(RN_TCP_OPEN);
    tx_name(host);
    tx16(port);
    hcca_writeByte(0xFF);          /* let the IA assign a handle */
    return hcca_readByte();        /* 0xFF = failed to connect */
}

static void tcp_close(uint8_t h)
{
    hcca_writeByte(RN_TCP_CLOSE);
    hcca_writeByte(h);
}

static int32_t tcp_avail(uint8_t h)
{
    hcca_writeByte(RN_TCP_AVAIL);
    hcca_writeByte(h);
    return rx32();                 /* -1 = disconnected */
}

/* Reads up to len bytes. Always drains the whole reply. Returns count stored,
   or -1 if disconnected. */
static int32_t tcp_read(uint8_t h, uint16_t len, char *buf)
{
    int32_t n, i, stored = 0;

    hcca_writeByte(RN_TCP_READ);
    hcca_writeByte(h);
    tx16(len);
    n = rx32();
    if (n <= 0) return n;
    for (i = 0; i < n; i++) {
        uint8_t c = hcca_readByte();
        if (stored < (int32_t)len) buf[stored++] = (char)c;
    }
    return stored;
}

static int32_t tcp_write(uint8_t h, const char *s, uint8_t n)
{
    hcca_writeByte(RN_TCP_WRITE);
    hcca_writeByte(h);
    tx16(n);
    for (uint8_t i = 0; i < n; i++) hcca_writeByte((uint8_t)s[i]);
    return rx32();                 /* bytes written, -1 = disconnected */
}

/* ---------------------------------------------------------------- */
/* Documents held in RAM: the headline list and the current story page */
/* ---------------------------------------------------------------- */
typedef struct {
    char     *buf;
    uint16_t *starts;
    uint16_t  cap;          /* max bytes */
    uint16_t  max_lines;
    uint16_t  len;
    uint16_t  nlines;
} Doc;

static char     list_buf[LIST_MAX + 1];
static uint16_t list_starts[LIST_LINES];
static char     story_buf[STORY_MAX + 1];
static uint16_t story_starts[STORY_LINES];

static Doc list_doc  = { list_buf,  list_starts,  LIST_MAX,  LIST_LINES,  0, 0 };
static Doc story_doc = { story_buf, story_starts, STORY_MAX, STORY_LINES, 0, 0 };

static char     edition[3] = "--";   /* label of the list we are showing */
static uint8_t  story_num;
static uint16_t story_page;
static uint16_t story_total;

static void split_lines(Doc *d)
{
    uint16_t i;
    bool at_start = true;

    d->nlines = 0;
    d->buf[d->len] = 0;
    for (i = 0; i < d->len; i++) {
        uint8_t c = (uint8_t)d->buf[i];
        if (at_start) {
            if (d->nlines >= d->max_lines) { d->buf[i] = 0; break; }
            d->starts[d->nlines++] = i;
            at_start = false;
        }
        if (c == '\n')      { d->buf[i] = 0; at_start = true; }
        else if (c == '\r') { d->buf[i] = 0; }
        else if (c < 32 || c > 126) { d->buf[i] = '?'; }
    }
}

/* Sends one request line, reads the reply up to ^Z into d, splits it into lines. */
static uint8_t fetch(Doc *d, const char *req)
{
    uint8_t  h, n = 0, status = FETCH_OK;
    int32_t  avail, got;
    uint16_t idle = 0, want, k;
    bool     done = false;

    d->len = 0;
    d->nlines = 0;
    while (req[n]) n++;

    h = tcp_open(BRIDGE_HOST, BRIDGE_PORT);
    if (h == 0xFF) return FETCH_NOCONN;

    if (tcp_write(h, req, n) < 0) {
        tcp_close(h);
        return FETCH_DROPPED;
    }

    while (!done && d->len < d->cap) {
        avail = tcp_avail(h);
        if (avail < 0) {
            if (d->len == 0) status = FETCH_DROPPED;
            break;
        }
        if (avail == 0) {
            if (++idle > IDLE_MAX) {
                if (d->len == 0) status = FETCH_TIMEOUT;
                break;
            }
            continue;
        }
        want = CHUNK;
        if ((int32_t)want > avail) want = (uint16_t)avail;
        if (want > d->cap - d->len) want = d->cap - d->len;

        got = tcp_read(h, want, d->buf + d->len);
        if (got < 0) break;
        if (got == 0) { idle++; continue; }
        idle = 0;

        for (k = 0; k < (uint16_t)got; k++) {
            if (d->buf[d->len + k] == 0x1A) {      /* end-of-page marker */
                got = k;
                done = true;
                break;
            }
        }
        d->len += (uint16_t)got;
    }
    tcp_close(h);

    split_lines(d);
    return status;
}

/* ---------------------------------------------------------------- */
/* Small parsers                                                     */
/* ---------------------------------------------------------------- */
static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

/* The list's last line ends with "#NN": the edition label for story requests. */
static void parse_edition(void)
{
    uint16_t i;
    uint8_t  tries = 0;

    edition[0] = '-'; edition[1] = '-'; edition[2] = 0;
    for (i = list_doc.nlines; i > 0 && tries < 3; i--, tries++) {
        const char *s = list_doc.buf + list_doc.starts[i - 1];
        uint8_t j, hash = 255;
        for (j = 0; s[j]; j++) if (s[j] == '#') hash = j;
        if (hash != 255 && is_hex(s[hash + 1]) && is_hex(s[hash + 2])) {
            edition[0] = s[hash + 1];
            edition[1] = s[hash + 2];
            return;
        }
    }
}

/* Headlines are numbered "01. ..." - count the lines that start that way. */
static uint8_t count_items(void)
{
    uint8_t  n = 0;
    uint16_t i;

    for (i = 0; i < list_doc.nlines; i++) {
        const char *s = list_doc.buf + list_doc.starts[i];
        if (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' && s[2] == '.') n++;
    }
    return n;
}

static uint16_t parse_num(const char **sp)
{
    uint16_t v = 0;
    while (**sp >= '0' && **sp <= '9') { v = v * 10 + (uint16_t)(**sp - '0'); (*sp)++; }
    return v;
}

/* Story reply line 0 is "page/total". */
static bool parse_story_header(void)
{
    const char *s;

    if (story_doc.nlines == 0) return false;
    s = story_doc.buf + story_doc.starts[0];
    story_page = parse_num(&s);
    if (*s != '/') return false;
    s++;
    story_total = parse_num(&s);
    return story_page >= 1 && story_total >= 1;
}

static bool story_is_error(void)
{
    const char *s;

    if (story_doc.nlines == 0) return false;
    s = story_doc.buf + story_doc.starts[0];
    return s[0] == 'E' && s[1] == 'R' && s[2] == 'R';
}

/* ---------------------------------------------------------------- */
/* Screen helpers                                                    */
/* ---------------------------------------------------------------- */
static char row[COLS + 1];

static void row_clear(void)
{
    for (uint8_t i = 0; i < COLS; i++) row[i] = ' ';
    row[COLS] = 0;
}

static uint8_t row_num(uint8_t p, uint32_t v)
{
    char tmp[10];
    uint8_t n = 0;
    do { tmp[n++] = (char)('0' + (uint8_t)(v % 10)); v /= 10; } while (v);
    while (n && p < COLS) row[p++] = tmp[--n];
    return p;
}

static uint8_t row_num2(uint8_t p, uint16_t v)     /* two digits, zero padded */
{
    if (p < COLS) row[p++] = (char)('0' + (uint8_t)((v / 10) % 10));
    if (p < COLS) row[p++] = (char)('0' + (uint8_t)(v % 10));
    return p;
}

static uint8_t row_hex(uint8_t p, uint8_t v)
{
    const char *digits = "0123456789ABCDEF";
    if (p < COLS) row[p++] = digits[v >> 4];
    if (p < COLS) row[p++] = digits[v & 15];
    return p;
}

static uint8_t row_text(uint8_t p, const char *s)
{
    while (*s && p < COLS) row[p++] = *s++;
    return p;
}

static void put_text(uint8_t r, const char *s)
{
    row_clear();
    row_text(0, s);
    plat_put(r, row);
}

static void draw_footer(void)
{
    row_clear();
    row_text(1, "H)ELP");
    row_text(COLS - 5, "Q)UIT");
    plat_put(PAGE_ROWS + 1, row);
}

/* Rows 1..22 show document lines first, first+1, ... */
static void draw_lines(const Doc *d, uint16_t first)
{
    for (uint8_t r = 0; r < PAGE_ROWS; r++) {
        uint16_t idx = first + r;
        row_clear();
        if (idx < d->nlines) {
            const char *s = d->buf + d->starts[idx];
            for (uint8_t i = 0; i < COLS && s[i]; i++) row[i] = s[i];
        }
        plat_put(1 + r, row);
    }
}

static void draw_list(uint16_t top)
{
    uint16_t pages = (list_doc.nlines + PAGE_ROWS - 1) / PAGE_ROWS;
    uint16_t page  = (top / PAGE_ROWS) + 1;
    uint8_t p;

    if (pages == 0) pages = 1;
    row_clear();
    row_text(0, " NABU NEWS v7");
    p = row_text(COLS - 12, "PAGE ");
    p = row_num(p, page);
    p = row_text(p, "/");
    row_num(p, pages);
    plat_put(0, row);

    draw_lines(&list_doc, top);
    draw_footer();
}

static void draw_story(void)
{
    uint8_t p;

    row_clear();
    p = row_text(1, " STORY ");
    p = row_num2(p, story_num);
    row_text(p, "  B)ACK");
    p = row_text(COLS - 12, "PAGE ");
    p = row_num(p, story_page);
    p = row_text(p, "/");
    row_num(p, story_total);
    plat_put(0, row);

    draw_lines(&story_doc, 1);        /* line 0 is the page/total header */
    draw_footer();
}

static void show_error(uint8_t status)
{
    uint8_t p;

    row_clear(); row_text(1, "Could not get that page."); plat_put(2, row);

    row_clear();
    if (status == FETCH_NOCONN)        row_text(1, "Could not connect to the bridge.");
    else if (status == FETCH_TIMEOUT)  row_text(1, "Bridge connected, sent nothing.");
    else if (status == FETCH_DROPPED)  row_text(1, "Bridge dropped the connection.");
    else                               row_text(1, "Bridge sent an empty page.");
    plat_put(4, row);

    row_clear();
    p = row_text(1, "Host: ");
    p = row_text(p, BRIDGE_HOST);
    p = row_text(p, ":");
    row_num(p, BRIDGE_PORT);
    plat_put(6, row);

    row_clear(); row_text(1, "Is nabu_bridge.py running?"); plat_put(8, row);
    row_clear(); row_text(1, "Press R to retry."); plat_put(10, row);
    draw_footer();
}

/* ---------------------------------------------------------------- */
/* Story handling                                                    */
/* ---------------------------------------------------------------- */

/* Fetches page `page` of story `num`. Returns true if a story page is loaded.
   On any problem it shows a message, waits for a key and returns false. */
static bool open_story(uint8_t num, uint16_t page)
{
    char     req[24];
    char     t[5];
    uint8_t  p = 0, n = 0, st;
    uint16_t v = page;

    req[p++] = 'S';
    req[p++] = (char)('0' + num / 10);
    req[p++] = (char)('0' + num % 10);
    req[p++] = ' ';
    req[p++] = edition[0];
    req[p++] = edition[1];
    req[p++] = ' ';
    do { t[n++] = (char)('0' + (uint8_t)(v % 10)); v /= 10; } while (v && n < 4);
    while (n) req[p++] = t[--n];
    req[p++] = '\n';
    req[p] = 0;

    row_clear(); row_text(0, "Fetching story..."); plat_put(PAGE_ROWS + 1, row);
    st = fetch(&story_doc, req);

    if (story_doc.nlines == 0) {
        vdp_clearScreen();
        show_error(st);
        (void)plat_key();
        return false;
    }
    if (story_is_error()) {
        vdp_clearScreen();
        put_text(2, " The bridge says:");
        for (uint8_t i = 1; i < story_doc.nlines && i < 6; i++)
            put_text(3 + i, story_doc.buf + story_doc.starts[i]);
        put_text(10, " Press any key.");
        draw_footer();
        (void)plat_key();
        return false;
    }
    if (!parse_story_header()) {
        vdp_clearScreen();
        put_text(2, " Unexpected reply from the bridge.");
        put_text(4, " Press any key.");
        draw_footer();
        (void)plat_key();
        return false;
    }
    story_num = num;
    return true;
}

/* Asks for a headline number on the bottom row. Returns 1..n, or 0 if cancelled. */
static uint8_t prompt_story(uint8_t n_items)
{
    char    digits[3];
    uint8_t len = 0, i, p, k, val;
    bool    bad = false;

    for (;;) {
        row_clear();
        p = row_text(1, "STORY NUMBER (1-");
        p = row_num(p, n_items);
        p = row_text(p, "): ");
        for (i = 0; i < len; i++) row[p++] = digits[i];
        if (p < COLS) row[p] = '_';
        if (bad) row_text(30, "NOT VALID");
        plat_put(PAGE_ROWS + 1, row);

        k = plat_key();
        bad = false;
        if (k >= '0' && k <= '9') {
            if (len < 2) digits[len++] = (char)k;
        } else if (k == 0x08 || k == 0x7F) {
            if (len) len--;
        } else if (k == 0x1B) {
            return 0;
        } else if (k == '\r' || k == '\n' || k == KEY_GO) {
            if (len == 0) return 0;
            val = (len == 1) ? (uint8_t)(digits[0] - '0')
                             : (uint8_t)((digits[0] - '0') * 10 + (digits[1] - '0'));
            if (val >= 1 && val <= n_items) return val;
            bad = true;
            len = 0;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Help and key finder                                               */
/* ---------------------------------------------------------------- */
static void show_help(void)
{
    vdp_clearScreen();
    put_text(0,  " HELP - KEYS");
    put_text(2,  " GO or ENTER  Pick a story by number");
    put_text(3,  " N or SPACE   Next page");
    put_text(4,  " P            Previous page");
    put_text(5,  " T            First page");
    put_text(6,  " B            Back to the headlines");
    put_text(7,  " R            Reload the headlines");
    put_text(8,  " K            Show a key's code");
    put_text(9,  " H            This help screen");
    put_text(10, " Q            Quit and restart NABU");
    put_text(12, " At the story number prompt:");
    put_text(13, "  0-9 type   DEL erase   ESC cancel");
    put_text(14, "  GO or ENTER opens the story");
    put_text(16, " Press any key to go back.");
    (void)plat_key();
}

static void key_finder(void)
{
    uint8_t k, p;

    vdp_clearScreen();
    put_text(0, " KEY FINDER");
    put_text(2, " Press a key to see its code.");
    k = plat_key();
    row_clear();
    p = row_text(1, "Code: 0x");
    p = row_hex(p, k);
    p = row_text(p, "  (decimal ");
    p = row_num(p, k);
    row_text(p, ")");
    plat_put(4, row);
    put_text(6, " Press any key to go back.");
    (void)plat_key();
}

static void redraw(uint8_t mode, uint16_t top, uint8_t status)
{
    vdp_clearScreen();
    if (mode == MODE_STORY)         draw_story();
    else if (list_doc.nlines == 0)  show_error(status);
    else                            draw_list(top);
}

/* ---------------------------------------------------------------- */
void main(void)
{
    uint16_t top = 0;
    uint8_t  status = FETCH_OK, mode = MODE_LIST, n_items = 0, num, k;
    bool     reload = true;

    plat_init();

    for (;;) {
        if (reload) {
            vdp_clearScreen();
            row_clear(); row_text(1, "Fetching headlines..."); plat_put(10, row);
            status = fetch(&list_doc, "L\n");
            parse_edition();
            n_items = count_items();
            top = 0;
            mode = MODE_LIST;
            reload = false;
            redraw(mode, top, status);
        }

        k = plat_key();

        if (k == KEY_GO || k == '\r' || k == '\n' || k == 'g' || k == 'G') {
            if (n_items) {
                num = prompt_story(n_items);
                if (num) mode = open_story(num, 1) ? MODE_STORY : MODE_LIST;
                redraw(mode, top, status);
            }
            continue;
        }

        switch (k) {
            case 'n': case 'N': case ' ':
                if (mode == MODE_LIST) {
                    if (list_doc.nlines && top + PAGE_ROWS < list_doc.nlines) {
                        top += PAGE_ROWS;
                        redraw(mode, top, status);
                    }
                } else if (story_page < story_total) {
                    if (!open_story(story_num, story_page + 1)) mode = MODE_LIST;
                    redraw(mode, top, status);
                }
                break;
            case 'p': case 'P':
                if (mode == MODE_LIST) {
                    if (list_doc.nlines && top >= PAGE_ROWS) {
                        top -= PAGE_ROWS;
                        redraw(mode, top, status);
                    }
                } else if (story_page > 1) {
                    if (!open_story(story_num, story_page - 1)) mode = MODE_LIST;
                    redraw(mode, top, status);
                }
                break;
            case 't': case 'T':
                if (mode == MODE_LIST) {
                    if (list_doc.nlines && top != 0) {
                        top = 0;
                        redraw(mode, top, status);
                    }
                } else if (story_page != 1) {
                    if (!open_story(story_num, 1)) mode = MODE_LIST;
                    redraw(mode, top, status);
                }
                break;
            case 'b': case 'B':
                if (mode == MODE_STORY) {
                    mode = MODE_LIST;
                    redraw(mode, top, status);
                }
                break;
            case 'r': case 'R':
                reload = true;
                break;
            case 'h': case 'H':
                show_help();
                redraw(mode, top, status);
                break;
            case 'k': case 'K':
                key_finder();
                redraw(mode, top, status);
                break;
            case 'q': case 'Q':
                vdp_clearScreen();
                vdp_disableVDPReadyInt();
                __asm
                    di
                    rst 0x00
                __endasm;
            default:
                break;
        }
    }
}
