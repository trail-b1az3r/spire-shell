#include "history.h"
#include <stdio.h>

static char **g_entries = NULL;
static size_t g_count = 0;
static size_t g_cap = 0;
static int g_max = 5000;
static char g_path[1024];

void history_init(int max_entries, const char *path) {
    g_max = max_entries > 0 ? max_entries : 5000;
    snprintf(g_path, sizeof(g_path), "%s", path ? path : "");
    g_cap = 64;
    g_entries = xmalloc(sizeof(char *) * g_cap);
    g_count = 0;

    if (g_path[0]) {
        FILE *f = fopen(g_path, "r");
        if (f) {
            char line[4096];
            while (fgets(line, sizeof(line), f)) {
                size_t n = strlen(line);
                while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
                if (n == 0) continue;
                if (g_count == g_cap) { g_cap *= 2; g_entries = xrealloc(g_entries, sizeof(char *) * g_cap); }
                g_entries[g_count++] = xstrdup(line);
            }
            fclose(f);
        }
    }
    while ((int)g_count > g_max) {
        free(g_entries[0]);
        memmove(g_entries, g_entries + 1, sizeof(char *) * (g_count - 1));
        g_count--;
    }
}

void history_add(const char *line) {
    if (!line || !*line) return;
    if (g_count > 0 && strcmp(g_entries[g_count - 1], line) == 0) return; /* dedupe consecutive dupes */

    if (g_count == g_cap) { g_cap *= 2; g_entries = xrealloc(g_entries, sizeof(char *) * g_cap); }
    g_entries[g_count++] = xstrdup(line);
    while ((int)g_count > g_max) {
        free(g_entries[0]);
        memmove(g_entries, g_entries + 1, sizeof(char *) * (g_count - 1));
        g_count--;
    }
    if (g_path[0]) {
        FILE *f = fopen(g_path, "a");
        if (f) { fprintf(f, "%s\n", line); fclose(f); }
    }
}

void history_save(void) { /* entries are appended incrementally; nothing to flush */ }

size_t history_count(void) { return g_count; }

const char *history_get(size_t index_from_oldest) {
    if (index_from_oldest >= g_count) return NULL;
    return g_entries[index_from_oldest];
}

const char *history_get_relative(int back) {
    if (back < 1 || (size_t)back > g_count) return NULL;
    return g_entries[g_count - back];
}

void history_print(void) {
    for (size_t i = 0; i < g_count; i++) printf("%5zu  %s\n", i + 1, g_entries[i]);
}

const char *history_find_prefix_match(const char *prefix) {
    size_t plen = strlen(prefix);
    if (plen == 0) return NULL;
    for (size_t i = g_count; i > 0; i--) {
        const char *e = g_entries[i - 1];
        if (strncmp(e, prefix, plen) == 0 && strlen(e) > plen) return e;
    }
    return NULL;
}

int history_find_substring(const char *needle, int search_from_back) {
    if (!needle || !*needle) return 0;
    for (int back = search_from_back + 1; (size_t)back <= g_count; back++) {
        const char *e = g_entries[g_count - (size_t)back];
        if (strstr(e, needle)) return back;
    }
    return 0;
}
