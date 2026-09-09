#include "complete.h"
#include "vars.h"
#include "funcs.h"
#include "aliases.h"
#include "builtins.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

static bool is_word_sep(char c) {
    return isspace((unsigned char)c) || c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}

/* case-insensitive, in-order subsequence test: every character of `pat`
 * must appear in `name` in the same order, though not necessarily
 * contiguously -- fzf-style fuzzy matching, so e.g. "cfgfile" still finds
 * "my_config_file.txt". */
static bool is_subsequence_ci(const char *name, const char *pat) {
    if (!*pat) return true;
    for (; *name; name++) {
        if (tolower((unsigned char)*name) == tolower((unsigned char)*pat)) {
            pat++;
            if (!*pat) return true;
        }
    }
    return false;
}

/* prefix match normally; fuzzy subsequence match as a fallback when
 * nothing matches the plain prefix. */
static bool word_matches(const char *name, const char *word, size_t wlen, bool fuzzy) {
    if (!fuzzy) return strncmp(name, word, wlen) == 0;
    if (wlen == 0) return true;
    return is_subsequence_ci(name, word);
}

static void complete_files(const char *word, strvec_t *out, bool dirs_only, bool fuzzy) {
    const char *slash = strrchr(word, '/');
    char dir[4096], prefix[1024];
    if (slash) {
        size_t dl = (size_t)(slash - word);
        if (dl == 0) snprintf(dir, sizeof(dir), "/");
        else { memcpy(dir, word, dl); dir[dl] = '\0'; }
        snprintf(prefix, sizeof(prefix), "%s", slash + 1);
    } else {
        snprintf(dir, sizeof(dir), ".");
        snprintf(prefix, sizeof(prefix), "%s", word);
    }

    /* `~` / `~/...` as the directory component: list the real home
     * directory, but keep the candidates spelled with a leading "~" so the
     * edited line doesn't suddenly grow the full expanded path. */
    char real_dir[4096];
    const char *opendir_path = dir;
    if (dir[0] == '~' && (dir[1] == '\0' || dir[1] == '/')) {
        const char *home = var_get("HOME");
        if (home) { snprintf(real_dir, sizeof(real_dir), "%s%s", home, dir + 1); opendir_path = real_dir; }
    }

    DIR *d = opendir(opendir_path);
    if (!d) return;
    struct dirent *ent;
    size_t plen = strlen(prefix);
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (plen == 0 && ent->d_name[0] == '.') continue;
        if (!word_matches(ent->d_name, prefix, plen, fuzzy)) continue;

        char statpath[4352];
        snprintf(statpath, sizeof(statpath), "%s/%s", opendir_path, ent->d_name);
        struct stat st;
        bool is_dir = (stat(statpath, &st) == 0 && S_ISDIR(st.st_mode));
        if (dirs_only && !is_dir) continue;

        dstr_t cand; ds_init(&cand);
        if (slash) { ds_append_n(&cand, word, (size_t)(slash - word)); ds_append_c(&cand, '/'); }
        ds_append(&cand, ent->d_name);
        if (is_dir) ds_append_c(&cand, '/');

        sv_push(out, xstrdup(cand.data));
        ds_free(&cand);
    }
    closedir(d);
}

static void complete_commands(const char *prefix, strvec_t *out, bool fuzzy) {
    size_t plen = strlen(prefix);
    strvec_t names; sv_init(&names);
    builtins_list_names(&names);
    func_list_names(&names);
    strvec_t avnames, avvals; sv_init(&avnames); sv_init(&avvals);
    alias_list(&avnames, &avvals);
    for (size_t i = 0; i < avnames.count; i++) sv_push_dup(&names, avnames.items[i]);
    sv_free(&avnames); sv_free(&avvals);
    for (size_t i = 0; i < names.count; i++)
        if (word_matches(names.items[i], prefix, plen, fuzzy)) sv_push_dup(out, names.items[i]);
    sv_free(&names);

    const char *path = var_get("PATH");
    if (!path) return;
    char *copy = xstrdup(path);
    for (char *dir = strtok(copy, ":"); dir; dir = strtok(NULL, ":")) {
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (!word_matches(ent->d_name, prefix, plen, fuzzy)) continue;
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
            if (access(full, X_OK) == 0) sv_push_dup(out, ent->d_name);
        }
        closedir(d);
    }
    free(copy);
}

static void complete_vars(const char *prefix, strvec_t *out, bool fuzzy) {
    strvec_t names; sv_init(&names);
    vars_dump(&names);
    size_t plen = strlen(prefix);
    for (size_t i = 0; i < names.count; i++) {
        if (word_matches(names.items[i], prefix, plen, fuzzy)) {
            dstr_t d; ds_init(&d); ds_append_c(&d, '$'); ds_append(&d, names.items[i]);
            sv_push(out, xstrdup(d.data));
            ds_free(&d);
        }
    }
    sv_free(&names);
}

static void dedup(strvec_t *v) {
    for (size_t i = 0; i < v->count; i++) {
        for (size_t j = v->count - 1; j > i; j--) {
            if (strcmp(v->items[i], v->items[j]) == 0) {
                free(v->items[j]);
                memmove(&v->items[j], &v->items[j+1], sizeof(char *) * (v->count - j - 1));
                v->count--;
            }
        }
    }
}

/* commands whose next bare-word argument is (almost) always a directory */
static bool takes_dir_arg(const char *cmd) {
    static const char *dircmds[] = { "cd", "pushd", "rmdir", NULL };
    for (int i = 0; dircmds[i]; i++) if (strcmp(dircmds[i], cmd) == 0) return true;
    return false;
}

/* Scans backward from `upto` for the start of the current `;`/`|`/`&`
 * separated command segment, then returns that segment's first word (the
 * command name) so completion can be context-aware about it, e.g. offering
 * only directories after `cd`. Caller frees the result. */
static char *current_segment_command(const char *line, size_t upto) {
    size_t seg_start = upto;
    while (seg_start > 0) {
        char c = line[seg_start - 1];
        if (c == '|' || c == '&' || c == ';') break;
        seg_start--;
    }
    size_t i = seg_start;
    while (i < upto && isspace((unsigned char)line[i])) i++;
    size_t ws = i;
    while (i < upto && !is_word_sep(line[i])) i++;
    if (i == ws) return NULL;
    return xstrndup(line + ws, i - ws);
}

static void complete_line_pass(const char *line, size_t cursor, size_t start, const char *word,
                                strvec_t *out, bool fuzzy) {
    if (word[0] == '$') { complete_vars(word + 1, out, fuzzy); return; }

    bool cmd_pos = true;
    size_t back = start;
    while (back > 0 && isspace((unsigned char)line[back - 1])) back--;
    if (back > 0 && !(line[back-1]=='|'||line[back-1]=='&'||line[back-1]==';')) cmd_pos = false;

    if (cmd_pos && !strchr(word, '/')) { complete_commands(word, out, fuzzy); return; }

    bool dirs_only = false;
    if (!cmd_pos) {
        char *segcmd = current_segment_command(line, start);
        if (segcmd) { dirs_only = takes_dir_arg(segcmd); free(segcmd); }
    }
    complete_files(word, out, dirs_only, fuzzy);
    (void)cursor;
}

void complete_line(const char *line, size_t cursor, strvec_t *out, size_t *out_word_start) {
    size_t start = cursor;
    while (start > 0 && !is_word_sep(line[start - 1])) start--;
    *out_word_start = start;
    char *word = xstrndup(line + start, cursor - start);

    complete_line_pass(line, cursor, start, word, out, false);
    dedup(out);

    if (out->count == 0 && word[0] != '\0') {
        complete_line_pass(line, cursor, start, word, out, true);
        dedup(out);
    }

    free(word);
}
