#include "prompt.h"
#include <ctype.h>
#include "colors.h"
#include "vars.h"
#include "exec.h"
#include "jobs.h"
#include "history.h"
#include "config.h"
#include <unistd.h>
#include <pwd.h>
#include <time.h>

static void append_shortened_cwd(dstr_t *out) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { ds_append(out, "?"); return; }
    const char *home = var_get("HOME");
    if (home && str_has_prefix(cwd, home)) {
        size_t hl = strlen(home);
        if (cwd[hl] == '\0') { ds_append_c(out, '~'); return; }
        if (cwd[hl] == '/') { ds_append_c(out, '~'); ds_append(out, cwd + hl); return; }
    }
    ds_append(out, cwd);
}

static void append_git_branch(dstr_t *out) {
    FILE *f = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
    if (!f) return;
    char buf[256];
    if (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
        if (n > 0) { ds_append_c(out, '('); ds_append(out, buf); ds_append_c(out, ')'); }
    }
    pclose(f);
}

/* (branch*) with * when the working tree has changes */
static void append_git_status(dstr_t *out) {
    FILE *f = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null && git status --porcelain --untracked-files=no 2>/dev/null | head -1", "r");
    if (!f) return;
    char branch[256] = "", line[512];
    bool dirty = false;
    if (fgets(branch, sizeof(branch), f)) {
        size_t n = strlen(branch);
        while (n > 0 && (branch[n-1] == '\n' || branch[n-1] == '\r')) branch[--n] = '\0';
        if (fgets(line, sizeof(line), f)) dirty = true;
    }
    pclose(f);
    if (!branch[0]) return;
    if (strcmp(branch, "HEAD") == 0) {
        FILE *g = popen("git rev-parse --short HEAD 2>/dev/null", "r");
        if (g) { if (fgets(branch, sizeof(branch), g)) { size_t n = strlen(branch); while (n && branch[n-1] == '\n') branch[--n] = 0; } pclose(g); }
    }
    ds_append_c(out, '(');
    ds_append(out, branch);
    if (dirty) ds_append_c(out, '*');
    ds_append(out, ") ");
}

static void append_duration(dstr_t *out) {
    long ms = g_last_duration_ms;
    long threshold = atol(config_get("prompt.duration_threshold", "2000"));
    if (ms < threshold || ms <= 0) return;
    char buf[64];
    if (ms < 60000) snprintf(buf, sizeof(buf), "%.1fs ", (double)ms / 1000.0);
    else if (ms < 3600000) snprintf(buf, sizeof(buf), "%ldm%lds ", ms / 60000, (ms / 1000) % 60);
    else snprintf(buf, sizeof(buf), "%ldh%ldm ", ms / 3600000, (ms / 60000) % 60);
    ds_append(out, buf);
}

static int last_status(void) {
    const char *v = var_get("?");
    return v ? atoi(v) : 0;
}

/* %(X.true.false) conditional; X is ? (last status 0), j (jobs exist),
 * # (root), or a digit N (last status == N). *pp points after "%(" */
static void render_conditional(const char **pp, dstr_t *out);

static void render_into(const char *fmt, dstr_t *out);

char *prompt_render(const char *fmt) {
    dstr_t out; ds_init(&out);
    render_into(fmt, &out);
    char *r = xstrdup(out.data);
    ds_free(&out);
    return r;
}

static void render_conditional(const char **pp, dstr_t *out) {
    const char *p = *pp;
    char cond = *p ? *p++ : '?';
    char sep = *p ? *p++ : '.';
    /* split the two branches, honoring nested %( ) */
    const char *a = p;
    int depth = 0;
    while (*p && !(depth == 0 && *p == sep)) { if (p[0] == '%' && p[1] == '(') depth++; else if (*p == ')' && depth) depth--; p++; }
    const char *a_end = p;
    if (*p) p++;
    const char *b = p;
    depth = 0;
    while (*p && !(depth == 0 && *p == ')')) { if (p[0] == '%' && p[1] == '(') depth++; else if (*p == ')' && depth) depth--; p++; }
    const char *b_end = p;
    if (*p) p++;
    *pp = p;
    bool truth;
    switch (cond) {
        case '?': truth = last_status() == 0; break;
        case 'j': truth = jobs_count() > 0; break;
        case '#': truth = getuid() == 0; break;
        default: truth = isdigit((unsigned char)cond) ? last_status() == cond - '0' : false; break;
    }
    char *branch = truth ? xstrndup(a, (size_t)(a_end - a)) : xstrndup(b, (size_t)(b_end - b));
    render_into(branch, out);
    free(branch);
}

static void render_into(const char *fmt, dstr_t *outp) {
    dstr_t out = *outp;
    const char *p = fmt;
    while (*p) {
        if (*p != '%') { ds_append_c(&out, *p); p++; continue; }
        p++;
        if (!*p) { ds_append_c(&out, '%'); break; }
        switch (*p) {
            case '%': ds_append_c(&out, '%'); p++; break;
            case 'n': {
                const char *u = var_get("USER");
                if (!u) { struct passwd *pw = getpwuid(getuid()); u = pw ? pw->pw_name : "user"; }
                ds_append(&out, u); p++; break;
            }
            case 'm': {
                char host[256];
                if (gethostname(host, sizeof(host)) == 0) {
                    char *dot = strchr(host, '.');
                    if (dot) *dot = '\0';
                    ds_append(&out, host);
                } else ds_append(&out, "host");
                p++; break;
            }
            case '~': append_shortened_cwd(&out); p++; break;
            case 'P': {
                /* fish's prompt_pwd: ~ for $HOME, every directory but the
                 * last shortened to one character (".c" for hidden ones) */
                char cwd[4096];
                if (!getcwd(cwd, sizeof(cwd))) { p++; break; }
                dstr_t full; ds_init(&full);
                const char *home = var_get("HOME");
                size_t hl = home ? strlen(home) : 0;
                if (hl > 1 && strncmp(cwd, home, hl) == 0 && (cwd[hl] == '/' || cwd[hl] == '\0')) { ds_append_c(&full, '~'); ds_append(&full, cwd + hl); }
                else ds_append(&full, cwd);
                const char *s = full.data;
                const char *last = strrchr(s, '/');
                for (const char *q = s; *q; ) {
                    if (last && q < last && *q != '/') {
                        const char *e = strchr(q, '/');
                        size_t keep = (*q == '.' && e - q > 1) ? 2 : 1;
                        ds_append_n(&out, q, keep);
                        q = e;
                        continue;
                    }
                    if (!last || q >= last) { ds_append(&out, q); break; }
                    ds_append_c(&out, *q++);
                }
                ds_free(&full);
                p++;
                break;
            }
            case 'd': {
                char cwd[4096];
                if (getcwd(cwd, sizeof(cwd))) ds_append(&out, cwd);
                p++; break;
            }
            case 'g': append_git_branch(&out); p++; break;
            case '#': ds_append_c(&out, getuid() == 0 ? '#' : '$'); p++; break;
            case 'F': {
                p++;
                if (*p == '{') {
                    p++;
                    const char *start = p;
                    while (*p && *p != '}') p++;
                    char *name = xstrndup(start, (size_t)(p - start));
                    if (*p == '}') p++;
                    char *code = ansi_code_for(name);
                    ds_append(&out, code);
                    free(code); free(name);
                }
                break;
            }
            case 'K': {
                p++;
                if (*p == '{') {
                    p++;
                    const char *start = p;
                    while (*p && *p != '}') p++;
                    char *name = xstrndup(start, (size_t)(p - start));
                    if (*p == '}') p++;
                    char *code = ansi_code_for(name);
                    /* turn the foreground code into a background one */
                    char *m;
                    if ((m = strstr(code, "38;"))) m[0] = '4';
                    else if (code[0]) {
                        for (char *q = code + 2; *q; q++) {
                            if (q[0] == '3' && isdigit((unsigned char)q[1]) && (q[2] == 'm' || q[2] == ';') && (q[-1] == '[' || q[-1] == ';')) { q[0] = '4'; break; }
                            if (q[0] == '9' && isdigit((unsigned char)q[1]) && (q[2] == 'm' || q[2] == ';') && (q[-1] == '[' || q[-1] == ';')) {
                                size_t off = (size_t)(q - code);
                                dstr_t d; ds_init(&d); ds_append_n(&d, code, off); ds_append(&d, "10"); ds_append(&d, q + 1);
                                free(code); code = xstrdup(d.data); ds_free(&d);
                                break;
                            }
                        }
                    }
                    ds_append(&out, code);
                    free(code); free(name);
                }
                break;
            }
            case 'k': ds_append(&out, "\x1b[49m"); p++; break;
            case 'B': ds_append(&out, "\x1b[1m"); p++; break;
            case 'b': ds_append(&out, "\x1b[22m"); p++; break;
            case 'U': ds_append(&out, "\x1b[4m"); p++; break;
            case 'u': ds_append(&out, "\x1b[24m"); p++; break;
            case 'f': ds_append(&out, ANSI_RESET); p++; break;
            case '?': { char b[16]; snprintf(b, sizeof(b), "%d", last_status()); ds_append(&out, b); p++; break; }
            case 'E': { int st = last_status(); if (st) { char b[32]; snprintf(b, sizeof(b), "\x1b[31m✘ %d\x1b[0m ", st); ds_append(&out, b); } p++; break; }
            case 'j': { char b[16]; snprintf(b, sizeof(b), "%d", jobs_count()); ds_append(&out, b); p++; break; }
            case 'J': { int n = jobs_count(); if (n) { char b[32]; snprintf(b, sizeof(b), "✦%d ", n); ds_append(&out, b); } p++; break; }
            case 'c': append_duration(&out); p++; break;
            case 'G': append_git_status(&out); p++; break;
            case 'h': { char b[24]; snprintf(b, sizeof(b), "%zu", history_count() + 1); ds_append(&out, b); p++; break; }
            case 'M': { char host[256]; if (gethostname(host, sizeof(host)) == 0) ds_append(&out, host); p++; break; }
            case '/': { char cwd[4096]; if (getcwd(cwd, sizeof(cwd))) ds_append(&out, cwd); p++; break; }
            case '.': {
                char cwd[4096];
                if (getcwd(cwd, sizeof(cwd))) {
                    const char *home = var_get("HOME");
                    if (home && strcmp(cwd, home) == 0) ds_append_c(&out, '~');
                    else { const char *b = strrchr(cwd, '/'); ds_append(&out, b && b[1] ? b + 1 : cwd); }
                }
                p++; break;
            }
            case 'v': {
                const char *venv = var_get("VIRTUAL_ENV");
                if (venv && *venv) { const char *b = strrchr(venv, '/'); ds_append_c(&out, '('); ds_append(&out, b ? b + 1 : venv); ds_append(&out, ") "); }
                p++; break;
            }
            case 'T': case '*': case 't': {
                char b[32]; time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
                strftime(b, sizeof(b), *p == 'T' ? "%H:%M" : *p == '*' ? "%H:%M:%S" : "%I:%M %p", &tmv);
                ds_append(&out, b); p++; break;
            }
            case 'D': {
                p++;
                char fmtbuf[128] = "%y-%m-%d";
                if (*p == '{') {
                    p++;
                    const char *start = p;
                    while (*p && *p != '}') p++;
                    snprintf(fmtbuf, sizeof(fmtbuf), "%.*s", (int)(p - start), start);
                    if (*p == '}') p++;
                }
                char b[256]; time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
                strftime(b, sizeof(b), fmtbuf, &tmv);
                ds_append(&out, b);
                break;
            }
            case '(': p++; render_conditional(&p, &out); break;
            default: ds_append_c(&out, '%'); ds_append_c(&out, *p); p++; break;
        }
    }
    *outp = out;
}

char *prompt_render_continuation(void) {
    const char *fmt = config_get("prompt.continuation", "%F{brightblack}  …%f ");
    return prompt_render(fmt);
}
