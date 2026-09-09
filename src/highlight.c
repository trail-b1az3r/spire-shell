#include "highlight.h"
#include "colors.h"
#include "config.h"
#include "aliases.h"
#include "funcs.h"
#include "builtins.h"
#include "vars.h"
#include <ctype.h>
#include <unistd.h>

static const char *g_keywords[] = {
    "if", "then", "else", "elif", "fi", "end", "for", "while", "until", "do", "done",
    "function", "in", "begin", "case", "esac", "switch", "[[", "]]", "!", NULL
};

static bool is_keyword(const char *w) {
    for (int i = 0; g_keywords[i]; i++) if (strcmp(g_keywords[i], w) == 0) return true;
    return false;
}

/* words that read as flags/operators wherever they appear -- `[[ ]]`/`test`
 * operators and bare `-x`-style option flags -- get their own color instead
 * of whatever color the surrounding position would otherwise imply. */
static const char *g_test_ops[] = {
    "==", "!=", "=~", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
    "-f", "-d", "-e", "-r", "-w", "-x", "-z", "-n", "-s", "-L", "-h",
    "-b", "-c", "-p", "-S", "-nt", "-ot", "-ef", NULL
};

static bool is_test_op(const char *w) {
    for (int i = 0; g_test_ops[i]; i++) if (strcmp(g_test_ops[i], w) == 0) return true;
    return false;
}

/* only called on plain barewords (no quotes/$/\), so a leading '-' reliably
 * means an option flag, not e.g. the start of a quoted string. */
static bool is_option_flag(const char *w, size_t len) {
    return len >= 2 && w[0] == '-';
}

bool command_exists(const char *name) {
    if (!name || !*name) return false;
    if (alias_get(name)) return true;
    if (func_get(name)) return true;
    if (builtin_exists(name)) return true;
    if (strchr(name, '/')) return access(name, X_OK) == 0;
    const char *path = var_get("PATH");
    if (!path) return false;
    char *copy = xstrdup(path);
    bool found = false;
    for (char *dir = strtok(copy, ":"); dir; dir = strtok(NULL, ":")) {
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir, name);
        if (access(full, X_OK) == 0) { found = true; break; }
    }
    free(copy);
    return found;
}

typedef enum { SEG_END, SEG_WS, SEG_WORD, SEG_OP, SEG_COMMENT } SegType;

/* mirrors lexer.c's scan_word but tolerant of unterminated quotes */
static size_t scan_word_len(const char *s, size_t i, size_t n) {
    bool insq = false, indq = false, inbt = false;
    int depth = 0;
    size_t start = i;
    while (i < n) {
        char c = s[i];
        if (!insq && !indq && !inbt && depth == 0) {
            if (isspace((unsigned char)c)) break;
            if (c=='|'||c=='&'||c==';'||c=='<'||c=='>'||c=='#') break;
        }
        if (c == '\\' && !insq) { i++; if (i < n) i++; continue; }
        if (c == '\'' && !indq && !inbt) { insq = !insq; i++; continue; }
        if (c == '"' && !insq && !inbt) { indq = !indq; i++; continue; }
        if (c == '`' && !insq) { inbt = !inbt; i++; continue; }
        if (c == '(' && !insq) { depth++; i++; continue; }
        if (c == ')' && !insq && depth > 0) { depth--; i++; continue; }
        if (c == '{' && !insq && i > start && s[i-1] == '$') { depth++; i++; continue; }
        if (c == '}' && !insq && depth > 0) { depth--; i++; continue; }
        i++;
    }
    return i - start;
}

static SegType next_segment(const char *s, size_t *pos, size_t *seglen) {
    size_t i = *pos, n = strlen(s);
    if (i >= n) return SEG_END;
    char c = s[i];
    if (isspace((unsigned char)c)) {
        size_t start = i;
        while (i < n && isspace((unsigned char)s[i])) i++;
        *seglen = i - start; *pos = i;
        return SEG_WS;
    }
    if (c == '#') {
        size_t start = i;
        while (i < n && s[i] != '\n') i++;
        *seglen = i - start; *pos = i;
        return SEG_COMMENT;
    }
    if (c == '|' || c == '&' || c == ';' || c == '<' || c == '>') {
        size_t start = i;
        if (c == '|') { i++; if (i<n && s[i]=='|') i++; }
        else if (c == '&') { i++; if (i<n && (s[i]=='&'||s[i]=='>')) i++; }
        else if (c == '>') { i++; if (i<n && s[i]=='>') i++; }
        else i++;
        *seglen = i - start; *pos = i;
        return SEG_OP;
    }
    size_t wl = scan_word_len(s, i, n);
    if (wl == 0) wl = 1; /* safety: never stall */
    *seglen = wl; *pos = i + wl;
    return SEG_WORD;
}

static void emit_word(dstr_t *out, const char *word, size_t len, const char *base_class) {
    bool all_digits = len > 0;
    for (size_t k = 0; k < len; k++) if (!isdigit((unsigned char)word[k])) { all_digits = false; break; }
    if (all_digits) { color_wrap_n(out, config_color("number"), word, len); return; }

    size_t i = 0;
    while (i < len) {
        if (word[i] == '\'') {
            size_t start = i; i++;
            while (i < len && word[i] != '\'') i++;
            if (i < len) i++;
            color_wrap_n(out, config_color("string"), word + start, i - start);
            continue;
        }
        if (word[i] == '"') {
            size_t seg_start = i;
            i++;
            while (i < len && word[i] != '"') {
                if (word[i] == '$') {
                    color_wrap_n(out, config_color("string"), word + seg_start, i - seg_start);
                    size_t vstart = i; i++;
                    if (i < len && word[i] == '{') { i++; while (i < len && word[i] != '}') i++; if (i < len) i++; }
                    else { while (i < len && (isalnum((unsigned char)word[i]) || word[i] == '_')) i++; }
                    color_wrap_n(out, config_color("variable"), word + vstart, i - vstart);
                    seg_start = i;
                    continue;
                }
                i++;
            }
            if (i < len) i++;
            color_wrap_n(out, config_color("string"), word + seg_start, i - seg_start);
            continue;
        }
        if (word[i] == '$') {
            size_t vstart = i; i++;
            if (i < len && word[i] == '{') { i++; while (i < len && word[i] != '}') i++; if (i < len) i++; }
            else { while (i < len && (isalnum((unsigned char)word[i]) || word[i] == '_')) i++; }
            color_wrap_n(out, config_color("variable"), word + vstart, i - vstart);
            continue;
        }
        size_t start = i;
        while (i < len && word[i] != '\'' && word[i] != '"' && word[i] != '$' && word[i] != '[') i++;
        color_wrap_n(out, config_color(base_class), word + start, i - start);
        if (i < len && word[i] == '[') {
            size_t bstart = i; i++;
            while (i < len && word[i] != ']') i++;
            if (i < len) i++;
            color_wrap_n(out, config_color("array"), word + bstart, i - bstart);
        }
    }
}

char *highlight_line(const char *buf) {
    dstr_t out; ds_init(&out);
    size_t pos = 0, seglen = 0;
    bool at_cmd_pos = true;

    for (;;) {
        SegType t = next_segment(buf, &pos, &seglen);
        if (t == SEG_END) break;
        const char *seg = buf + (pos - seglen);
        switch (t) {
            case SEG_WS:
                ds_append_n(&out, seg, seglen);
                break;
            case SEG_COMMENT:
                color_wrap_n(&out, config_color("comment"), seg, seglen);
                break;
            case SEG_OP:
                color_wrap_n(&out, config_color("operator"), seg, seglen);
                at_cmd_pos = true;
                break;
            case SEG_WORD: {
                char *word = xstrndup(seg, seglen);
                bool plain_bareword = true;
                for (size_t k = 0; k < seglen; k++)
                    if (seg[k]=='\''||seg[k]=='"'||seg[k]=='$'||seg[k]=='\\') { plain_bareword = false; break; }

                if (at_cmd_pos && plain_bareword && is_keyword(word)) {
                    color_wrap_n(&out, config_color("keyword"), seg, seglen);
                    /* "[[" is followed by a test expression, not a command;
                     * every other keyword stays at command position since
                     * its next word still starts a command/condition. */
                    if (strcmp(word, "[[") == 0) at_cmd_pos = false;
                } else if (at_cmd_pos) {
                    const char *cls;
                    if (plain_bareword && builtin_exists(word)) cls = "builtin";
                    else if (plain_bareword && command_exists(word)) cls = "command";
                    else if (plain_bareword) cls = "command_invalid";
                    else cls = "text";
                    if (plain_bareword) color_wrap_n(&out, config_color(cls), seg, seglen);
                    else emit_word(&out, seg, seglen, "text");
                    at_cmd_pos = false;
                } else if (plain_bareword && is_test_op(word)) {
                    color_wrap_n(&out, config_color("flag"), seg, seglen);
                } else if (plain_bareword && is_option_flag(word, seglen)) {
                    color_wrap_n(&out, config_color("flag"), seg, seglen);
                } else {
                    bool looks_path = seglen > 0 && (seg[0] == '/' ||
                        (seglen > 1 && seg[0] == '.' && seg[1] == '/') ||
                        (seglen > 2 && seg[0] == '.' && seg[1] == '.' && seg[2] == '/') ||
                        (seglen > 1 && seg[0] == '~' && seg[1] == '/'));
                    emit_word(&out, seg, seglen, looks_path ? "path" : "text");
                }
                free(word);
                break;
            }
            case SEG_END: break;
        }
    }
    char *result = xstrdup(out.data);
    ds_free(&out);
    return result;
}
