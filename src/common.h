#ifndef SPIRE_COMMON_H
#define SPIRE_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

/* ---- allocation helpers (abort on OOM, a shell has no graceful path) ---- */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---- dynamic string buffer ---- */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} dstr_t;

void ds_init(dstr_t *s);
void ds_free(dstr_t *s);
void ds_clear(dstr_t *s);
void ds_append_c(dstr_t *s, char c);
void ds_append(dstr_t *s, const char *str);
void ds_append_n(dstr_t *s, const char *str, size_t n);
void ds_reserve(dstr_t *s, size_t extra);

/* ---- dynamic array of owned strings ---- */
typedef struct {
    char **items;
    size_t count;
    size_t cap;
} strvec_t;

void sv_init(strvec_t *v);
void sv_push(strvec_t *v, char *s);   /* takes ownership of s */
void sv_push_dup(strvec_t *v, const char *s);
void sv_free(strvec_t *v);            /* frees items and backing array */
void sv_clear(strvec_t *v);
char **sv_to_argv(const strvec_t *v); /* NULL terminated, caller does NOT own strings */

/* prints "spire: " (red when stderr is a terminal) followed by the
 * formatted message, which should include its own trailing newline */
void sh_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* misc */
bool str_has_prefix(const char *s, const char *p);
void trim_ws(char *s);

#endif
