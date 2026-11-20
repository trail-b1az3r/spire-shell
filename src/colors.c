#include "colors.h"
#include <ctype.h>
#include <wchar.h>
#include "vars.h"

typedef struct { const char *name; int code; } NamedColor;

static const NamedColor g_colors[] = {
    {"black", 30}, {"red", 31}, {"green", 32}, {"yellow", 33},
    {"blue", 34}, {"magenta", 35}, {"cyan", 36}, {"white", 37},
    {"brightblack", 90}, {"brightred", 91}, {"brightgreen", 92}, {"brightyellow", 93},
    {"brightblue", 94}, {"brightmagenta", 95}, {"brightcyan", 96}, {"brightwhite", 97},
    {"gray", 90}, {"grey", 90}, {"orange", 208}, {"purple", 35},
    {NULL, 0}
};

/* Color spec grammar: [bold-|dim-|italic-|underline-|reverse-]* NAME, where
 * NAME is a named color, a 256-color index, #rrggbb truecolor, or one of
 * the pure attributes "bold", "underline", "reverse", "italic", "dim". */
char *ansi_code_for(const char *name) {
    if (!name || strcmp(name, "default") == 0 || strcmp(name, "normal") == 0 || name[0] == '\0') return xstrdup("");
    dstr_t params; ds_init(&params);
    const char *n = name;
    for (;;) {
        const char *attrs[][2] = { {"bold-", "1"}, {"dim-", "2"}, {"italic-", "3"}, {"underline-", "4"}, {"reverse-", "7"}, {NULL, NULL} };
        bool hit = false;
        for (int k = 0; attrs[k][0]; k++) {
            if (str_has_prefix(n, attrs[k][0])) {
                if (params.len) ds_append_c(&params, ';');
                ds_append(&params, attrs[k][1]);
                n += strlen(attrs[k][0]);
                hit = true;
            }
        }
        if (!hit) break;
    }
    const char *pure[][2] = { {"bold", "1"}, {"dim", "2"}, {"italic", "3"}, {"underline", "4"}, {"reverse", "7"}, {NULL, NULL} };
    char buf[48] = "";
    bool all_digit = *n != '\0';
    for (const char *p = n; *p; p++) if (!isdigit((unsigned char)*p)) { all_digit = false; break; }
    if (all_digit) snprintf(buf, sizeof(buf), "38;5;%s", n);
    else if (n[0] == '#' && strlen(n) == 7) {
        unsigned r = 0, g = 0, b = 0;
        if (sscanf(n + 1, "%2x%2x%2x", &r, &g, &b) == 3) snprintf(buf, sizeof(buf), "38;2;%u;%u;%u", r, g, b);
    } else {
        for (int i = 0; pure[i][0]; i++) if (strcmp(n, pure[i][0]) == 0) snprintf(buf, sizeof(buf), "%s", pure[i][1]);
        for (int i = 0; !buf[0] && g_colors[i].name; i++) if (strcmp(g_colors[i].name, n) == 0) snprintf(buf, sizeof(buf), "%d", g_colors[i].code);
    }
    if (buf[0]) { if (params.len) ds_append_c(&params, ';'); ds_append(&params, buf); }
    char *r;
    if (params.len) {
        dstr_t d; ds_init(&d);
        ds_append(&d, "\x1b["); ds_append(&d, params.data); ds_append_c(&d, 'm');
        r = xstrdup(d.data);
        ds_free(&d);
    } else r = xstrdup("");
    ds_free(&params);
    return r;
}

void color_wrap_n(dstr_t *out, const char *color_name, const char *text, size_t len) {
    if (len == 0) return;
    char *code = ansi_code_for(color_name);
    if (code[0]) { ds_append(out, code); ds_append_n(out, text, len); ds_append(out, ANSI_RESET); }
    else ds_append_n(out, text, len);
    free(code);
}

void color_wrap(dstr_t *out, const char *color_name, const char *text) {
    color_wrap_n(out, color_name, text, strlen(text));
}

/* ---------------- LS_COLORS ---------------- */

static char *g_ls_src = NULL;      /* copy of the LS_COLORS value we parsed */
static strvec_t g_ls_keys, g_ls_vals;
static bool g_ls_init = false;

static const char *g_ls_default =
    "di=01;34:ln=01;36:ex=01;32:so=01;35:pi=33:bd=01;33:cd=01;33:or=01;31:"
    "*.tar=01;31:*.tgz=01;31:*.gz=01;31:*.zip=01;31:*.xz=01;31:*.bz2=01;31:*.zst=01;31:*.7z=01;31:*.rar=01;31:*.deb=01;31:*.rpm=01;31:"
    "*.jpg=01;35:*.jpeg=01;35:*.png=01;35:*.gif=01;35:*.svg=01;35:*.webp=01;35:*.bmp=01;35:*.ico=01;35:"
    "*.mp4=01;35:*.mkv=01;35:*.webm=01;35:*.mov=01;35:*.mp3=00;36:*.flac=00;36:*.wav=00;36:*.ogg=00;36:"
    "*.md=00;33:*.txt=00;37:*.pdf=00;31:*.json=00;33:*.yml=00;33:*.yaml=00;33:*.toml=00;33:*.c=00;32:*.h=00;32:*.py=00;32:*.rs=00;32:*.go=00;32:*.js=00;32:*.ts=00;32:*.sh=00;32";

static void ls_parse(void) {
    const char *env = getenv("LS_COLORS");
    if (!env || !*env) env = g_ls_default;
    if (g_ls_init && g_ls_src && strcmp(g_ls_src, env) == 0) return;
    if (g_ls_init) { sv_free(&g_ls_keys); sv_free(&g_ls_vals); free(g_ls_src); }
    sv_init(&g_ls_keys); sv_init(&g_ls_vals);
    g_ls_src = xstrdup(env);
    g_ls_init = true;
    char *copy = xstrdup(env);
    for (char *save = NULL, *e = strtok_r(copy, ":", &save); e; e = strtok_r(NULL, ":", &save)) {
        char *eq = strchr(e, '=');
        if (!eq) continue;
        *eq = '\0';
        sv_push_dup(&g_ls_keys, e);
        sv_push_dup(&g_ls_vals, eq + 1);
    }
    free(copy);
}

static const char *ls_get(const char *key) {
    for (size_t i = 0; i < g_ls_keys.count; i++) if (strcmp(g_ls_keys.items[i], key) == 0) return g_ls_vals.items[i];
    return NULL;
}

const char *lscolor_for(const char *name, int kind) {
    ls_parse();
    if (kind == 1) return ls_get("di");
    if (kind == 3) return ls_get("ln");
    if (kind == 2) { const char *v = ls_get("ex"); if (v) return v; }
    size_t nl = strlen(name);
    if (nl && name[nl - 1] == '/') return ls_get("di");
    /* longest matching *.ext / *suffix pattern */
    const char *best = NULL;
    size_t bestlen = 0;
    for (size_t i = 0; i < g_ls_keys.count; i++) {
        const char *k = g_ls_keys.items[i];
        if (k[0] != '*') continue;
        size_t kl = strlen(k + 1);
        if (kl <= nl && kl > bestlen && strcasecmp(name + nl - kl, k + 1) == 0) { best = g_ls_vals.items[i]; bestlen = kl; }
    }
    if (best) return best;
    if (kind == 2) return "01;32";
    return ls_get("fi");
}

/* ---------------- display width ---------------- */

size_t display_width_n(const char *s, size_t n) {
    size_t w = 0, i = 0;
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    while (i < n && s[i]) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            /* skip CSI (ESC [ ... final) and OSC (ESC ] ... BEL/ST) sequences */
            i++;
            if (i < n && s[i] == '[') { i++; while (i < n && s[i] && !(s[i] >= 0x40 && s[i] <= 0x7e)) i++; if (i < n && s[i]) i++; }
            else if (i < n && s[i] == ']') { i++; while (i < n && s[i] && s[i] != '\a' && !(s[i] == 0x1b && s[i + 1] == '\\')) i++; if (i < n && s[i] == '\a') i++; else if (i + 1 < n && s[i]) i += 2; }
            else if (i < n && s[i]) i++;
            continue;
        }
        if (c < 0x80) { if (c >= 32) w++; i++; continue; }
        wchar_t wc;
        size_t r = mbrtowc(&wc, s + i, n - i, &st);
        if (r == (size_t)-1 || r == (size_t)-2 || r == 0) { memset(&st, 0, sizeof(st)); w++; i++; continue; }
        int cw = wcwidth(wc);
        w += cw < 0 ? 1 : (size_t)cw;
        i += r;
    }
    return w;
}

size_t display_width(const char *s) { return display_width_n(s, strlen(s)); }
