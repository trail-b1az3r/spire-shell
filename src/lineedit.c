#include "lineedit.h"
#include "highlight.h"
#include "history.h"
#include "complete.h"
#include "prompt.h"
#include "colors.h"
#include "config.h"
#include <termios.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

static void wr(const char *s, size_t n) { ssize_t _r = write(STDOUT_FILENO, s, n); (void)_r; }

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
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_active = true;
}

static void raw_off(void) {
    if (!g_raw_active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
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

typedef struct { dstr_t buf; size_t cursor; } LState;

/* fish-style autosuggestion: the most recent history entry that extends
 * the current buffer, offered only when the cursor is at the very end
 * (matches fish's behavior — no suggestion while editing mid-line). */
static const char *find_suggestion(LState *s) {
    if (s->cursor != s->buf.len || s->buf.len == 0) return NULL;
    return history_find_prefix_match(s->buf.data);
}

static void goto_end_or_accept(LState *s) {
    const char *sugg = find_suggestion(s);
    if (sugg) { ds_clear(&s->buf); ds_append(&s->buf, sugg); }
    s->cursor = s->buf.len;
}

static void redraw(const char *prompt_rendered, LState *s) {
    dstr_t out; ds_init(&out);
    ds_append_c(&out, '\r');
    ds_append(&out, prompt_rendered);
    char *hl = highlight_line(s->buf.data);
    ds_append(&out, hl);
    free(hl);

    size_t sugg_len = 0;
    const char *sugg = find_suggestion(s);
    if (sugg) {
        sugg_len = strlen(sugg) - s->buf.len;
        color_wrap_n(&out, config_color("suggestion"), sugg + s->buf.len, sugg_len);
    }

    ds_append(&out, "\x1b[K");
    size_t move_left = (s->buf.len - s->cursor) + sugg_len;
    if (move_left > 0) {
        char seq[32];
        snprintf(seq, sizeof(seq), "\x1b[%zuD", move_left);
        ds_append(&out, seq);
    }
    wr(out.data, out.len);
    ds_free(&out);
}

static void insert_char(LState *s, char c) {
    ds_reserve(&s->buf, 1);
    memmove(s->buf.data + s->cursor + 1, s->buf.data + s->cursor, s->buf.len - s->cursor);
    s->buf.data[s->cursor] = c;
    s->buf.len++;
    s->buf.data[s->buf.len] = '\0';
    s->cursor++;
}

static void delete_at_cursor(LState *s) {
    if (s->cursor >= s->buf.len) return;
    memmove(s->buf.data + s->cursor, s->buf.data + s->cursor + 1, s->buf.len - s->cursor - 1);
    s->buf.len--;
    s->buf.data[s->buf.len] = '\0';
}

static void backspace(LState *s) {
    if (s->cursor == 0) return;
    s->cursor--;
    delete_at_cursor(s);
}

static void kill_range(LState *s, size_t from, size_t to) {
    if (to <= from) return;
    memmove(s->buf.data + from, s->buf.data + to, s->buf.len - to);
    s->buf.len -= (to - from);
    s->buf.data[s->buf.len] = '\0';
    s->cursor = from;
}

/* Alt/Meta word motion: skip current run of spaces, then the run of
 * non-space beyond it (forward), or the mirror image (backward). */
static size_t word_forward_pos(LState *s) {
    size_t i = s->cursor;
    while (i < s->buf.len && isspace((unsigned char)s->buf.data[i])) i++;
    while (i < s->buf.len && !isspace((unsigned char)s->buf.data[i])) i++;
    return i;
}

static size_t word_backward_pos(LState *s) {
    size_t i = s->cursor;
    while (i > 0 && isspace((unsigned char)s->buf.data[i-1])) i--;
    while (i > 0 && !isspace((unsigned char)s->buf.data[i-1])) i--;
    return i;
}

static void transpose_chars(LState *s) {
    if (s->buf.len < 2) return;
    size_t i = s->cursor;
    if (i == 0) return;
    if (i >= s->buf.len) i = s->buf.len - 1;
    if (i == 0) return;
    char tmp = s->buf.data[i-1];
    s->buf.data[i-1] = s->buf.data[i];
    s->buf.data[i] = tmp;
    if (s->cursor < s->buf.len) s->cursor++;
}

static size_t common_prefix_len(strvec_t *v) {
    if (v->count == 0) return 0;
    size_t plen = strlen(v->items[0]);
    for (size_t i = 1; i < v->count; i++) {
        size_t j = 0;
        while (j < plen && v->items[i][j] == v->items[0][j]) j++;
        plen = j;
    }
    return plen;
}

/* persists across consecutive Tab presses within one lineedit_read call;
 * reset (via complete_state_reset) on any other keystroke. */
typedef struct {
    bool active;       /* a menu-cycle is in progress */
    strvec_t cands;
    size_t idx;          /* candidate that would be inserted next */
    size_t word_start;   /* where the completed word begins in the buffer */
} CompleteState;

static void complete_state_reset(CompleteState *cs) {
    if (cs->active) sv_free(&cs->cands);
    cs->active = false;
    cs->idx = 0;
}

/* dirs get the path color, $vars the variable color, everything else the
 * command color -- cheap enough of a heuristic to not need re-stat'ing. */
static void print_candidate_list(strvec_t *cands) {
    dstr_t list; ds_init(&list);
    ds_append_c(&list, '\n');
    for (size_t i = 0; i < cands->count; i++) {
        const char *c = cands->items[i];
        size_t clen = strlen(c);
        const char *cls = "command";
        if (clen > 0 && c[0] == '$') cls = "variable";
        else if (clen > 0 && c[clen - 1] == '/') cls = "path";
        color_wrap_n(&list, config_color(cls), c, clen);
        ds_append(&list, (i + 1 < cands->count) ? "  " : "\n");
    }
    wr(list.data, list.len);
    ds_free(&list);
}

static void do_complete(LState *s, const char *prompt_rendered, CompleteState *cs) {
    if (cs->active) {
        /* consecutive Tab: cycle to the next candidate in place */
        kill_range(s, cs->word_start, s->cursor);
        s->cursor = cs->word_start;
        const char *pick = cs->cands.items[cs->idx];
        for (const char *p = pick; *p; p++) insert_char(s, *p);
        cs->idx = (cs->idx + 1) % cs->cands.count;
        redraw(prompt_rendered, s);
        return;
    }

    strvec_t cands; sv_init(&cands);
    size_t word_start;
    complete_line(s->buf.data, s->cursor, &cands, &word_start);
    if (cands.count == 0) { wr("\a", 1); sv_free(&cands); return; }

    size_t cp = common_prefix_len(&cands);
    size_t cur_word_len = s->cursor - word_start;
    if (cp > cur_word_len) {
        kill_range(s, word_start, s->cursor);
        s->cursor = word_start;
        for (size_t i = 0; i < cp; i++) insert_char(s, cands.items[0][i]);
    }

    if (cands.count > 1) {
        print_candidate_list(&cands);
        /* next Tab (with the buffer unchanged since) starts cycling */
        cs->active = true;
        cs->cands = cands;
        cs->idx = 0;
        cs->word_start = word_start;
    } else {
        sv_free(&cands);
    }
    redraw(prompt_rendered, s);
}

char *lineedit_read(const char *prompt_fmt) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        /* non-interactive fallback: plain buffered read */
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

    char *prompt_rendered = prompt_fmt ? prompt_render(prompt_fmt) : prompt_render_continuation();

    raw_on();
    LState s; ds_init(&s.buf); s.cursor = 0;
    int hist_nav = 0;
    char *saved_line = NULL;
    bool eof = false;
    CompleteState cs; memset(&cs, 0, sizeof(cs));

    redraw(prompt_rendered, &s);

    for (;;) {
        int c = read_byte();
        if (c < 0) { eof = (s.buf.len == 0); break; }
        if (c != 9) complete_state_reset(&cs); /* any non-Tab key ends a completion cycle */

        if (c == '\r' || c == '\n') { wr("\r\n", 2); break; }

        if (c == 3) { /* Ctrl-C: cancel line */
            wr("^C\r\n", 4);
            ds_clear(&s.buf); s.cursor = 0; hist_nav = 0;
            free(saved_line); saved_line = NULL;
            redraw(prompt_rendered, &s);
            continue;
        }
        if (c == 4) { /* Ctrl-D */
            if (s.buf.len == 0) { eof = true; break; }
            delete_at_cursor(&s);
            redraw(prompt_rendered, &s);
            continue;
        }
        if (c == 12) { /* Ctrl-L */
            wr("\x1b[H\x1b[2J", 7);
            redraw(prompt_rendered, &s);
            continue;
        }
        if (c == 127 || c == 8) { backspace(&s); redraw(prompt_rendered, &s); continue; }
        if (c == 1) { s.cursor = 0; redraw(prompt_rendered, &s); continue; }
        if (c == 5) { goto_end_or_accept(&s); redraw(prompt_rendered, &s); continue; }
        if (c == 21) { kill_range(&s, 0, s.cursor); redraw(prompt_rendered, &s); continue; }
        if (c == 11) { kill_range(&s, s.cursor, s.buf.len); redraw(prompt_rendered, &s); continue; }
        if (c == 23) { /* Ctrl-W: delete word backward */
            size_t end = s.cursor;
            size_t i = s.cursor;
            while (i > 0 && isspace((unsigned char)s.buf.data[i-1])) i--;
            while (i > 0 && !isspace((unsigned char)s.buf.data[i-1])) i--;
            kill_range(&s, i, end);
            redraw(prompt_rendered, &s);
            continue;
        }
        if (c == 9) { /* Tab */
            do_complete(&s, prompt_rendered, &cs);
            continue;
        }
        if (c == 20) { /* Ctrl-T: transpose the two characters before the cursor */
            transpose_chars(&s);
            redraw(prompt_rendered, &s);
            continue;
        }
        if (c == 18) { /* Ctrl-R: reverse incremental history search */
            dstr_t query; ds_init(&query);
            dstr_t original; ds_init(&original); ds_append(&original, s.buf.data);
            size_t original_cursor = s.cursor;
            int back = 0;
            const char *match = NULL;
            bool cancelled = false;
            for (;;) {
                dstr_t line; ds_init(&line);
                ds_append(&line, "\r\x1b[K");
                if (!match && query.len > 0) ds_append(&line, "(failed reverse-i-search)`");
                else ds_append(&line, "(reverse-i-search)`");
                ds_append(&line, query.data);
                ds_append(&line, "': ");
                if (match) ds_append(&line, match);
                wr(line.data, line.len);
                ds_free(&line);

                int sc = read_byte();
                if (sc < 0) { cancelled = true; break; }
                if (sc == 18) { /* search again, older */
                    if (query.len > 0) {
                        int nb = history_find_substring(query.data, back);
                        if (nb) { back = nb; match = history_get_relative(back); }
                        else wr("\a", 1);
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
                if (sc == 7 || sc == 27) { cancelled = true; break; } /* Ctrl-G / ESC */
                if (sc == 3) { cancelled = true; break; } /* Ctrl-C */
                if (sc == '\r' || sc == '\n') { break; }
                if (sc >= 32 && sc < 127) {
                    ds_append_c(&query, (char)sc);
                    back = 0; match = NULL;
                    int nb = history_find_substring(query.data, 0);
                    if (nb) { back = nb; match = history_get_relative(nb); }
                    continue;
                }
                break; /* any other key: stop searching, then let the normal loop handle it next time */
            }
            if (cancelled) { ds_clear(&s.buf); ds_append(&s.buf, original.data); s.cursor = original_cursor; }
            else if (match) { ds_clear(&s.buf); ds_append(&s.buf, match); s.cursor = s.buf.len; }
            ds_free(&query); ds_free(&original);
            hist_nav = 0;
            redraw(prompt_rendered, &s);
            continue;
        }
        if (c == 27) { /* ESC sequence */
            int c1 = read_byte();
            if (c1 == '[' || c1 == 'O') {
                int c2 = read_byte();
                if (c2 == 'A') { /* up */
                    if (hist_nav == 0) { free(saved_line); saved_line = xstrdup(s.buf.data); }
                    const char *entry = history_get_relative(hist_nav + 1);
                    if (entry) { hist_nav++; ds_clear(&s.buf); ds_append(&s.buf, entry); s.cursor = s.buf.len; }
                    else wr("\a", 1);
                } else if (c2 == 'B') { /* down */
                    if (hist_nav > 0) {
                        hist_nav--;
                        if (hist_nav == 0) { ds_clear(&s.buf); if (saved_line) ds_append(&s.buf, saved_line); }
                        else { const char *entry = history_get_relative(hist_nav); ds_clear(&s.buf); ds_append(&s.buf, entry ? entry : ""); }
                        s.cursor = s.buf.len;
                    } else wr("\a", 1);
                } else if (c2 == 'C') { if (s.cursor < s.buf.len) s.cursor++; else goto_end_or_accept(&s); }
                else if (c2 == 'D') { if (s.cursor > 0) s.cursor--; }
                else if (c2 == 'H') { s.cursor = 0; }
                else if (c2 == 'F') { goto_end_or_accept(&s); }
                else if (c2 == '1' && c1 == '[') {
                    /* xterm Ctrl-Arrow: ESC [ 1 ; 5 C/D -- word motion */
                    int c3 = read_byte();
                    if (c3 == ';') {
                        int c4 = read_byte();
                        int c5 = read_byte();
                        if (c4 == '5' && c5 == 'C') s.cursor = word_forward_pos(&s);
                        else if (c4 == '5' && c5 == 'D') s.cursor = word_backward_pos(&s);
                    }
                }
                else if (c2 >= '0' && c2 <= '9') {
                    int c3 = read_byte();
                    if (c3 == '~') {
                        if (c2 == '3') delete_at_cursor(&s);
                        else if (c2 == '1' || c2 == '7') s.cursor = 0;
                        else if (c2 == '4' || c2 == '8') goto_end_or_accept(&s);
                    }
                }
                redraw(prompt_rendered, &s);
            } else if (c1 == 'f' || c1 == 'F') { /* Alt-F: word forward */
                s.cursor = word_forward_pos(&s);
                redraw(prompt_rendered, &s);
            } else if (c1 == 'b' || c1 == 'B') { /* Alt-B: word backward */
                s.cursor = word_backward_pos(&s);
                redraw(prompt_rendered, &s);
            } else if (c1 == 'd' || c1 == 'D') { /* Alt-D: delete word forward */
                kill_range(&s, s.cursor, word_forward_pos(&s));
                redraw(prompt_rendered, &s);
            } else if (c1 == 127 || c1 == 8) { /* Alt-Backspace: delete word backward */
                kill_range(&s, word_backward_pos(&s), s.cursor);
                redraw(prompt_rendered, &s);
            }
            continue;
        }
        if (c >= 32) { insert_char(&s, (char)c); redraw(prompt_rendered, &s); continue; }
        /* other control chars: ignore */
    }

    complete_state_reset(&cs);
    raw_off();
    free(saved_line);
    free(prompt_rendered);

    if (eof) { ds_free(&s.buf); return NULL; }
    char *result = xstrdup(s.buf.data);
    ds_free(&s.buf);
    return result;
}
