/* lineedit.c - the raw-terminal line editor.
 *
 * Redraws are wrap- and UTF-8-aware: the input may span several terminal
 * rows, and cursor placement is computed in display columns (wide
 * characters count as two). Below the input it can show a completion
 * menu (grid of candidates with descriptions, colored by kind and
 * LS_COLORS, with a selectable highlight), and to the right an optional
 * right prompt. */
#include "lineedit.h"
#include "highlight.h"
#include "history.h"
#include "complete.h"
#include "prompt.h"
#include "colors.h"
#include "config.h"
#include "builtins.h"
#include "vars.h"
#include <termios.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

static void wr(const char *s, size_t n) {
    while (n > 0) {
        ssize_t r = write(STDOUT_FILENO, s, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return;
        s += r; n -= (size_t)r;
    }
}
static void wrs(const char *s) { wr(s, strlen(s)); }

static struct termios g_orig_termios;
static bool g_raw_active = false;

void lineedit_init(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
}

static void raw_on(void) {
    if (!isatty(STDIN_FILENO)) return;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
    g_raw_active = true;
}

static void raw_off(void) {
    if (!g_raw_active) return;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &g_orig_termios);
    g_raw_active = false;
}

void lineedit_shutdown(void) { raw_off(); }

static int read_byte(void) {
    unsigned char c;
    ssize_t n;
    do { n = read(STDIN_FILENO, &c, 1); } while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;
    return c;
}

static size_t term_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

static size_t term_height(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return 24;
}

/* ------------------------------------------------------------------ */

typedef struct {
    dstr_t buf;
    size_t cursor;
    char *ptail;        /* last line of the rendered prompt (redrawn every time) */
    char *rprompt;      /* rendered right prompt, or NULL */
    size_t cur_row;     /* terminal row of the cursor, relative to the prompt row */

    /* completion menu */
    bool menu;          /* a menu is displayed */
    bool selecting;     /* Tab-cycling through it */
    int sel;            /* selected candidate, -1 = none */
    Completion comp;
    size_t comp_start;  /* buffer offset of the word being completed */
    size_t comp_end;    /* end of the currently inserted text */
    char *comp_orig;    /* original text of the word (for Esc) */
    int menu_scroll;    /* first visible menu row */

    /* history browsing */
    int hist_nav;
    char *hist_saved;
    char *hist_prefix;

    /* kill ring / undo / Alt-. */
    char *yank;
    strvec_t undo;
    int lastarg_back;
    size_t lastarg_start, lastarg_end;
} LState;

/* ---------- UTF-8 cursor stepping ---------- */

static size_t next_cp(LState *s, size_t i) {
    if (i >= s->buf.len) return s->buf.len;
    i++;
    while (i < s->buf.len && ((unsigned char)s->buf.data[i] & 0xC0) == 0x80) i++;
    return i;
}

static size_t prev_cp(LState *s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s->buf.data[i] & 0xC0) == 0x80) i--;
    return i;
}

/* ---------- autosuggestion ---------- */

static const char *find_suggestion(LState *s) {
    if (s->cursor != s->buf.len || s->buf.len == 0 || s->menu) return NULL;
    if (strcmp(config_get("autosuggest", "yes"), "no") == 0) return NULL;
    return history_find_prefix_match(s->buf.data);
}

/* ---------- menu rendering ---------- */

static const char *kind_color(const Cand *c, char *buf, size_t cap) {
    int lk = -1;
    switch (c->kind) {
        case CK_DIR: lk = 1; break;
        case CK_EXEC: lk = 2; break;
        case CK_LINK: lk = 3; break;
        case CK_FILE: lk = 0; break;
        default: break;
    }
    if (lk >= 0) {
        const char *sgr = lscolor_for(c->text, lk);
        if (sgr) { snprintf(buf, cap, "\x1b[%sm", sgr); return buf; }
        return "";
    }
    const char *cls = "text";
    switch (c->kind) {
        case CK_COMMAND: case CK_ALIAS: case CK_ABBR: cls = "command"; break;
        case CK_BUILTIN: case CK_FUNCTION: cls = "builtin"; break;
        case CK_KEYWORD: cls = "keyword"; break;
        case CK_VAR: cls = "variable"; break;
        case CK_OPTION: cls = "flag"; break;
        default: cls = "completion"; break;
    }
    char *code = ansi_code_for(config_color(cls));
    snprintf(buf, cap, "%s", code);
    free(code);
    return buf;
}

/* the text shown for a candidate: just the last path component for files */
static const char *cand_display(const Cand *c) {
    if (c->kind == CK_FILE || c->kind == CK_DIR || c->kind == CK_EXEC || c->kind == CK_LINK) {
        size_t n = strlen(c->text);
        const char *end = c->text + n - (n && c->text[n - 1] == '/' ? 1 : 0);
        const char *p = end;
        while (p > c->text && p[-1] != '/') p--;
        return p;
    }
    return c->text;
}

typedef struct { size_t cols, rows, cellw, textw; bool descs; } MenuLayout;

static MenuLayout menu_layout(LState *s, size_t W) {
    MenuLayout L = {1, 0, 1, 1, false};
    size_t maxt = 1, maxd = 0;
    for (size_t i = 0; i < s->comp.cands.count; i++) {
        size_t tw = display_width(cand_display(&s->comp.cands.items[i]));
        if (tw > maxt) maxt = tw;
        if (s->comp.cands.items[i].desc) {
            L.descs = true;
            size_t dw = display_width(s->comp.cands.items[i].desc);
            if (dw > maxd) maxd = dw;
        }
    }
    if (maxt > W - 4) maxt = W - 4;
    L.textw = maxt;
    size_t cell = maxt + 2;
    if (L.descs) {
        if (maxd > 40) maxd = 40;
        cell += maxd + 4; /* "  (desc)" */
    }
    if (cell > W - 1) cell = W - 1;
    L.cellw = cell;
    L.cols = (W - 1) / cell;
    if (L.cols < 1) L.cols = 1;
    L.rows = (s->comp.cands.count + L.cols - 1) / L.cols;
    return L;
}

/* appends the menu lines (without leading newline) to *out; returns line count */
static size_t render_menu(LState *s, dstr_t *out, size_t W) {
    MenuLayout L = menu_layout(s, W);
    size_t maxrows = term_height() > 8 ? term_height() / 2 : 4;
    if (maxrows > 16) maxrows = 16;
    size_t vis = L.rows < maxrows ? L.rows : maxrows;
    if (s->sel >= 0) {
        int selrow = s->sel % (int)L.rows;
        if (selrow < s->menu_scroll) s->menu_scroll = selrow;
        if (selrow >= s->menu_scroll + (int)vis) s->menu_scroll = selrow - (int)vis + 1;
    } else s->menu_scroll = 0;
    if (s->menu_scroll + vis > L.rows) s->menu_scroll = (int)(L.rows - vis);
    if (s->menu_scroll < 0) s->menu_scroll = 0;

    char *desc_code = ansi_code_for(config_color("description"));
    char *sel_code = ansi_code_for(config_color("selection"));
    size_t lines = 0;
    for (size_t r = (size_t)s->menu_scroll; r < (size_t)s->menu_scroll + vis; r++) {
        if (lines) ds_append(out, "\r\n");
        for (size_t c = 0; c < L.cols; c++) {
            size_t idx = c * L.rows + r; /* column-major, like ls */
            if (idx >= s->comp.cands.count) break;
            const Cand *cd = &s->comp.cands.items[idx];
            bool selected = (int)idx == s->sel;
            char cbuf[64];
            const char *col = kind_color(cd, cbuf, sizeof(cbuf));
            const char *disp = cand_display(cd);
            size_t used = 0;
            if (selected) ds_append(out, sel_code[0] ? sel_code : "\x1b[7m");
            else ds_append(out, col);
            /* text, truncated to its column */
            size_t tw = display_width(disp);
            /* fish's pager: the part already typed is shown in its own style */
            size_t pre = 0;
            if (!selected && s->comp_orig) {
                const char *typed = strrchr(s->comp_orig, '/');
                typed = typed ? typed + 1 : s->comp_orig;
                if (*typed == '"' || *typed == '\'') typed++;
                size_t tl = strlen(typed);
                if (tl && tl <= strlen(disp) && strncasecmp(disp, typed, tl) == 0 && tw <= L.textw) pre = tl;
            }
            if (pre) {
                char *pc = ansi_code_for(config_color("completion_prefix"));
                ds_append(out, ANSI_RESET); ds_append(out, pc); ds_append(out, col);
                ds_append_n(out, disp, pre);
                ds_append(out, ANSI_RESET); ds_append(out, col);
                ds_append(out, disp + pre);
                used = tw;
                free(pc);
            } else if (tw > L.textw) {
                size_t k = 0, w = 0;
                while (disp[k] && w + 1 < L.textw) { size_t st = k; k++; while (disp[k] && ((unsigned char)disp[k] & 0xC0) == 0x80) k++; w += display_width_n(disp + st, k - st); }
                ds_append_n(out, disp, k); ds_append(out, "…");
                used = w + 1;
            } else { ds_append(out, disp); used = tw; }
            if (cd->kind == CK_DIR && disp[0] && disp[strlen(disp) - 1] != '/') { ds_append_c(out, '/'); used++; }
            if (!selected) ds_append(out, ANSI_RESET);
            if (cd->desc && L.descs) {
                size_t pad = L.textw + 2 > used ? L.textw + 2 - used : 1;
                for (size_t k = 0; k < pad; k++) ds_append_c(out, ' ');
                used += pad;
                if (!selected) ds_append(out, desc_code);
                ds_append_c(out, '(');
                size_t room = L.cellw > used + 4 ? L.cellw - used - 4 : 0;
                const char *d = cd->desc;
                size_t k = 0, w = 0;
                while (d[k] && w < room) { size_t st = k; k++; while (d[k] && ((unsigned char)d[k] & 0xC0) == 0x80) k++; w += display_width_n(d + st, k - st); }
                ds_append_n(out, d, k);
                if (d[k]) { ds_append(out, "…"); w++; }
                ds_append_c(out, ')');
                used += w + 2;
                if (!selected) ds_append(out, ANSI_RESET);
            }
            if (selected) ds_append(out, ANSI_RESET);
            if (c + 1 < L.cols && (c + 1) * L.rows + r < s->comp.cands.count)
                for (size_t k = used; k < L.cellw; k++) ds_append_c(out, ' ');
        }
        lines++;
    }
    if (vis < L.rows) {
        char foot[96];
        snprintf(foot, sizeof(foot), "\r\n%s… rows %d-%zu of %zu (%zu matches)%s", desc_code, s->menu_scroll + 1,
                 (size_t)s->menu_scroll + vis, L.rows, s->comp.cands.count, ANSI_RESET);
        ds_append(out, foot);
        lines++;
    }
    free(desc_code);
    free(sel_code);
    return lines;
}

/* ---------- redraw ---------- */

static void redraw_ex(LState *s, bool final) {
    size_t W = term_width();
    dstr_t out; ds_init(&out);
    ds_append(&out, "\x1b[?25l"); /* hide cursor while drawing: no flicker */
    if (s->cur_row > 0) { char m[32]; snprintf(m, sizeof(m), "\x1b[%zuA", s->cur_row); ds_append(&out, m); }
    ds_append_c(&out, '\r');
    ds_append(&out, s->ptail);
    char *hl = highlight_line(s->buf.data);
    ds_append(&out, hl);
    free(hl);

    size_t pw = display_width(s->ptail);
    size_t total = pw + display_width(s->buf.data);
    const char *sugg = final ? NULL : find_suggestion(s);
    if (sugg) {
        const char *rest = sugg + s->buf.len;
        color_wrap(&out, config_color("suggestion"), rest);
        total += display_width(rest);
    }
    bool rp_drawn = false;
    if (s->rprompt && *s->rprompt) {
        size_t rw = display_width(s->rprompt);
        if (total + rw + 2 <= W) {
            char m[32]; snprintf(m, sizeof(m), "\x1b[K\x1b[%zuG", W - rw);
            ds_append(&out, m);
            ds_append(&out, s->rprompt);
            ds_append(&out, ANSI_RESET);
            rp_drawn = true;
        }
    }
    if (!rp_drawn && total > 0 && total % W == 0) ds_append(&out, " \b"); /* settle a pending wrap */
    ds_append(&out, "\x1b[J");
    size_t end_row = total / W;
    if (rp_drawn) end_row = 0;

    size_t menu_lines = 0;
    if (s->menu && !final && s->comp.cands.count > 0) {
        ds_append(&out, "\r\n");
        menu_lines = render_menu(s, &out, W);
    }

    /* place the cursor */
    size_t cur_cols = final ? total : pw + display_width_n(s->buf.data, s->cursor);
    size_t trow = cur_cols / W, tcol = cur_cols % W;
    size_t now_row = end_row + menu_lines;
    if (menu_lines) now_row = end_row + menu_lines; /* the "\r\n" moved down one, lines-1 more */
    if (now_row > trow) { char m[32]; snprintf(m, sizeof(m), "\x1b[%zuA", now_row - trow); ds_append(&out, m); }
    ds_append_c(&out, '\r');
    if (tcol) { char m[32]; snprintf(m, sizeof(m), "\x1b[%zuC", tcol); ds_append(&out, m); }
    ds_append(&out, "\x1b[?25h");
    s->cur_row = trow;
    wr(out.data, out.len);
    ds_free(&out);
}

static void redraw(LState *s) { redraw_ex(s, false); }

static char *g_left_override = NULL, *g_right_override = NULL;

void lineedit_set_prompt_override(const char *left, const char *right) {
    free(g_left_override); free(g_right_override);
    g_left_override = left ? xstrdup(left) : NULL;
    g_right_override = right ? xstrdup(right) : NULL;
}

static char *render_primary(const char *fmt) {
    if (fmt && g_left_override) return xstrdup(g_left_override);
    return fmt ? prompt_render(fmt) : prompt_render_continuation();
}

/* exposed for the `commandline` builtin (completion functions call it) */
static LState *g_active = NULL;
const char *lineedit_buffer(void) { return g_active && g_active->buf.data ? g_active->buf.data : ""; }
size_t lineedit_cursor(void) { return g_active ? g_active->cursor : 0; }

/* ---------- editing primitives ---------- */

static void insert_text(LState *s, const char *t, size_t n) {
    ds_reserve(&s->buf, n);
    memmove(s->buf.data + s->cursor + n, s->buf.data + s->cursor, s->buf.len - s->cursor);
    memcpy(s->buf.data + s->cursor, t, n);
    s->buf.len += n;
    s->buf.data[s->buf.len] = '\0';
    s->cursor += n;
}

static void insert_char(LState *s, char c) { insert_text(s, &c, 1); }

static void kill_range_ex(LState *s, size_t from, size_t to, bool save) {
    if (to <= from) return;
    if (save) { free(s->yank); s->yank = xstrndup(s->buf.data + from, to - from); }
    memmove(s->buf.data + from, s->buf.data + to, s->buf.len - to);
    s->buf.len -= (to - from);
    s->buf.data[s->buf.len] = '\0';
    s->cursor = from;
}
static void kill_range(LState *s, size_t from, size_t to) { kill_range_ex(s, from, to, true); }

static void delete_at_cursor(LState *s) { kill_range_ex(s, s->cursor, next_cp(s, s->cursor), false); }

static void backspace(LState *s) {
    if (s->cursor == 0) return;
    kill_range_ex(s, prev_cp(s, s->cursor), s->cursor, false);
}

static void set_buffer(LState *s, const char *text) {
    ds_clear(&s->buf);
    ds_append(&s->buf, text);
    s->cursor = s->buf.len;
}

static bool is_word_char(char c) { return isalnum((unsigned char)c) || c == '_' || ((unsigned char)c & 0x80); }

static size_t word_forward_pos(LState *s) {
    size_t i = s->cursor;
    while (i < s->buf.len && !is_word_char(s->buf.data[i])) i++;
    while (i < s->buf.len && is_word_char(s->buf.data[i])) i++;
    return i;
}

static size_t word_backward_pos(LState *s) {
    size_t i = s->cursor;
    while (i > 0 && !is_word_char(s->buf.data[i-1])) i--;
    while (i > 0 && is_word_char(s->buf.data[i-1])) i--;
    return i;
}

/* whitespace-delimited "big word" before the cursor (Ctrl-W) */
static size_t bigword_backward_pos(LState *s) {
    size_t i = s->cursor;
    while (i > 0 && isspace((unsigned char)s->buf.data[i-1])) i--;
    while (i > 0 && !isspace((unsigned char)s->buf.data[i-1])) i--;
    return i;
}

static void transpose_chars(LState *s) {
    if (s->buf.len < 2 || s->cursor == 0) return;
    size_t i = s->cursor >= s->buf.len ? s->buf.len - 1 : s->cursor;
    if (i == 0) return;
    char tmp = s->buf.data[i-1];
    s->buf.data[i-1] = s->buf.data[i];
    s->buf.data[i] = tmp;
    if (s->cursor < s->buf.len) s->cursor++;
}

static void case_word(LState *s, int mode) { /* 1 upper, -1 lower, 0 capitalize */
    size_t i = s->cursor;
    while (i < s->buf.len && !is_word_char(s->buf.data[i])) i++;
    bool first = true;
    while (i < s->buf.len && is_word_char(s->buf.data[i])) {
        char *c = &s->buf.data[i];
        if (mode > 0 || (mode == 0 && first)) *c = (char)toupper((unsigned char)*c);
        else *c = (char)tolower((unsigned char)*c);
        first = false;
        i++;
    }
    s->cursor = i;
}

static void accept_suggestion(LState *s, bool one_word) {
    const char *sugg = find_suggestion(s);
    if (!sugg) { s->cursor = s->buf.len; return; }
    if (!one_word) { set_buffer(s, sugg); return; }
    const char *rest = sugg + s->buf.len;
    size_t k = 0;
    while (rest[k] && !is_word_char(rest[k])) k++;
    while (rest[k] && is_word_char(rest[k])) k++;
    insert_text(s, rest, k);
}

/* ---------- abbreviations ---------- */

/* expands a fish abbreviation in command position right before the cursor */
static bool expand_abbr(LState *s) {
    size_t end = s->cursor, start = end;
    while (start > 0 && !isspace((unsigned char)s->buf.data[start - 1]) && !strchr("|&;(", s->buf.data[start - 1])) start--;
    if (start == end) return false;
    /* command position: only whitespace back to a separator / line start */
    size_t b = start;
    while (b > 0 && isspace((unsigned char)s->buf.data[b - 1])) b--;
    if (b > 0 && !strchr("|&;(", s->buf.data[b - 1])) {
        /* allow after `sudo`, `and`, `or`, `not` ... */
        size_t ws = b;
        while (ws > 0 && !isspace((unsigned char)s->buf.data[ws - 1])) ws--;
        char *prev = xstrndup(s->buf.data + ws, b - ws);
        bool ok = !strcmp(prev, "sudo") || !strcmp(prev, "and") || !strcmp(prev, "or") || !strcmp(prev, "not") ||
                  !strcmp(prev, "time") || !strcmp(prev, "then") || !strcmp(prev, "do") || !strcmp(prev, "else");
        free(prev);
        if (!ok) return false;
    }
    char *w = xstrndup(s->buf.data + start, end - start);
    const char *exp = abbr_get(w);
    free(w);
    if (!exp) return false;
    kill_range_ex(s, start, end, false);
    insert_text(s, exp, strlen(exp));
    return true;
}

/* ---------- completion ---------- */

static void menu_close(LState *s) {
    if (s->menu) completion_free(&s->comp);
    s->menu = false;
    s->selecting = false;
    s->sel = -1;
    free(s->comp_orig);
    s->comp_orig = NULL;
}

static char *escaped_cand(LState *s, const Cand *c) {
    if (c->kind == CK_VAR) return xstrdup(c->text);
    char *esc = complete_escape(c->text, s->comp.quote);
    if (!s->comp.quote) return esc;
    dstr_t d; ds_init(&d);
    ds_append_c(&d, s->comp.quote);
    ds_append(&d, esc);
    free(esc);
    char *r = xstrdup(d.data);
    ds_free(&d);
    return r;
}

/* replaces [comp_start, comp_end) with `text` */
static void replace_word(LState *s, const char *text) {
    s->cursor = s->comp_end;
    kill_range_ex(s, s->comp_start, s->comp_end, false);
    insert_text(s, text, strlen(text));
    s->comp_end = s->cursor;
}

static void select_cand(LState *s, int idx) {
    int n = (int)s->comp.cands.count;
    if (n == 0) return;
    s->sel = ((idx % n) + n) % n;
    char *t = escaped_cand(s, &s->comp.cands.items[s->sel]);
    replace_word(s, t);
    free(t);
}

static size_t common_prefix_len(CandList *v) {
    if (v->count == 0) return 0;
    size_t plen = strlen(v->items[0].text);
    for (size_t i = 1; i < v->count; i++) {
        size_t j = 0;
        while (j < plen && v->items[i].text[j] == v->items[0].text[j]) j++;
        plen = j;
    }
    /* don't split a UTF-8 sequence */
    while (plen > 0 && ((unsigned char)v->items[0].text[plen] & 0xC0) == 0x80) plen--;
    return plen;
}

static void do_complete(LState *s, int dir) {
    if (s->menu && s->comp.cands.count > 1) {
        /* second Tab (or Shift-Tab): enter / continue menu selection */
        s->selecting = true;
        if (s->sel < 0) select_cand(s, dir > 0 ? 0 : (int)s->comp.cands.count - 1);
        else select_cand(s, s->sel + dir);
        redraw(s);
        return;
    }
    menu_close(s);
    Completion c;
    complete_line(s->buf.data, s->cursor, &c);
    if (c.cands.count == 0) { completion_free(&c); wrs("\a"); return; }
    s->comp = c;
    s->comp_start = c.word_start;
    s->comp_end = s->cursor;
    s->comp_orig = xstrndup(s->buf.data + s->comp_start, s->comp_end - s->comp_start);
    s->sel = -1;
    s->menu = true;

    if (c.cands.count == 1) {
        Cand *only = &s->comp.cands.items[0];
        char *t = escaped_cand(s, only);
        dstr_t d; ds_init(&d); ds_append(&d, t); free(t);
        size_t tl = strlen(only->text);
        bool is_dir = tl && only->text[tl - 1] == '/';
        bool open_ended = is_dir || (tl && (only->text[tl - 1] == '=' || only->text[tl - 1] == ':'));
        if (!open_ended) {
            if (s->comp.quote) ds_append_c(&d, s->comp.quote);
            if (s->cursor >= s->buf.len || s->buf.data[s->cursor] != ' ') ds_append_c(&d, ' ');
        }
        replace_word(s, d.data);
        ds_free(&d);
        menu_close(s);
        redraw(s);
        return;
    }
    /* several: extend to the longest common prefix, then show the menu */
    size_t cp = common_prefix_len(&s->comp.cands);
    if (cp > 0) {
        char *pre = xstrndup(s->comp.cands.items[0].text, cp);
        Cand tmp = { pre, NULL, s->comp.cands.items[0].kind };
        char *t = escaped_cand(s, &tmp);
        if (strlen(t) > s->comp_end - s->comp_start || strcmp(t, s->comp_orig) != 0) {
            /* only replace if it doesn't lose what was typed (fuzzy matches may not share the prefix) */
            if (strncasecmp(pre, s->comp_orig, strlen(s->comp_orig)) == 0 || strlen(pre) >= strlen(s->comp_orig)) replace_word(s, t);
        }
        free(t);
        free(pre);
    }
    redraw(s);
}

/* ---------- history ---------- */

static void hist_reset(LState *s) {
    s->hist_nav = 0;
    free(s->hist_saved); s->hist_saved = NULL;
    free(s->hist_prefix); s->hist_prefix = NULL;
}

static void hist_move(LState *s, int dir) {
    if (s->hist_nav == 0 && dir > 0) {
        free(s->hist_saved); s->hist_saved = xstrdup(s->buf.data);
        free(s->hist_prefix); s->hist_prefix = xstrdup(s->buf.data);
    }
    const char *prefix = s->hist_prefix ? s->hist_prefix : "";
    size_t pl = strlen(prefix);
    int n = s->hist_nav;
    const char *cur = s->buf.data;
    for (;;) {
        n += dir;
        if (n <= 0) {
            s->hist_nav = 0;
            set_buffer(s, s->hist_saved ? s->hist_saved : "");
            return;
        }
        const char *e = history_get_relative(n);
        if (!e) { wrs("\a"); return; }
        /* prefix search (fish-style): skip non-matches and repeats */
        if ((pl == 0 || strncmp(e, prefix, pl) == 0) && strcmp(e, cur) != 0) {
            s->hist_nav = n;
            set_buffer(s, e);
            return;
        }
    }
}

/* Alt-. : insert the last argument of earlier commands, cycling back */
static void insert_last_arg(LState *s, bool continuing) {
    if (continuing) { kill_range_ex(s, s->lastarg_start, s->lastarg_end, false); s->lastarg_back++; }
    else s->lastarg_back = 1;
    const char *e = history_get_relative(s->lastarg_back);
    if (!e) { wrs("\a"); s->lastarg_back--; e = s->lastarg_back > 0 ? history_get_relative(s->lastarg_back) : NULL; if (!e) return; }
    size_t n = strlen(e);
    while (n > 0 && isspace((unsigned char)e[n - 1])) n--;
    size_t st = n;
    while (st > 0 && !isspace((unsigned char)e[st - 1])) st--;
    s->lastarg_start = s->cursor;
    insert_text(s, e + st, n - st);
    s->lastarg_end = s->cursor;
}

/* ---------- Ctrl-R incremental search ---------- */

static void reverse_search(LState *s) {
    dstr_t query; ds_init(&query);
    char *original = xstrdup(s->buf.data);
    size_t original_cursor = s->cursor;
    int back = 0;
    const char *match = NULL;
    bool cancelled = false;
    int pending_key = -1;
    for (;;) {
        size_t W = term_width();
        dstr_t line; ds_init(&line);
        if (s->cur_row > 0) { char m[32]; snprintf(m, sizeof(m), "\x1b[%zuA", s->cur_row); ds_append(&line, m); }
        ds_append(&line, "\r\x1b[J");
        dstr_t vis; ds_init(&vis);
        bool failed = !match && query.len > 0;
        ds_append(&vis, failed ? "\x1b[31m(failed reverse-i-search)\x1b[0m`" : "\x1b[36m(reverse-i-search)\x1b[0m`");
        ds_append(&vis, query.data);
        ds_append(&vis, "': ");
        size_t cursor_cols = display_width(vis.data);
        if (match) {
            const char *hit = query.len ? strstr(match, query.data) : NULL;
            if (hit) {
                cursor_cols += display_width_n(match, (size_t)(hit - match));
                ds_append_n(&vis, match, (size_t)(hit - match));
                ds_append(&vis, "\x1b[1;4;33m"); ds_append(&vis, query.data); ds_append(&vis, "\x1b[0m");
                ds_append(&vis, hit + query.len);
            } else ds_append(&vis, match);
        }
        ds_append(&line, vis.data);
        size_t tot = display_width(vis.data);
        if (tot > 0 && tot % W == 0) ds_append(&line, " \b");
        size_t end_row = tot / W, crow = cursor_cols / W, ccol = cursor_cols % W;
        if (end_row > crow) { char m[32]; snprintf(m, sizeof(m), "\x1b[%zuA", end_row - crow); ds_append(&line, m); }
        ds_append_c(&line, '\r');
        if (ccol) { char m[32]; snprintf(m, sizeof(m), "\x1b[%zuC", ccol); ds_append(&line, m); }
        s->cur_row = crow;
        wr(line.data, line.len);
        ds_free(&line); ds_free(&vis);

        int sc = read_byte();
        if (sc < 0) { cancelled = true; break; }
        if (sc == 18) { /* search again, older */
            if (query.len > 0) {
                int nb = history_find_substring(query.data, back);
                if (nb) { back = nb; match = history_get_relative(back); }
                else wrs("\a");
            }
            continue;
        }
        if (sc == 127 || sc == 8) {
            if (query.len > 0) { query.len--; query.data[query.len] = '\0'; }
            back = 0; match = NULL;
            if (query.len > 0) {
                int nb = history_find_substring(query.data, 0);
                if (nb) { back = nb; match = history_get_relative(nb); }
            }
            continue;
        }
        if (sc == 7 || sc == 3) { cancelled = true; break; } /* Ctrl-G / Ctrl-C */
        if (sc == 27) { break; } /* Esc: keep the match for editing */
        if (sc == '\r' || sc == '\n') { pending_key = '\r'; break; }
        if (sc >= 32) {
            ds_append_c(&query, (char)sc);
            back = 0; match = NULL;
            int nb = history_find_substring(query.data, 0);
            if (nb) { back = nb; match = history_get_relative(nb); }
            continue;
        }
        pending_key = sc;
        break;
    }
    if (cancelled) { set_buffer(s, original); s->cursor = original_cursor; }
    else if (match) set_buffer(s, match);
    ds_free(&query);
    free(original);
    hist_reset(s);
    redraw(s);
    (void)pending_key;
}

/* ---------- Ctrl-X Ctrl-E / Alt-E: edit in $EDITOR ---------- */

static void edit_in_editor(LState *s) {
    const char *ed = var_get("VISUAL");
    if (!ed || !*ed) ed = var_get("EDITOR");
    if (!ed || !*ed) ed = "vi";
    char path[] = "/tmp/spire-edit-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { wrs("\a"); return; }
    ssize_t _w = write(fd, s->buf.data, s->buf.len); (void)_w;
    _w = write(fd, "\n", 1); (void)_w;
    close(fd);
    s->cursor = s->buf.len;
    redraw_ex(s, true);
    wrs("\r\n");
    raw_off();
    dstr_t cmd; ds_init(&cmd);
    ds_append(&cmd, ed); ds_append(&cmd, " '"); ds_append(&cmd, path); ds_append(&cmd, "'");
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
        signal(SIGTTOU, SIG_DFL); signal(SIGTTIN, SIG_DFL);
        execl("/bin/sh", "sh", "-c", cmd.data, (char *)NULL);
        _exit(127);
    }
    ds_free(&cmd);
    if (pid > 0) {
        tcsetpgrp(STDIN_FILENO, getpgrp());
        int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    }
    raw_on();
    FILE *f = fopen(path, "r");
    if (f) {
        dstr_t d; ds_init(&d);
        char chunk[4096]; size_t r;
        while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) ds_append_n(&d, chunk, r);
        fclose(f);
        while (d.len && (d.data[d.len - 1] == '\n' || d.data[d.len - 1] == '\r')) d.data[--d.len] = '\0';
        /* the editor may produce several lines: join them so the line editor
         * stays single-line; `;` keeps them as separate commands */
        for (size_t i = 0; i < d.len; i++) if (d.data[i] == '\n') d.data[i] = ';';
        set_buffer(s, d.data);
        ds_free(&d);
    }
    unlink(path);
    s->cur_row = 0;
}

/* ---------- undo ---------- */

static void undo_push(LState *s, const char *prev) {
    if (s->undo.count && strcmp(s->undo.items[s->undo.count - 1], prev) == 0) return;
    if (s->undo.count >= 200) { free(s->undo.items[0]); memmove(s->undo.items, s->undo.items + 1, sizeof(char *) * (s->undo.count - 1)); s->undo.count--; }
    sv_push_dup(&s->undo, prev);
}

/* ------------------------------------------------------------------ */

/* prints the PROMPT_SP marker: if the previous output didn't end with a
 * newline, a dim ⏎ is left at its end and the prompt starts on a fresh line */
static void prompt_sp(void) {
    if (strcmp(config_get("prompt.missing_newline_marker", "yes"), "no") == 0) return;
    size_t W = term_width();
    dstr_t d; ds_init(&d);
    ds_append(&d, "\x1b[90m⏎\x1b[0m");
    for (size_t i = 1; i < W; i++) ds_append_c(&d, ' ');
    ds_append(&d, "\r\x1b[K");
    wr(d.data, d.len);
    ds_free(&d);
}

static void print_prompt_head(const char *rendered, char **tail_out) {
    const char *nl = strrchr(rendered, '\n');
    if (nl) {
        dstr_t d; ds_init(&d);
        for (const char *p = rendered; p <= nl; p++) {
            if (*p == '\n') ds_append(&d, "\x1b[K\r\n"); else ds_append_c(&d, *p);
        }
        wr(d.data, d.len);
        ds_free(&d);
        *tail_out = xstrdup(nl + 1);
    } else *tail_out = xstrdup(rendered);
}

/* is there unread input (type-ahead)? */
static bool input_pending(void) {
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    return poll(&pfd, 1, 0) > 0;
}

/* Fade a new prompt in, from dim grey to its real colors, over a few
 * frames. Skipped when keys are already waiting, so it never slows typing. */
static void animate_prompt(const char *ptail) {
    if (strcmp(config_get("prompt.animate", "yes"), "no") == 0) return;
    const char *term = getenv("TERM");
    if (!term || strcmp(term, "dumb") == 0) return;
    long total_ms = atol(config_get("prompt.animate_ms", "60"));
    if (total_ms <= 0) return;
    /* the prompt's text without its colors */
    dstr_t plain; ds_init(&plain);
    for (const char *p = ptail; *p; p++) {
        if (*p == 0x1b && p[1] == '[') { p += 2; while (*p && !(*p >= 0x40 && *p <= 0x7e)) p++; if (!*p) break; continue; }
        ds_append_c(&plain, *p);
    }
    static const int shades[] = { 237, 240, 244, 248 };
    int frames = (int)(sizeof(shades) / sizeof(shades[0]));
    struct timespec ts = { 0, (total_ms * 1000000L) / frames };
    for (int i = 0; i < frames; i++) {
        if (input_pending()) break;
        char frame[64];
        snprintf(frame, sizeof(frame), "\x1b[?25l\r\x1b[38;5;%dm", shades[i]);
        dstr_t out; ds_init(&out);
        ds_append(&out, frame); ds_append(&out, plain.data); ds_append(&out, "\x1b[0m\x1b[K");
        wr(out.data, out.len);
        ds_free(&out);
        nanosleep(&ts, NULL);
    }
    wrs("\x1b[?25h");
    ds_free(&plain);
}

static char *read_plain_line(void) {
    dstr_t buf; ds_init(&buf);
    int c; bool any = false;
    while ((c = getchar()) != EOF) {
        any = true;
        if (c == '\n') break;
        ds_append_c(&buf, (char)c);
    }
    if (!any && buf.len == 0) { ds_free(&buf); return NULL; }
    char *r = xstrdup(buf.data);
    ds_free(&buf);
    return r;
}

char *lineedit_read(const char *prompt_fmt) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return read_plain_line();

    char *prompt_rendered = render_primary(prompt_fmt);

    raw_on();
    LState s; memset(&s, 0, sizeof(s));
    ds_init(&s.buf);
    sv_init(&s.undo);
    s.sel = -1;
    const char *rfmt = config_get("rprompt", "");
    if (prompt_fmt && g_left_override) s.rprompt = g_right_override ? xstrdup(g_right_override) : NULL;
    else s.rprompt = (prompt_fmt && rfmt && *rfmt) ? prompt_render(rfmt) : NULL;
    if (s.rprompt && display_width(s.rprompt) == 0) { free(s.rprompt); s.rprompt = NULL; }

    if (prompt_fmt) prompt_sp();
    print_prompt_head(prompt_rendered, &s.ptail);
    if (prompt_fmt) animate_prompt(s.ptail);
    bool eof = false;
    int last_key = 0;
    g_active = &s;

    redraw(&s);

    for (;;) {
        int c = read_byte();
        if (c < 0) { eof = s.buf.len == 0; break; }

        char *before = xstrdup(s.buf.data);
        int key = c;
        bool was_lastarg = (last_key == 1000 + '.');

        /* ---- menu selection mode: arrows move, Enter accepts ---- */
        if (s.menu && s.selecting && (c == '\r' || c == '\n')) {
            menu_close(&s);
            redraw(&s);
            free(before);
            last_key = c;
            continue;
        }

        if (c != 9 && c != 27 && s.menu) {
            if (!(c == 7 && s.selecting)) { menu_close(&s); }
        }

        if (c == '\r' || c == '\n') {
            expand_abbr(&s);
            s.cursor = s.buf.len;
            redraw_ex(&s, true);
            wrs("\r\n");
            free(before);
            break;
        }

        switch (c) {
            case 3: /* Ctrl-C: cancel line */
                s.cursor = s.buf.len;
                redraw_ex(&s, true);
                wrs("\x1b[90m^C\x1b[0m\r\n");
                ds_clear(&s.buf); s.cursor = 0;
                hist_reset(&s);
                s.cur_row = 0;
                {
                    char *fresh = render_primary(prompt_fmt);
                    free(s.ptail);
                    print_prompt_head(fresh, &s.ptail);
                    free(fresh);
                }
                redraw(&s);
                break;
            case 4: /* Ctrl-D */
                if (s.buf.len == 0) { eof = true; break; }
                delete_at_cursor(&s);
                redraw(&s);
                break;
            case 12: /* Ctrl-L */
                wrs("\x1b[H\x1b[2J");
                {
                    char *fresh = render_primary(prompt_fmt);
                    free(s.ptail);
                    print_prompt_head(fresh, &s.ptail);
                    free(fresh);
                }
                s.cur_row = 0;
                redraw(&s);
                break;
            case 127: case 8: backspace(&s); redraw(&s); break;
            case 1: s.cursor = 0; redraw(&s); break;                       /* Ctrl-A */
            case 5: accept_suggestion(&s, false); redraw(&s); break;        /* Ctrl-E */
            case 2: s.cursor = prev_cp(&s, s.cursor); redraw(&s); break;   /* Ctrl-B */
            case 6:                                                        /* Ctrl-F */
                if (s.cursor < s.buf.len) s.cursor = next_cp(&s, s.cursor); else accept_suggestion(&s, false);
                redraw(&s);
                break;
            case 16: hist_move(&s, 1); redraw(&s); break;                  /* Ctrl-P */
            case 14: hist_move(&s, -1); redraw(&s); break;                 /* Ctrl-N */
            case 21: kill_range(&s, 0, s.cursor); redraw(&s); break;       /* Ctrl-U */
            case 11: kill_range(&s, s.cursor, s.buf.len); redraw(&s); break; /* Ctrl-K */
            case 23: kill_range(&s, bigword_backward_pos(&s), s.cursor); redraw(&s); break; /* Ctrl-W */
            case 25: if (s.yank) insert_text(&s, s.yank, strlen(s.yank)); redraw(&s); break; /* Ctrl-Y */
            case 20: transpose_chars(&s); redraw(&s); break;               /* Ctrl-T */
            case 9: do_complete(&s, 1); break;                             /* Tab */
            case 18: reverse_search(&s); break;                            /* Ctrl-R */
            case 31: case 26:                                              /* Ctrl-_ / Ctrl-Z: undo */
                if (s.undo.count) {
                    char *prev = s.undo.items[--s.undo.count];
                    set_buffer(&s, prev);
                    free(prev);
                } else wrs("\a");
                redraw(&s);
                free(before); before = NULL;
                break;
            case 24: { /* Ctrl-X prefix: Ctrl-X Ctrl-E edits in $EDITOR */
                int c2 = read_byte();
                if (c2 == 5 || c2 == 'e') { edit_in_editor(&s); redraw(&s); }
                break;
            }
            case 7: /* Ctrl-G: cancel menu selection, restoring the word */
                if (s.menu && s.selecting && s.comp_orig) { char *o = xstrdup(s.comp_orig); replace_word(&s, o); free(o); }
                menu_close(&s);
                redraw(&s);
                break;
            case 27: { /* ESC sequences and Alt-keys */
                int c1 = read_byte();
                if (c1 == '[' || c1 == 'O') {
                    int c2 = read_byte();
                    int mod = 0; /* 3 = Alt, 5 = Ctrl */
                    if (c2 == '1' && c1 == '[') {
                        int c3 = read_byte();
                        if (c3 == ';') { mod = read_byte() - '0'; c2 = read_byte(); }
                        else if (c3 == '~') c2 = 'H';
                    } else if (c2 >= '2' && c2 <= '8' && c1 == '[') {
                        int c3 = read_byte();
                        if (c3 == ';') { mod = read_byte() - '0'; c3 = read_byte(); }
                        if (c3 == '~') {
                            if (c2 == '3') { if (mod == 5) kill_range(&s, s.cursor, word_forward_pos(&s)); else delete_at_cursor(&s); }
                            else if (c2 == '7') s.cursor = 0;
                            else if (c2 == '4' || c2 == '8') accept_suggestion(&s, false);
                            else if (c2 == '5') hist_move(&s, 1);   /* PgUp */
                            else if (c2 == '6') hist_move(&s, -1);  /* PgDn */
                        }
                        if (s.menu) menu_close(&s);
                        redraw(&s);
                        break;
                    }
                    if (s.menu && s.selecting) {
                        /* arrows navigate the menu grid */
                        MenuLayout L = menu_layout(&s, term_width());
                        int rows = (int)L.rows;
                        if (c2 == 'A') { select_cand(&s, s.sel - 1); redraw(&s); break; }
                        if (c2 == 'B') { select_cand(&s, s.sel + 1); redraw(&s); break; }
                        if (c2 == 'C') { select_cand(&s, s.sel + rows < (int)s.comp.cands.count ? s.sel + rows : s.sel % rows); redraw(&s); break; }
                        if (c2 == 'D') { select_cand(&s, s.sel - rows >= 0 ? s.sel - rows : s.sel); redraw(&s); break; }
                        if (c2 == 'Z') { do_complete(&s, -1); break; }
                    }
                    if (c2 == 'Z') { do_complete(&s, -1); break; }   /* Shift-Tab */
                    if (s.menu) menu_close(&s);
                    if (c2 == 'A') hist_move(&s, 1);
                    else if (c2 == 'B') hist_move(&s, -1);
                    else if (c2 == 'C') {
                        if (mod == 3 || mod == 5) { if (s.cursor >= s.buf.len) accept_suggestion(&s, true); else s.cursor = word_forward_pos(&s); }
                        else if (s.cursor < s.buf.len) s.cursor = next_cp(&s, s.cursor);
                        else accept_suggestion(&s, false);
                    }
                    else if (c2 == 'D') { if (mod == 3 || mod == 5) s.cursor = word_backward_pos(&s); else s.cursor = prev_cp(&s, s.cursor); }
                    else if (c2 == 'H') s.cursor = 0;
                    else if (c2 == 'F') accept_suggestion(&s, false);
                    redraw(&s);
                    break;
                }
                if (s.menu) menu_close(&s);
                switch (c1) {
                    case 'f': case 'F':
                        if (s.cursor >= s.buf.len) accept_suggestion(&s, true); else s.cursor = word_forward_pos(&s);
                        break;
                    case 'b': case 'B': s.cursor = word_backward_pos(&s); break;
                    case 'd': case 'D': kill_range(&s, s.cursor, word_forward_pos(&s)); break;
                    case 127: case 8: kill_range(&s, word_backward_pos(&s), s.cursor); break;
                    case '.': case '_': insert_last_arg(&s, was_lastarg); key = 1000 + '.'; break;
                    case 'u': case 'U': case_word(&s, 1); break;
                    case 'l': case 'L': case_word(&s, -1); break;
                    case 'c': case 'C': case_word(&s, 0); break;
                    case 'e': case 'E': edit_in_editor(&s); break;
                    case 's': case 'S': /* toggle a leading sudo */
                        if (str_has_prefix(s.buf.data, "sudo ")) { size_t cur = s.cursor; kill_range_ex(&s, 0, 5, false); s.cursor = cur >= 5 ? cur - 5 : 0; }
                        else { size_t cur = s.cursor; s.cursor = 0; insert_text(&s, "sudo ", 5); s.cursor = cur + 5; }
                        break;
                    case 't': case 'T': { /* transpose words */
                        size_t e2 = word_forward_pos(&s); size_t b2 = e2; while (b2 > 0 && is_word_char(s.buf.data[b2 - 1])) b2--;
                        size_t save = s.cursor; s.cursor = b2; size_t b1 = word_backward_pos(&s); size_t e1 = b1; while (e1 < s.buf.len && is_word_char(s.buf.data[e1])) e1++;
                        if (e1 <= b2 && b1 < e1 && b2 < e2) {
                            char *w1 = xstrndup(s.buf.data + b1, e1 - b1), *mid = xstrndup(s.buf.data + e1, b2 - e1), *w2 = xstrndup(s.buf.data + b2, e2 - b2);
                            dstr_t d; ds_init(&d); ds_append(&d, w2); ds_append(&d, mid); ds_append(&d, w1);
                            memcpy(s.buf.data + b1, d.data, d.len);
                            s.cursor = e2;
                            ds_free(&d); free(w1); free(mid); free(w2);
                        } else s.cursor = save;
                        break;
                    }
                    default: break;
                }
                redraw(&s);
                break;
            }
            default:
                if (c == ' ') { expand_abbr(&s); insert_char(&s, ' '); redraw(&s); break; }
                if (c >= 0xC0) {
                    /* UTF-8 lead byte: read the whole sequence before redrawing */
                    char seq[4]; int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
                    seq[0] = (char)c;
                    int k = 1;
                    for (; k < len; k++) { int b = read_byte(); if (b < 0) break; seq[k] = (char)b; }
                    insert_text(&s, seq, (size_t)k);
                    redraw(&s);
                    break;
                }
                if (c >= 32) { insert_char(&s, (char)c); redraw(&s); break; }
                break; /* other control chars: ignore */
        }
        if (eof) { free(before); break; }
        if (before) {
            if (strcmp(before, s.buf.data) != 0) {
                undo_push(&s, before);
                if (key != 1000 + '.' && s.hist_nav == 0) { /* typing ends a history prefix search */ }
            }
            free(before);
        }
        last_key = key;
    }

    g_active = NULL;
    menu_close(&s);
    raw_off();
    hist_reset(&s);
    free(s.yank);
    sv_free(&s.undo);
    free(s.ptail);
    free(s.rprompt);
    free(prompt_rendered);

    if (eof) { ds_free(&s.buf); return NULL; }
    char *result = xstrdup(s.buf.data);
    ds_free(&s.buf);
    return result;
}
