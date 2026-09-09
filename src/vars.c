#include "vars.h"

extern char **environ;

#define VAR_BUCKETS 256

typedef struct VarEntry {
    char *name;
    char *value;
    bool exported;
    struct VarEntry *next;
} VarEntry;

static VarEntry *buckets[VAR_BUCKETS];

static unsigned hash_name(const char *s) {
    unsigned h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)(*s++);
    return h % VAR_BUCKETS;
}

static VarEntry *find_entry(const char *name) {
    unsigned h = hash_name(name);
    for (VarEntry *e = buckets[h]; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

void vars_init(void) {
    for (int i = 0; i < VAR_BUCKETS; i++) buckets[i] = NULL;
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq) continue;
        char *name = xstrndup(*e, (size_t)(eq - *e));
        var_set(name, eq + 1, true);
        free(name);
    }
}

const char *var_get(const char *name) {
    VarEntry *e = find_entry(name);
    return e ? e->value : NULL;
}

void var_set(const char *name, const char *value, bool exported) {
    unsigned h = hash_name(name);
    VarEntry *e = find_entry(name);
    if (!e) {
        e = xmalloc(sizeof(VarEntry));
        e->name = xstrdup(name);
        e->value = NULL;
        e->exported = false;
        e->next = buckets[h];
        buckets[h] = e;
    }
    free(e->value);
    e->value = xstrdup(value ? value : "");
    if (exported) e->exported = true;
    if (e->exported) setenv(e->name, e->value, 1);
}

void var_export(const char *name) {
    VarEntry *e = find_entry(name);
    if (!e) { var_set(name, "", true); return; }
    e->exported = true;
    setenv(e->name, e->value, 1);
}

void var_unset(const char *name) {
    unsigned h = hash_name(name);
    VarEntry **pp = &buckets[h];
    while (*pp) {
        VarEntry *e = *pp;
        if (strcmp(e->name, name) == 0) {
            *pp = e->next;
            if (e->exported) unsetenv(e->name);
            free(e->name);
            free(e->value);
            free(e);
            return;
        }
        pp = &e->next;
    }
}

bool var_is_exported(const char *name) {
    VarEntry *e = find_entry(name);
    return e && e->exported;
}

void vars_dump(strvec_t *names_out) {
    for (int i = 0; i < VAR_BUCKETS; i++)
        for (VarEntry *e = buckets[i]; e; e = e->next)
            sv_push_dup(names_out, e->name);
}

/* ---------------- local variable scoping ---------------- */

typedef struct {
    strvec_t names;
    strvec_t vals;      /* saved old value, or "" when had==0 */
    strvec_t had;        /* "1"/"0": did the name exist before this frame declared it */
    strvec_t exported;   /* "1"/"0": was it exported before */
} LocalFrame;

static LocalFrame *g_frames = NULL;
static int g_frame_count = 0;
static int g_frame_cap = 0;

void var_push_scope(void) {
    if (g_frame_count + 1 > g_frame_cap) {
        g_frame_cap = g_frame_cap ? g_frame_cap * 2 : 8;
        g_frames = xrealloc(g_frames, sizeof(LocalFrame) * (size_t)g_frame_cap);
    }
    LocalFrame *f = &g_frames[g_frame_count++];
    sv_init(&f->names); sv_init(&f->vals); sv_init(&f->had); sv_init(&f->exported);
}

void var_pop_scope(void) {
    if (g_frame_count == 0) return;
    LocalFrame *f = &g_frames[--g_frame_count];
    for (size_t i = f->names.count; i > 0; i--) {
        size_t idx = i - 1;
        if (strcmp(f->had.items[idx], "1") == 0)
            var_set(f->names.items[idx], f->vals.items[idx], strcmp(f->exported.items[idx], "1") == 0);
        else
            var_unset(f->names.items[idx]);
    }
    sv_free(&f->names); sv_free(&f->vals); sv_free(&f->had); sv_free(&f->exported);
}

bool var_declare_local(const char *name) {
    if (g_frame_count == 0) return false;
    LocalFrame *f = &g_frames[g_frame_count - 1];
    for (size_t i = 0; i < f->names.count; i++)
        if (strcmp(f->names.items[i], name) == 0) return true; /* already declared this frame */
    const char *old = var_get(name);
    sv_push_dup(&f->names, name);
    sv_push_dup(&f->vals, old ? old : "");
    sv_push_dup(&f->had, old ? "1" : "0");
    sv_push_dup(&f->exported, var_is_exported(name) ? "1" : "0");
    return true;
}

bool var_in_function_scope(void) { return g_frame_count > 0; }
