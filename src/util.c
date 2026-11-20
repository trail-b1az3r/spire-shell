#include "common.h"
#include <stdarg.h>
#include <ctype.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { sh_err("out of memory\n"); exit(127); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n ? n : 1);
    if (!r) { sh_err("out of memory\n"); exit(127); }
    return r;
}

char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = xmalloc(n);
    memcpy(r, s, n);
    return r;
}

char *xstrndup(const char *s, size_t n) {
    char *r = xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

/* ---------------- dstr ---------------- */

void ds_init(dstr_t *s) {
    s->cap = 32;
    s->len = 0;
    s->data = xmalloc(s->cap);
    s->data[0] = '\0';
}

void ds_free(dstr_t *s) {
    free(s->data);
    s->data = NULL;
    s->len = s->cap = 0;
}

void ds_clear(dstr_t *s) {
    s->len = 0;
    if (s->data) s->data[0] = '\0';
}

void ds_reserve(dstr_t *s, size_t extra) {
    if (s->len + extra + 1 <= s->cap) return;
    while (s->len + extra + 1 > s->cap) s->cap *= 2;
    s->data = xrealloc(s->data, s->cap);
}

void ds_append_c(dstr_t *s, char c) {
    ds_reserve(s, 1);
    s->data[s->len++] = c;
    s->data[s->len] = '\0';
}

void ds_append_n(dstr_t *s, const char *str, size_t n) {
    ds_reserve(s, n);
    memcpy(s->data + s->len, str, n);
    s->len += n;
    s->data[s->len] = '\0';
}

void ds_append(dstr_t *s, const char *str) {
    ds_append_n(s, str, strlen(str));
}

/* ---------------- strvec ---------------- */

void sv_init(strvec_t *v) {
    v->count = 0;
    v->cap = 8;
    v->items = xmalloc(sizeof(char *) * v->cap);
}

void sv_push(strvec_t *v, char *s) {
    if (v->count + 1 > v->cap) {
        v->cap *= 2;
        v->items = xrealloc(v->items, sizeof(char *) * v->cap);
    }
    v->items[v->count++] = s;
}

void sv_push_dup(strvec_t *v, const char *s) {
    sv_push(v, xstrdup(s));
}

void sv_clear(strvec_t *v) {
    for (size_t i = 0; i < v->count; i++) free(v->items[i]);
    v->count = 0;
}

void sv_free(strvec_t *v) {
    sv_clear(v);
    free(v->items);
    v->items = NULL;
    v->cap = 0;
}

char **sv_to_argv(const strvec_t *v) {
    char **argv = xmalloc(sizeof(char *) * (v->count + 1));
    for (size_t i = 0; i < v->count; i++) argv[i] = v->items[i];
    argv[v->count] = NULL;
    return argv;
}

bool str_has_prefix(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}

void trim_ws(char *s) {
    size_t n = strlen(s);
    size_t start = 0;
    while (start < n && isspace((unsigned char)s[start])) start++;
    size_t end = n;
    while (end > start && isspace((unsigned char)s[end - 1])) end--;
    size_t newlen = end - start;
    if (start > 0) memmove(s, s + start, newlen);
    s[newlen] = '\0';
}

void sh_err(const char *fmt, ...) {
    fflush(stdout);
    bool color = isatty(STDERR_FILENO);
    fputs(color ? "\x1b[1;31mspire:\x1b[0m " : "spire: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
