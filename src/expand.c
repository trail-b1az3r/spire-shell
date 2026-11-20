#include "expand.h"
#include "vars.h"
#include "exec.h"
#include "arith.h"
#include "arrays.h"
#include <ctype.h>
#include <glob.h>
#include <fnmatch.h>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>

static char *cmdsubst_run(const char *src) {
    char *out = NULL;
    exec_capture_stdout(src, &out);
    return out ? out : xstrdup("");
}

/* *pp points at '(' ; returns content between matching parens (exclusive),
 * advances *pp past the closing ')'. Respects nested parens/quotes. */
static char *scan_balanced_parens(const char **pp) {
    const char *p = *pp;
    p++;
    int depth = 1;
    dstr_t buf; ds_init(&buf);
    bool insq = false, indq = false;
    while (*p && depth > 0) {
        char c = *p;
        if (c == '\\' && !insq) {
            ds_append_c(&buf, c); p++;
            if (*p) { ds_append_c(&buf, *p); p++; }
            continue;
        }
        if (c == '\'' && !indq) { insq = !insq; ds_append_c(&buf, c); p++; continue; }
        if (c == '"' && !insq) { indq = !indq; ds_append_c(&buf, c); p++; continue; }
        if (c == '(' && !insq) { depth++; ds_append_c(&buf, c); p++; continue; }
        if (c == ')' && !insq) {
            depth--;
            p++;
            if (depth == 0) break;
            ds_append_c(&buf, c);
            continue;
        }
        ds_append_c(&buf, c); p++;
    }
    *pp = p;
    char *res = xstrdup(buf.data);
    ds_free(&buf);
    return res;
}

/* *pp points at '{'; content up to the matching '}' (nesting- and
 * quote-aware, so `${x:-${y:-z}}` and `${x/"}"/a}` work). */
static char *scan_balanced_braces(const char **pp) {
    const char *p = *pp;
    p++;
    const char *start = p;
    int depth = 1;
    bool insq = false, indq = false;
    while (*p) {
        char c = *p;
        if (c == '\\' && !insq && p[1]) { p += 2; continue; }
        if (c == '\'' && !indq) insq = !insq;
        else if (c == '"' && !insq) indq = !indq;
        else if (!insq && c == '{' && p > start && p[-1] == '$') depth++;
        else if (!insq && !indq && c == '}') { depth--; if (depth == 0) break; }
        p++;
    }
    char *res = xstrndup(start, (size_t)(p - start));
    if (*p == '}') p++;
    *pp = p;
    return res;
}

static char *scan_backtick(const char **pp) {
    const char *p = *pp;
    p++; /* opening ` */
    dstr_t buf; ds_init(&buf);
    while (*p && *p != '`') {
        if (*p == '\\' && (p[1] == '`' || p[1] == '\\' || p[1] == '$')) {
            ds_append_c(&buf, p[1]); p += 2; continue;
        }
        ds_append_c(&buf, *p); p++;
    }
    if (*p == '`') p++;
    *pp = p;
    char *res = xstrdup(buf.data);
    ds_free(&buf);
    return res;
}

/* ---------------- ${...} parameter expansion ---------------- */

static size_t utf8_strlen(const char *s) {
    size_t n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

static char *str_case(const char *v, int mode, bool first_only) {
    /* mode: 1 upper, -1 lower, 0 toggle */
    char *r = xstrdup(v);
    for (char *q = r; *q; q++) {
        unsigned char c = (unsigned char)*q;
        if (mode > 0) *q = (char)toupper(c);
        else if (mode < 0) *q = (char)tolower(c);
        else *q = (char)(isupper(c) ? tolower(c) : toupper(c));
        if (first_only) break;
    }
    return r;
}

/* ${v#pat} ${v##pat} ${v%pat} ${v%%pat} */
static char *remove_pattern(const char *v, const char *pat, bool prefix, bool longest) {
    size_t n = strlen(v);
    char *buf = xmalloc(n + 1);
    if (prefix) {
        for (size_t k = 0; k <= n; k++) {
            size_t i = longest ? n - k : k;
            memcpy(buf, v, i); buf[i] = '\0';
            if (fnmatch(pat, buf, 0) == 0) { free(buf); return xstrdup(v + i); }
        }
    } else {
        for (size_t k = 0; k <= n; k++) {
            size_t i = longest ? k : n - k; /* suffix starts at i */
            if (fnmatch(pat, v + i, 0) == 0) { free(buf); return xstrndup(v, i); }
        }
    }
    free(buf);
    return xstrdup(v);
}

/* longest match of `pat` starting exactly at v[start]; returns its length or -1 */
static long match_at(const char *v, size_t start, const char *pat, bool must_reach_end) {
    size_t n = strlen(v);
    char *buf = xmalloc(n + 1);
    for (size_t end = n; ; end--) {
        if (!must_reach_end || end == n) {
            memcpy(buf, v + start, end - start); buf[end - start] = '\0';
            if (fnmatch(pat, buf, 0) == 0) { free(buf); return (long)(end - start); }
        }
        if (end == start) break;
    }
    free(buf);
    return -1;
}

/* ${v/pat/rep} ${v//pat/rep} ${v/#pat/rep} ${v/%pat/rep} */
static char *substitute(const char *v, const char *pat, const char *rep, char mode) {
    dstr_t d; ds_init(&d);
    size_t n = strlen(v);
    size_t i = 0;
    bool done = false;
    if (!*pat) return xstrdup(v);
    while (i <= n) {
        if (!done && (mode != '#' || i == 0) ) {
            long m = match_at(v, i, pat, mode == '%');
            if (m > 0 || (m == 0 && mode == '%')) {
                ds_append(&d, rep);
                i += (size_t)m;
                if (mode != '/') done = true;
                if (m == 0) { if (i < n) ds_append_c(&d, v[i]); i++; }
                continue;
            }
        }
        if (i < n) ds_append_c(&d, v[i]);
        i++;
    }
    char *r = xstrdup(d.data);
    ds_free(&d);
    return r;
}

static char *quote_for_shell(const char *v) {
    dstr_t d; ds_init(&d);
    ds_append_c(&d, '\'');
    for (; *v; v++) { if (*v == '\'') ds_append(&d, "'\\''"); else ds_append_c(&d, *v); }
    ds_append_c(&d, '\'');
    char *r = xstrdup(d.data); ds_free(&d); return r;
}

/* splits "pat/rep" at the first unescaped, unquoted '/' */
static void split_slash(const char *s, char **pat, char **rep) {
    bool insq = false, indq = false;
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '\'' && !indq) insq = !insq;
        else if (*p == '"' && !insq) indq = !indq;
        else if (*p == '/' && !insq && !indq) { *pat = xstrndup(s, (size_t)(p - s)); *rep = xstrdup(p + 1); return; }
    }
    *pat = xstrdup(s); *rep = xstrdup("");
}

/* applies one operator suffix (`#pat`, `/a/b`, `^^`, `:1:2`, ...) to a value */
static char *apply_value_op(const char *v, const char *op) {
    if (!*op) return xstrdup(v);
    if (op[0] == '#' || op[0] == '%') {
        bool longest = op[1] == op[0];
        char *pat = expand_pattern_single(op + (longest ? 2 : 1));
        char *r = remove_pattern(v, pat, op[0] == '#', longest);
        free(pat);
        return r;
    }
    if (op[0] == '/') {
        char mode = '1';
        const char *body = op + 1;
        if (*body == '/') { mode = '/'; body++; }
        else if (*body == '#') { mode = '#'; body++; }
        else if (*body == '%') { mode = '%'; body++; }
        char *praw, *rraw;
        split_slash(body, &praw, &rraw);
        char *pat = expand_pattern_single(praw);
        char *rep = expand_pattern_single(rraw);
        char *r = substitute(v, pat, rep, mode);
        free(praw); free(rraw); free(pat); free(rep);
        return r;
    }
    if (op[0] == '^') return str_case(v, 1, op[1] != '^');
    if (op[0] == ',') return str_case(v, -1, op[1] != ',');
    if (op[0] == '~') return str_case(v, 0, op[1] != '~');
    if (op[0] == '@') {
        switch (op[1]) {
            case 'Q': return quote_for_shell(v);
            case 'U': return str_case(v, 1, false);
            case 'L': return str_case(v, -1, false);
            case 'u': return str_case(v, 1, true);
            default: return xstrdup(v);
        }
    }
    if (op[0] == ':') {
        /* substring ${v:off} / ${v:off:len} */
        const char *rest = op + 1;
        const char *colon = strchr(rest, ':');
        char *offs = colon ? xstrndup(rest, (size_t)(colon - rest)) : xstrdup(rest);
        long n = (long)strlen(v);
        long off = arith_eval(offs);
        free(offs);
        if (off < 0) off += n;
        if (off < 0) off = 0;
        if (off > n) off = n;
        long len = n - off;
        if (colon) {
            len = arith_eval(colon + 1);
            if (len < 0) len = (n - off) + len;
            if (len < 0) len = 0;
            if (off + len > n) len = n - off;
        }
        return xstrndup(v + off, (size_t)len);
    }
    return xstrdup(v);
}

static bool is_special_param(char c) {
    return c == '@' || c == '*' || c == '#' || c == '?' || c == '$' || c == '!' || c == '-' || isdigit((unsigned char)c);
}

static char *param_scalar(const char *name) {
    if (strcmp(name, "-") == 0) return xstrdup("");
    if (array_exists(name)) { const char *el = array_get_index(name, 0); return xstrdup(el ? el : ""); }
    const char *v = var_get(name);
    return v ? xstrdup(v) : NULL;
}

static void positional_all(strvec_t *out) {
    for (size_t i = 0; i < exec_positional_count(); i++) sv_push_dup(out, exec_positional_get(i));
}

static bool expand_brace_param(const char *inner, bool quoted, strvec_t *multi, char **single);

/* `inner` is the text between ${ and }. Returns true with *single set,
 * or false with *multi filled (one word per element). */
static bool expand_brace_param(const char *inner, bool quoted, strvec_t *multi, char **single) {
    const char *p = inner;
    bool length = false, indirect = false;
    if (p[0] == '#' && p[1] && p[1] != '}' && !(p[1] == '#' || p[1] == '%' || p[1] == ':' || p[1] == '/')) { length = true; p++; }
    else if (p[0] == '!' && p[1]) { indirect = true; p++; }

    /* name */
    const char *ns = p;
    if (isalpha((unsigned char)*p) || *p == '_') { while (isalnum((unsigned char)*p) || *p == '_') p++; }
    else if (isdigit((unsigned char)*p)) { while (isdigit((unsigned char)*p)) p++; }
    else if (is_special_param(*p)) p++;
    char *name = xstrndup(ns, (size_t)(p - ns));
    if (!*name) { free(name); *single = xstrdup(""); return true; }

    /* subscript */
    char *sub = NULL;
    if (*p == '[') {
        const char *ss = ++p;
        int depth = 1;
        while (*p && depth) { if (*p == '[') depth++; else if (*p == ']') depth--; if (depth) p++; }
        sub = xstrndup(ss, (size_t)(p - ss));
        if (*p == ']') p++;
    }
    const char *op = p;

    if (indirect) {
        /* ${!arr[@]} -> indices; ${!prefix*} -> names; ${!name} -> indirection */
        if (sub && (strcmp(sub, "@") == 0 || strcmp(sub, "*") == 0)) {
            size_t n = array_exists(name) ? array_len(name) : (var_get(name) ? 1 : 0);
            char buf[32];
            for (size_t i = 0; i < n; i++) { snprintf(buf, sizeof(buf), "%zu", i); sv_push_dup(multi, buf); }
            free(name); free(sub);
            return false;
        }
        if (!sub && (strcmp(op, "*") == 0 || strcmp(op, "@") == 0)) {
            strvec_t names; sv_init(&names);
            vars_dump(&names);
            arrays_dump(&names);
            size_t nl = strlen(name);
            for (size_t i = 0; i < names.count; i++)
                if (strncmp(names.items[i], name, nl) == 0) sv_push_dup(multi, names.items[i]);
            sv_free(&names);
            /* sort for stable output */
            for (size_t i = 1; i < multi->count; i++)
                for (size_t j = i; j > 0 && strcmp(multi->items[j-1], multi->items[j]) > 0; j--) {
                    char *t = multi->items[j]; multi->items[j] = multi->items[j-1]; multi->items[j-1] = t;
                }
            free(name);
            return false;
        }
        char *target = param_scalar(name);
        free(name); free(sub);
        if (!target || !*target) { free(target); *single = xstrdup(""); return true; }
        dstr_t d; ds_init(&d); ds_append(&d, target); ds_append(&d, op);
        bool r = expand_brace_param(d.data, quoted, multi, single);
        ds_free(&d); free(target);
        return r;
    }

    /* fetch the value(s) */
    strvec_t vals; sv_init(&vals);
    bool is_list = false, star_join = false, is_set = true;
    if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
        positional_all(&vals);
        is_list = true; star_join = name[0] == '*';
        is_set = vals.count > 0;
    } else if (sub && (strcmp(sub, "@") == 0 || strcmp(sub, "*") == 0)) {
        if (array_exists(name)) array_get_all(name, &vals);
        else { const char *v = var_get(name); if (v) sv_push_dup(&vals, v); }
        is_list = true; star_join = sub[0] == '*';
        is_set = vals.count > 0;
    } else if (sub) {
        const char *el = NULL;
        if (array_exists(name)) el = array_get_index(name, arith_eval(sub));
        else if (arith_eval(sub) == 0) el = var_get(name);
        if (el) sv_push_dup(&vals, el); else is_set = false;
    } else {
        char *v = param_scalar(name);
        if (v) sv_push(&vals, v); else is_set = false;
    }
    free(sub);

    if (length) {
        char buf[32];
        if (is_list) snprintf(buf, sizeof(buf), "%zu", vals.count);
        else snprintf(buf, sizeof(buf), "%zu", vals.count ? utf8_strlen(vals.items[0]) : (size_t)0);
        sv_free(&vals); free(name);
        *single = xstrdup(buf);
        return true;
    }

    /* default / alternate / assign / error operators */
    bool colon = op[0] == ':' && (op[1] == '-' || op[1] == '=' || op[1] == '+' || op[1] == '?');
    char kind = colon ? op[1] : ((op[0] == '-' || op[0] == '=' || op[0] == '+' || op[0] == '?') ? op[0] : 0);
    if (kind) {
        const char *word = op + (colon ? 2 : 1);
        bool empty = !is_set || (colon && (vals.count == 0 || (vals.count == 1 && vals.items[0][0] == '\0')));
        char *result = NULL;
        if (kind == '-') { if (empty) result = expand_pattern_single(word); }
        else if (kind == '=') {
            if (empty) { result = expand_pattern_single(word); var_set(name, result, var_is_exported(name)); }
        } else if (kind == '+') { result = empty ? xstrdup("") : expand_pattern_single(word); }
        else if (kind == '?') {
            if (empty) {
                char *msg = expand_pattern_single(word);
                sh_err("%s: %s\n", name, *msg ? msg : "parameter null or not set");
                free(msg);
                if (!g_shell_is_interactive) { g_should_exit = true; g_exit_code = 1; }
                result = xstrdup("");
            }
        }
        free(name);
        if (result) { sv_free(&vals); *single = result; return true; }
        op = ""; /* value is used as-is */
    } else {
        free(name);
    }

    /* slicing a list: ${arr[@]:1:2}, ${@:2} */
    if (is_list && op[0] == ':') {
        const char *rest = op + 1;
        const char *c2 = strchr(rest, ':');
        char *offs = c2 ? xstrndup(rest, (size_t)(c2 - rest)) : xstrdup(rest);
        long n = (long)vals.count;
        long off = arith_eval(offs);
        free(offs);
        if (star_join == false && op == p && ns[0] == '@') off -= 1; /* ${@:1} starts at $1 */
        else if (ns[0] == '*' && !sub) off -= 1;
        if (off < 0) off += n;
        if (off < 0) off = 0;
        if (off > n) off = n;
        long len = c2 ? arith_eval(c2 + 1) : n - off;
        if (len < 0) len = 0;
        if (off + len > n) len = n - off;
        strvec_t sl; sv_init(&sl);
        for (long i = off; i < off + len; i++) sv_push_dup(&sl, vals.items[i]);
        sv_free(&vals);
        vals = sl;
        op = "";
    }

    for (size_t i = 0; i < vals.count; i++) {
        char *nv = apply_value_op(vals.items[i], op);
        free(vals.items[i]);
        vals.items[i] = nv;
    }

    if (is_list && !(star_join && quoted)) {
        for (size_t i = 0; i < vals.count; i++) sv_push_dup(multi, vals.items[i]);
        sv_free(&vals);
        return false;
    }
    dstr_t d; ds_init(&d);
    for (size_t i = 0; i < vals.count; i++) { if (i) ds_append_c(&d, ' '); ds_append(&d, vals.items[i]); }
    *single = xstrdup(d.data);
    ds_free(&d);
    sv_free(&vals);
    return true;
}

/* fish list indexing after $name: [N], [-1], [2..4]  (1-based). Returns
 * false if the bracket text isn't a fish index (leave it literal). */
static bool parse_fish_index(const char **pp, long *from, long *to) {
    const char *p = *pp;
    if (*p != '[') return false;
    p++;
    char *end;
    long a = strtol(p, &end, 10);
    if (end == p) return false;
    long b = a;
    p = end;
    if (p[0] == '.' && p[1] == '.') {
        p += 2;
        b = strtol(p, &end, 10);
        if (end == p) return false;
        p = end;
    }
    if (*p != ']') return false;
    *pp = p + 1;
    *from = a; *to = b;
    return true;
}

/* *pp points just after '$'. Returns true for a single-value result (with
 * *single set, never NULL), or false with *multi populated instead (one
 * entry per element -- "$@", "${arr[@]}", fish-style $list). Advances *pp
 * past the construct either way; if '$' wasn't followed by anything
 * expandable, *pp is left unchanged and *single = "$". */
static bool expand_dollar(const char **pp, bool quoted, strvec_t *multi, char **single) {
    const char *p = *pp;
    if (p[0] == '(' && p[1] == '(') {
        char *src = scan_balanced_parens(&p);
        *pp = p;
        size_t sl = strlen(src);
        /* scan_balanced_parens on "((x))" yields "(x)": strip the inner pair */
        if (sl >= 2 && src[0] == '(' && src[sl-1] == ')') { src[sl-1] = '\0'; memmove(src, src + 1, sl - 1); }
        char *ex = expand_heredoc_body(src); /* allow $var / $(cmd) inside */
        long v = arith_eval(ex);
        free(ex);
        free(src);
        char buf[32]; snprintf(buf, sizeof(buf), "%ld", v);
        *single = xstrdup(buf);
        return true;
    }
    if (*p == '(') {
        char *src = scan_balanced_parens(&p);
        *pp = p;
        *single = cmdsubst_run(src);
        free(src);
        return true;
    }
    if (*p == '{') {
        char *inner = scan_balanced_braces(&p);
        *pp = p;
        bool r = expand_brace_param(inner, quoted, multi, single);
        free(inner);
        return r;
    }
    if (*p == '@') {
        *pp = p + 1;
        positional_all(multi);
        return false;
    }
    if (*p == '?' || *p == '$' || *p == '#' || *p == '*' || *p == '!' || *p == '-' || (*p >= '0' && *p <= '9')) {
        char name[2] = { *p, '\0' };
        p++;
        *pp = p;
        const char *v = name[0] == '-' ? "" : var_get(name);
        *single = xstrdup(v ? v : "");
        return true;
    }
    if (isalpha((unsigned char)*p) || *p == '_') {
        const char *start = p;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        char *name = xstrndup(start, (size_t)(p - start));
        if (array_exists(name)) {
            /* fish semantics for the brace-less form: $list is every
             * element, $list[2] / $list[-1] / $list[2..3] index from 1 */
            strvec_t all; sv_init(&all);
            array_get_all(name, &all);
            long from, to;
            strvec_t sel; sv_init(&sel);
            if (parse_fish_index(&p, &from, &to)) {
                long n = (long)all.count;
                if (from < 0) from += n + 1;
                if (to < 0) to += n + 1;
                long step = from <= to ? 1 : -1;
                for (long i = from; ; i += step) {
                    if (i >= 1 && i <= n) sv_push_dup(&sel, all.items[i - 1]);
                    if (i == to) break;
                }
            } else {
                for (size_t i = 0; i < all.count; i++) sv_push_dup(&sel, all.items[i]);
            }
            sv_free(&all);
            *pp = p;
            free(name);
            if (quoted) {
                dstr_t d; ds_init(&d);
                for (size_t i = 0; i < sel.count; i++) { if (i) ds_append_c(&d, ' '); ds_append(&d, sel.items[i]); }
                *single = xstrdup(d.data);
                ds_free(&d);
                sv_free(&sel);
                return true;
            }
            for (size_t i = 0; i < sel.count; i++) sv_push_dup(multi, sel.items[i]);
            sv_free(&sel);
            return false;
        }
        *pp = p;
        const char *v = var_get(name);
        if (!v && g_opt_nounset) {
            sh_err("%s: unbound variable\n", name);
            if (!g_shell_is_interactive) { g_should_exit = true; g_exit_code = 1; }
        }
        *single = xstrdup(v ? v : "");
        free(name);
        return true;
    }
    *single = xstrdup("$"); /* literal '$', *pp left unchanged */
    return true;
}

/* $'...' ANSI-C quoting. *pp points at the opening quote. */
static void ansi_c_quote(const char **pp, dstr_t *cur) {
    const char *p = *pp + 1;
    while (*p && *p != '\'') {
        if (*p != '\\' || !p[1]) { ds_append_c(cur, *p++); continue; }
        p++;
        char c = *p++;
        switch (c) {
            case 'n': ds_append_c(cur, '\n'); break;
            case 't': ds_append_c(cur, '\t'); break;
            case 'r': ds_append_c(cur, '\r'); break;
            case 'a': ds_append_c(cur, '\a'); break;
            case 'b': ds_append_c(cur, '\b'); break;
            case 'f': ds_append_c(cur, '\f'); break;
            case 'v': ds_append_c(cur, '\v'); break;
            case 'e': case 'E': ds_append_c(cur, 27); break;
            case '\\': ds_append_c(cur, '\\'); break;
            case '\'': ds_append_c(cur, '\''); break;
            case '"': ds_append_c(cur, '"'); break;
            case 'x': {
                int v = 0, k = 0;
                while (k < 2 && isxdigit((unsigned char)*p)) { v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10)); p++; k++; }
                ds_append_c(cur, (char)v);
                break;
            }
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                int v = c - '0', k = 1;
                while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; k++; }
                ds_append_c(cur, (char)v);
                break;
            }
            default: ds_append_c(cur, '\\'); ds_append_c(cur, c); break;
        }
    }
    if (*p == '\'') p++;
    *pp = p;
}

/* Splits `val` on IFS whitespace, merging the first fragment into *cur and
 * pushing completed words to *out. Used only for UNQUOTED expansions. */
static void append_split(dstr_t *cur, strvec_t *out, const char *val) {
    if (!*val) return;
    if (isspace((unsigned char)val[0]) && cur->len > 0) {
        sv_push(out, xstrdup(cur->data));
        ds_clear(cur);
    }
    const char *p = val;
    bool first_seg = true;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (!first_seg) { sv_push(out, xstrdup(cur->data)); ds_clear(cur); }
        ds_append_n(cur, start, (size_t)(p - start));
        first_seg = false;
    }
    size_t vlen = strlen(val);
    if (vlen > 0 && isspace((unsigned char)val[vlen - 1])) {
        sv_push(out, xstrdup(cur->data));
        ds_clear(cur);
    }
}

/* Splices array elements in: element 0 merges into *cur, each subsequent
 * element flushes *cur as its own word and starts a fresh one. Used for
 * both quoted and unquoted "${arr[@]}" (each element stays atomic either
 * way, which matches the overwhelmingly common real-world usage). */
static void append_array_multi(dstr_t *cur, strvec_t *out, strvec_t *elems) {
    for (size_t i = 0; i < elems->count; i++) {
        if (i > 0) { sv_push(out, xstrdup(cur->data)); ds_clear(cur); }
        ds_append(cur, elems->items[i]);
    }
}

/* ---------------- brace expansion: {a,b,c} and {1..5}[..step] ---------------- */

/* Splits `body` on top-level (unquoted, unnested) commas. */
static void split_top_level_commas(const char *body, strvec_t *out) {
    size_t n = strlen(body);
    size_t start = 0;
    int depth = 0;
    bool insq = false, indq = false;
    for (size_t i = 0; i < n; i++) {
        char c = body[i];
        if (c == '\\' && !insq) { i++; continue; }
        if (c == '\'' && !indq) { insq = !insq; continue; }
        if (c == '"' && !insq) { indq = !indq; continue; }
        if (insq || indq) continue;
        if (c == '{') depth++;
        else if (c == '}') depth--;
        else if (c == ',' && depth == 0) { sv_push(out, xstrndup(body + start, i - start)); start = i + 1; }
    }
    sv_push(out, xstrndup(body + start, n - start));
}

/* Recognizes "X..Y" or "X..Y..STEP" where X/Y are both integers (optionally
 * zero-padded, which is preserved) or both single letters. */
static bool try_range(const char *body, strvec_t *out) {
    char *copy = xstrdup(body);
    char *p1 = strstr(copy, "..");
    if (!p1) { free(copy); return false; }
    *p1 = '\0';
    char *rest = p1 + 2;
    char *p2 = strstr(rest, "..");
    long step = 1;
    if (p2) { *p2 = '\0'; long s = strtol(p2 + 2, NULL, 10); step = s == 0 ? 1 : labs(s); }

    const char *xs = copy, *ys = rest;
    size_t xl = strlen(xs), yl = strlen(ys);
    bool numeric = xl > 0 && yl > 0;
    for (size_t k = 0; numeric && k < xl; k++)
        if (!isdigit((unsigned char)xs[k]) && !(k == 0 && (xs[k] == '-' || xs[k] == '+'))) numeric = false;
    for (size_t k = 0; numeric && k < yl; k++)
        if (!isdigit((unsigned char)ys[k]) && !(k == 0 && (ys[k] == '-' || ys[k] == '+'))) numeric = false;

    if (numeric) {
        long lo = strtol(xs, NULL, 10), hi = strtol(ys, NULL, 10);
        bool pad = (xl > 1 && xs[0] == '0') || (yl > 1 && ys[0] == '0');
        int width = pad ? (int)(xl > yl ? xl : yl) : 0;
        char buf[40];
        if (lo <= hi) for (long v = lo; v <= hi; v += step) {
            if (pad) snprintf(buf, sizeof(buf), "%0*ld", width, v); else snprintf(buf, sizeof(buf), "%ld", v);
            sv_push_dup(out, buf);
        } else for (long v = lo; v >= hi; v -= step) {
            if (pad) snprintf(buf, sizeof(buf), "%0*ld", width, v); else snprintf(buf, sizeof(buf), "%ld", v);
            sv_push_dup(out, buf);
        }
        free(copy);
        return true;
    }
    if (xl == 1 && yl == 1 && isalpha((unsigned char)xs[0]) && isalpha((unsigned char)ys[0])) {
        char lo = xs[0], hi = ys[0];
        char buf[2] = { 0, 0 };
        if (lo <= hi) for (char v = lo; v <= hi; v = (char)(v + step)) { buf[0] = v; sv_push_dup(out, buf); }
        else for (char v = lo; v >= hi; v = (char)(v - step)) { buf[0] = v; sv_push_dup(out, buf); }
        free(copy);
        return true;
    }
    free(copy);
    return false;
}

/* Finds the first unquoted '{' whose matching '}' delimits a body with a
 * top-level comma or a valid ".." range -- i.e. an actually-expandable
 * brace group, as opposed to a literal "{like this}". */
static bool find_brace_group(const char *s, size_t *open_at, size_t *close_at) {
    size_t n = strlen(s);
    bool insq = false, indq = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' && !insq) { i++; continue; }
        if (c == '\'' && !indq) { insq = !insq; continue; }
        if (c == '"' && !insq) { indq = !indq; continue; }
        if (insq || indq) continue;
        if (c == '{' && i > 0 && s[i-1] == '$') {
            /* ${...} is parameter expansion, never a brace group */
            int d = 1; i++;
            while (i < n && d > 0) { if (s[i] == '{') d++; else if (s[i] == '}') d--; if (d) i++; }
            continue;
        }
        if (c != '{') continue;

        int depth = 1;
        size_t j = i + 1;
        bool has_comma = false, has_dots = false;
        bool jsq = false, jdq = false;
        while (j < n && depth > 0) {
            char cj = s[j];
            if (cj == '\\' && !jsq) { j += 2; continue; }
            if (cj == '\'' && !jdq) { jsq = !jsq; j++; continue; }
            if (cj == '"' && !jsq) { jdq = !jdq; j++; continue; }
            if (!jsq && !jdq) {
                if (cj == '{') depth++;
                else if (cj == '}') { depth--; if (depth == 0) break; }
                else if (cj == ',' && depth == 1) has_comma = true;
                else if (cj == '.' && depth == 1 && j + 1 < n && s[j+1] == '.') has_dots = true;
            }
            j++;
        }
        if (j < n && depth == 0 && (has_comma || has_dots)) { *open_at = i; *close_at = j; return true; }
        i = (j < n) ? j : n - 1; /* skip past this non-expandable group and keep scanning */
    }
    return false;
}

static void brace_expand_rec(const char *s, strvec_t *out, int depth) {
    size_t open_at, close_at;
    if (depth > 8 || !find_brace_group(s, &open_at, &close_at)) { sv_push_dup(out, s); return; }
    char *pre = xstrndup(s, open_at);
    char *body = xstrndup(s + open_at + 1, close_at - open_at - 1);
    char *post = xstrdup(s + close_at + 1);

    strvec_t alts; sv_init(&alts);
    split_top_level_commas(body, &alts);
    if (alts.count <= 1) {
        sv_free(&alts); sv_init(&alts);
        if (!try_range(body, &alts)) { sv_free(&alts); sv_push_dup(out, s); free(pre); free(body); free(post); return; }
    }
    for (size_t i = 0; i < alts.count; i++) {
        dstr_t combined; ds_init(&combined);
        ds_append(&combined, pre); ds_append(&combined, alts.items[i]); ds_append(&combined, post);
        brace_expand_rec(combined.data, out, depth + 1);
        ds_free(&combined);
    }
    sv_free(&alts);
    free(pre); free(body); free(post);
}

/* raw text -> one or more raw texts, expanding {a,b,c} / {1..5} groups.
 * Quote-aware: braces inside '...' or "..." are left untouched, matching
 * bash (brace expansion only sees unquoted braces). */
static void brace_expand(const char *raw, strvec_t *out) { brace_expand_rec(raw, out, 0); }

/* ---------------- ** recursive globbing ---------------- */

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* matches path components comps[ci..] under directory `dir` (whose
 * display prefix is `shown`), "**" matching zero or more directories */
static void globstar_rec(const char *dir, const char *shown, char **comps, size_t ncomps, size_t ci, strvec_t *out, int depth) {
    if (depth > 40) return;
    if (ci == ncomps) { if (*shown) sv_push_dup(out, shown); return; }
    const char *pat = comps[ci];
    bool last = ci + 1 == ncomps;
    if (strcmp(pat, "**") == 0) {
        globstar_rec(dir, shown, comps, ncomps, ci + 1, out, depth); /* zero dirs */
        DIR *d = opendir(*dir ? dir : ".");
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char path[4096], disp[4096];
            snprintf(path, sizeof(path), "%s%s%s", dir, *dir && dir[strlen(dir)-1] != '/' ? "/" : "", e->d_name);
            snprintf(disp, sizeof(disp), "%s%s%s", shown, *shown && shown[strlen(shown)-1] != '/' ? "/" : "", e->d_name);
            struct stat st;
            if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
                globstar_rec(path, disp, comps, ncomps, ci, out, depth + 1);
        }
        closedir(d);
        return;
    }
    DIR *d = opendir(*dir ? dir : ".");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && pat[0] != '.') continue;
        if (fnmatch(pat, e->d_name, 0) != 0) continue;
        char path[4096], disp[4096];
        snprintf(path, sizeof(path), "%s%s%s", dir, *dir && dir[strlen(dir)-1] != '/' ? "/" : "", e->d_name);
        snprintf(disp, sizeof(disp), "%s%s%s", shown, *shown && shown[strlen(shown)-1] != '/' ? "/" : "", e->d_name);
        if (last) { sv_push_dup(out, disp); continue; }
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            globstar_rec(path, disp, comps, ncomps, ci + 1, out, depth + 1);
    }
    closedir(d);
}

static bool globstar(const char *pattern, strvec_t *out) {
    strvec_t comps; sv_init(&comps);
    const char *p = pattern;
    bool absolute = *p == '/';
    while (*p) {
        while (*p == '/') p++;
        const char *s0 = p;
        while (*p && *p != '/') p++;
        if (p == s0) continue;
        char *c = xstrndup(s0, (size_t)(p - s0));
        char *ss = strstr(c, "**");
        if (ss && strcmp(c, "**") != 0) {
            /* fish: `**.txt` = any depth, then `*.txt` */
            sv_push_dup(&comps, "**");
            memmove(ss, ss + 1, strlen(ss));
        }
        sv_push(&comps, c);
    }
    strvec_t found; sv_init(&found);
    globstar_rec(absolute ? "/" : "", absolute ? "/" : "", comps.items, comps.count, 0, &found, 0);
    sv_free(&comps);
    if (found.count == 0) { sv_free(&found); return false; }
    qsort(found.items, found.count, sizeof(char *), cmp_str);
    for (size_t i = 0; i < found.count; i++)
        if (i == 0 || strcmp(found.items[i], found.items[i-1]) != 0) sv_push_dup(out, found.items[i]);
    sv_free(&found);
    return true;
}

static void expand_word_impl(const char *raw, strvec_t *out, bool do_glob) {
    dstr_t cur; ds_init(&cur);
    bool force_emit = false;
    bool glob_eligible = false;
    size_t n = strlen(raw);
    size_t i = 0;

    if (n > 0 && raw[0] == '~' && (n == 1 || raw[1] == '/')) {
        const char *home = var_get("HOME");
        ds_append(&cur, home ? home : "~");
        force_emit = true;
        i = 1;
    } else if (n > 1 && raw[0] == '~' && (isalpha((unsigned char)raw[1]) || raw[1] == '_')) {
        /* ~user and ~user/path */
        size_t e = 1;
        while (e < n && (isalnum((unsigned char)raw[e]) || raw[e] == '_' || raw[e] == '-' || raw[e] == '.')) e++;
        if (e == n || raw[e] == '/') {
            char *user = xstrndup(raw + 1, e - 1);
            struct passwd *pw = getpwnam(user);
            free(user);
            if (pw) { ds_append(&cur, pw->pw_dir); force_emit = true; i = e; }
        }
    } else if (n > 1 && raw[0] == '~' && (raw[1] == '+' || raw[1] == '-') && (n == 2 || raw[2] == '/')) {
        const char *d = var_get(raw[1] == '+' ? "PWD" : "OLDPWD");
        if (d) { ds_append(&cur, d); force_emit = true; i = 2; }
    }

    while (i < n) {
        char c = raw[i];

        if (c == '\'') {
            force_emit = true;
            i++;
            while (i < n && raw[i] != '\'') { ds_append_c(&cur, raw[i]); i++; }
            if (i < n) i++;
            continue;
        }
        if (c == '"') {
            force_emit = true;
            i++;
            while (i < n && raw[i] != '"') {
                if (raw[i] == '\\' && i + 1 < n &&
                    (raw[i+1]=='"' || raw[i+1]=='\\' || raw[i+1]=='$' || raw[i+1]=='`')) {
                    ds_append_c(&cur, raw[i+1]); i += 2; continue;
                }
                if (raw[i] == '$') {
                    const char *p = raw + i + 1;
                    strvec_t multi; sv_init(&multi);
                    char *val = NULL;
                    bool single = expand_dollar(&p, true, &multi, &val);
                    if (single) { ds_append(&cur, val); free(val); }
                    else { append_array_multi(&cur, out, &multi); if (multi.count == 0) force_emit = false; }
                    sv_free(&multi);
                    i = (size_t)(p - raw);
                    continue;
                }
                if (raw[i] == '`') {
                    const char *p = raw + i;
                    char *src = scan_backtick(&p);
                    char *val = cmdsubst_run(src);
                    free(src);
                    ds_append(&cur, val);
                    free(val);
                    i = (size_t)(p - raw);
                    continue;
                }
                ds_append_c(&cur, raw[i]); i++;
            }
            if (i < n) i++;
            continue;
        }
        if (c == '\\') {
            force_emit = true;
            i++;
            if (i < n) { ds_append_c(&cur, raw[i]); i++; }
            continue;
        }
        if (c == '$' && i + 1 < n && raw[i+1] == '\'') {
            const char *p = raw + i + 1;
            ansi_c_quote(&p, &cur);
            force_emit = true;
            i = (size_t)(p - raw);
            continue;
        }
        if ((c == '<' || c == '>') && i + 1 < n && raw[i+1] == '(') {
            const char *p = raw + i + 1;
            char *src = scan_balanced_parens(&p);
            char *path = exec_procsub(src, c == '<');
            ds_append(&cur, path);
            free(path); free(src);
            force_emit = true;
            i = (size_t)(p - raw);
            continue;
        }
        if (c == '$') {
            const char *p = raw + i + 1;
            size_t before = (size_t)(p - raw);
            strvec_t multi; sv_init(&multi);
            char *val = NULL;
            bool single = expand_dollar(&p, false, &multi, &val);
            size_t after = (size_t)(p - raw);
            if (single && after == before) {
                force_emit = true;
                ds_append_c(&cur, '$');
                free(val);
            } else if (single) {
                append_split(&cur, out, val);
                free(val);
            } else {
                append_array_multi(&cur, out, &multi);
            }
            sv_free(&multi);
            i = after;
            continue;
        }
        if (c == '`') {
            const char *p = raw + i;
            char *src = scan_backtick(&p);
            char *val = cmdsubst_run(src);
            free(src);
            append_split(&cur, out, val);
            free(val);
            i = (size_t)(p - raw);
            continue;
        }
        if (c == '(') {
            const char *p = raw + i;
            char *src = scan_balanced_parens(&p);
            char *val = cmdsubst_run(src);
            free(src);
            append_split(&cur, out, val);
            free(val);
            i = (size_t)(p - raw);
            continue;
        }
        if (c == '*' || c == '?' || c == '[') glob_eligible = true;
        force_emit = true;
        ds_append_c(&cur, c);
        i++;
    }

    if (force_emit || cur.len > 0) {
        if (do_glob && glob_eligible && cur.len > 0 && strstr(cur.data, "**")) {
            if (globstar(cur.data, out)) { ds_free(&cur); return; }
        } else if (do_glob && glob_eligible && cur.len > 0) {
            glob_t g;
            int r = glob(cur.data, GLOB_NOCHECK, NULL, &g);
            if (r == 0 && g.gl_pathc > 0) {
                for (size_t k = 0; k < g.gl_pathc; k++) sv_push_dup(out, g.gl_pathv[k]);
                globfree(&g);
                ds_free(&cur);
                return;
            }
            globfree(&g);
        }
        sv_push_dup(out, cur.data);
    }
    ds_free(&cur);
}

void expand_word(const char *raw, strvec_t *out) {
    strvec_t braced; sv_init(&braced);
    brace_expand(raw, &braced);
    for (size_t i = 0; i < braced.count; i++) expand_word_impl(braced.items[i], out, true);
    sv_free(&braced);
}

char *expand_word_single(const char *raw) {
    strvec_t tmp; sv_init(&tmp);
    expand_word(raw, &tmp);
    char *r;
    if (tmp.count == 0) r = xstrdup("");
    else if (tmp.count == 1) r = xstrdup(tmp.items[0]);
    else {
        dstr_t d; ds_init(&d);
        for (size_t i = 0; i < tmp.count; i++) {
            if (i) ds_append_c(&d, ' ');
            ds_append(&d, tmp.items[i]);
        }
        r = xstrdup(d.data);
        ds_free(&d);
    }
    sv_free(&tmp);
    return r;
}

/* Like expand_word_single but never touches the filesystem glob()-wise —
 * used for `case`/`switch` patterns, where "*.txt" is a match pattern
 * (fed to fnmatch), not a file lookup. */
char *expand_pattern_single(const char *raw) {
    strvec_t tmp; sv_init(&tmp);
    expand_word_impl(raw, &tmp, false);
    char *r;
    if (tmp.count == 0) r = xstrdup("");
    else if (tmp.count == 1) r = xstrdup(tmp.items[0]);
    else {
        dstr_t d; ds_init(&d);
        for (size_t i = 0; i < tmp.count; i++) {
            if (i) ds_append_c(&d, ' ');
            ds_append(&d, tmp.items[i]);
        }
        r = xstrdup(d.data);
        ds_free(&d);
    }
    sv_free(&tmp);
    return r;
}

char *expand_heredoc_body(const char *raw) {
    dstr_t cur; ds_init(&cur);
    size_t n = strlen(raw);
    size_t i = 0;
    while (i < n) {
        if (raw[i] == '\\' && i + 1 < n && (raw[i+1] == '$' || raw[i+1] == '`' || raw[i+1] == '\\')) {
            ds_append_c(&cur, raw[i+1]);
            i += 2;
            continue;
        }
        if (raw[i] == '$') {
            const char *p = raw + i + 1;
            strvec_t multi; sv_init(&multi);
            char *val = NULL;
            bool single = expand_dollar(&p, true, &multi, &val);
            if (single) { ds_append(&cur, val); free(val); }
            else {
                for (size_t k = 0; k < multi.count; k++) {
                    if (k) ds_append_c(&cur, ' ');
                    ds_append(&cur, multi.items[k]);
                }
            }
            sv_free(&multi);
            i = (size_t)(p - raw);
            continue;
        }
        if (raw[i] == '`') {
            const char *p = raw + i;
            char *src = scan_backtick(&p);
            char *val = cmdsubst_run(src);
            free(src);
            ds_append(&cur, val);
            free(val);
            i = (size_t)(p - raw);
            continue;
        }
        ds_append_c(&cur, raw[i]);
        i++;
    }
    char *r = xstrdup(cur.data);
    ds_free(&cur);
    return r;
}
