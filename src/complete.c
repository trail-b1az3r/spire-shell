/* complete.c - context-aware tab completion.
 *
 * Works out what the word under the cursor is (respecting quotes and
 * escapes) and which command it belongs to, then gathers candidates from:
 *   - user rules registered with the `complete` builtin (fish -c/-s/-l/-a/-d
 *     style, or bash -W/-F/-f/-d style)
 *   - built-in completers for common tools (git, make, ssh, kill, npm, man, ...)
 *   - options parsed from `CMD --help` output (cached; native binaries only)
 *   - commands / variables / users / files as the position demands
 * then filters by prefix, falling back to case-insensitive and fuzzy
 * matching. Every candidate carries a kind (for coloring) and an optional
 * description (shown in the menu). */
#include "complete.h"
#include "vars.h"
#include "arrays.h"
#include "funcs.h"
#include "aliases.h"
#include "builtins.h"
#include "exec.h"
#include "expand.h"
#include "lexer.h"
#include "jobs.h"
#include "config.h"
#include "fishfuncs.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <pwd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

/* ======================= candidate lists ======================= */

void cands_init(CandList *l) { l->items = NULL; l->count = l->cap = 0; }

void cands_free(CandList *l) {
    for (size_t i = 0; i < l->count; i++) { free(l->items[i].text); free(l->items[i].desc); }
    free(l->items);
    cands_init(l);
}

void cands_add(CandList *l, const char *text, const char *desc, CandKind kind) {
    if (!text || !*text) return;
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->items = xrealloc(l->items, sizeof(Cand) * l->cap);
    }
    l->items[l->count].text = xstrdup(text);
    l->items[l->count].desc = (desc && *desc) ? xstrdup(desc) : NULL;
    l->items[l->count].kind = kind;
    l->count++;
}

static int cand_cmp(const void *a, const void *b) {
    const Cand *x = a, *y = b;
    return strcmp(x->text, y->text);
}

static void cands_sort_dedup(CandList *l) {
    if (l->count < 2) return;
    qsort(l->items, l->count, sizeof(Cand), cand_cmp);
    size_t w = 1;
    for (size_t r = 1; r < l->count; r++) {
        if (strcmp(l->items[r].text, l->items[w - 1].text) == 0) {
            /* keep the richer entry (with a description) */
            if (!l->items[w - 1].desc && l->items[r].desc) { free(l->items[w - 1].desc); l->items[w - 1].desc = l->items[r].desc; l->items[r].desc = NULL; }
            free(l->items[r].text); free(l->items[r].desc);
            continue;
        }
        l->items[w++] = l->items[r];
    }
    l->count = w;
}

/* ======================= small helpers ======================= */

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

/* moves the matching candidates of `all` into `out`: exact-case prefix
 * first, then case-insensitive prefix, then fuzzy subsequence */
static void filter_cands(CandList *all, const char *word, CandList *out) {
    size_t wl = strlen(word);
    for (int pass = 0; pass < 3 && out->count == 0; pass++) {
        if (pass == 2 && wl == 0) break;
        for (size_t i = 0; i < all->count; i++) {
            const char *t = all->items[i].text;
            bool ok;
            if (pass == 0) ok = strncmp(t, word, wl) == 0;
            else if (pass == 1) ok = strncasecmp(t, word, wl) == 0;
            else {
                /* fuzzy: match against the last path component only */
                const char *ws = strrchr(word, '/');
                const char *ts = strrchr(t, '/');
                if (ws && ts && (size_t)(ws - word) == (size_t)(ts - t) && strncmp(t, word, (size_t)(ws - word)) == 0)
                    ok = is_subsequence_ci(ts + 1, ws + 1);
                else ok = !ws && is_subsequence_ci(t, word);
            }
            if (!ok) continue;
            /* hidden files only when asked for */
            const char *base = strrchr(t, '/');
            base = base ? base + 1 : t;
            const char *wbase = strrchr(word, '/');
            wbase = wbase ? wbase + 1 : word;
            if ((all->items[i].kind == CK_FILE || all->items[i].kind == CK_DIR || all->items[i].kind == CK_EXEC || all->items[i].kind == CK_LINK)
                && base[0] == '.' && wbase[0] != '.') continue;
            cands_add(out, t, all->items[i].desc, all->items[i].kind);
        }
    }
}

/* runs argv with stdin=/dev/null and stdout+stderr captured, killing it
 * after timeout_ms. Returns malloc'd output (possibly empty), never NULL. */
static char *run_capture(char *const argv[], int timeout_ms, size_t maxbytes) {
    int pfd[2];
    if (pipe(pfd) != 0) return xstrdup("");
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return xstrdup(""); }
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
        signal(SIGTTOU, SIG_DFL); signal(SIGTTIN, SIG_DFL);
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { dup2(dn, 0); close(dn); }
        dup2(pfd[1], 1); dup2(pfd[1], 2);
        close(pfd[0]); close(pfd[1]);
        setenv("PAGER", "cat", 1); setenv("MANPAGER", "cat", 1); setenv("GIT_PAGER", "cat", 1);
        setenv("COLUMNS", "200", 1); setenv("NO_COLOR", "1", 1); setenv("TERM", "dumb", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    dstr_t buf; ds_init(&buf);
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
        long left = timeout_ms - elapsed;
        if (left <= 0) break;
        fd_set fds; FD_ZERO(&fds); FD_SET(pfd[0], &fds);
        struct timeval tv = { left / 1000, (left % 1000) * 1000 };
        int r = select(pfd[0] + 1, &fds, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        char chunk[4096];
        ssize_t n = read(pfd[0], chunk, sizeof(chunk));
        if (n <= 0) break;
        if (buf.len < maxbytes) ds_append_n(&buf, chunk, (size_t)n);
    }
    close(pfd[0]);
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
    char *r = xstrdup(buf.data);
    ds_free(&buf);
    return r;
}

static char *read_small_file(const char *path, size_t max) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    dstr_t d; ds_init(&d);
    char chunk[4096]; size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0 && d.len < max) ds_append_n(&d, chunk, r);
    fclose(f);
    char *s = xstrdup(d.data);
    ds_free(&d);
    return s;
}

static void trunc_desc(char *d, size_t max) {
    size_t n = strlen(d);
    while (n > 0 && isspace((unsigned char)d[n - 1])) d[--n] = '\0';
    if (n > max) { d[max - 3] = '.'; d[max - 2] = '.'; d[max - 1] = '.'; d[max] = '\0'; }
}

/* ======================= basic sources ======================= */

static void add_files(const char *word, CandList *out, bool dirs_only) {
    const char *slash = strrchr(word, '/');
    char dir[4096];
    if (slash) {
        size_t dl = (size_t)(slash - word);
        if (dl == 0) snprintf(dir, sizeof(dir), "/");
        else snprintf(dir, sizeof(dir), "%.*s", (int)dl, word);
    } else snprintf(dir, sizeof(dir), ".");

    /* `~`/`~/x` and `$VAR/x` directories: list the real directory but keep
     * the candidates spelled the way the user typed them */
    char real_dir[4096];
    const char *opendir_path = dir;
    if (dir[0] == '~' && (dir[1] == '\0' || dir[1] == '/')) {
        const char *home = var_get("HOME");
        if (home) { snprintf(real_dir, sizeof(real_dir), "%s%s", home, dir + 1); opendir_path = real_dir; }
    } else if (dir[0] == '~') {
        /* ~user/... */
        const char *s = strchr(dir, '/');
        char user[256];
        snprintf(user, sizeof(user), "%.*s", (int)(s ? (size_t)(s - dir - 1) : strlen(dir) - 1), dir + 1);
        struct passwd *pw = getpwnam(user);
        if (pw) { snprintf(real_dir, sizeof(real_dir), "%s%s", pw->pw_dir, s ? s : ""); opendir_path = real_dir; }
    } else if (strchr(dir, '$')) {
        char *ex = expand_pattern_single(dir);
        snprintf(real_dir, sizeof(real_dir), "%s", ex);
        free(ex);
        opendir_path = real_dir;
    }

    DIR *d = opendir(opendir_path);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char statpath[8300];
        snprintf(statpath, sizeof(statpath), "%s/%s", opendir_path, ent->d_name);
        struct stat st, lst;
        bool ok = stat(statpath, &st) == 0;
        bool is_dir = ok && S_ISDIR(st.st_mode);
        if (dirs_only && !is_dir) continue;
        bool is_link = lstat(statpath, &lst) == 0 && S_ISLNK(lst.st_mode);
        CandKind k = is_dir ? CK_DIR : is_link ? CK_LINK : (ok && (st.st_mode & 0111)) ? CK_EXEC : CK_FILE;

        dstr_t cand; ds_init(&cand);
        if (slash) { ds_append_n(&cand, word, (size_t)(slash - word)); ds_append_c(&cand, '/'); }
        ds_append(&cand, ent->d_name);
        if (is_dir) ds_append_c(&cand, '/');
        cands_add(out, cand.data, NULL, k);
        ds_free(&cand);
    }
    closedir(d);
}

static const char *g_shell_keywords[] = { "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
    "case", "esac", "switch", "function", "begin", "end", "time", "and", "or", "not", NULL };

static void add_path_commands(CandList *out) {
    const char *path = var_get("PATH");
    if (!path) return;
    char *copy = xstrdup(path);
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            char full[8300];
            snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0)
                cands_add(out, ent->d_name, NULL, CK_COMMAND);
        }
        closedir(d);
    }
    free(copy);
}

static void add_commands(CandList *out, bool with_path) {
    strvec_t names; sv_init(&names);
    builtins_list_names(&names);
    for (size_t i = 0; i < names.count; i++) cands_add(out, names.items[i], "builtin", CK_BUILTIN);
    sv_clear(&names);
    func_list_names(&names);
    for (size_t i = 0; i < names.count; i++) {
        Node *fn = func_get(names.items[i]);
        cands_add(out, names.items[i], fn && fn->case_subject ? fn->case_subject : "function", CK_FUNCTION);
    }
    sv_clear(&names);
    strvec_t vals; sv_init(&vals);
    alias_list(&names, &vals);
    for (size_t i = 0; i < names.count; i++) {
        char d[96]; snprintf(d, sizeof(d), "alias: %s", vals.items[i]); trunc_desc(d, 50);
        cands_add(out, names.items[i], d, CK_ALIAS);
    }
    sv_free(&vals);
    sv_clear(&names);
    abbr_list(&names);
    for (size_t i = 0; i < names.count; i++) {
        char d[96]; snprintf(d, sizeof(d), "abbr: %s", abbr_get(names.items[i])); trunc_desc(d, 50);
        cands_add(out, names.items[i], d, CK_ABBR);
    }
    sv_free(&names);
    for (int i = 0; g_shell_keywords[i]; i++) cands_add(out, g_shell_keywords[i], "keyword", CK_KEYWORD);
    if (with_path) add_path_commands(out);
}

static void add_var_names(CandList *out, const char *prefix_text, bool brace) {
    strvec_t names; sv_init(&names);
    vars_dump(&names);
    for (size_t i = 0; i < names.count; i++) {
        const char *n = names.items[i];
        if (!isalpha((unsigned char)n[0]) && n[0] != '_') continue;
        dstr_t t; ds_init(&t);
        ds_append(&t, prefix_text); ds_append(&t, n); if (brace) ds_append_c(&t, '}');
        char d[64];
        const char *v = var_get(n);
        snprintf(d, sizeof(d), "%s", v ? v : "");
        for (char *q = d; *q; q++) if (*q == '\n' || *q == '\t') *q = ' ';
        trunc_desc(d, 40);
        cands_add(out, t.data, d, CK_VAR);
        ds_free(&t);
    }
    sv_clear(&names);
    arrays_dump(&names);
    for (size_t i = 0; i < names.count; i++) {
        dstr_t t; ds_init(&t);
        ds_append(&t, prefix_text); ds_append(&t, names.items[i]); if (brace) ds_append_c(&t, '}');
        char d[32]; snprintf(d, sizeof(d), "list of %zu", array_len(names.items[i]));
        cands_add(out, t.data, d, CK_VAR);
        ds_free(&t);
    }
    sv_free(&names);
}

static void add_plain_var_names(CandList *out) {
    strvec_t names; sv_init(&names);
    vars_dump(&names);
    arrays_dump(&names);
    for (size_t i = 0; i < names.count; i++)
        if (isalpha((unsigned char)names.items[i][0]) || names.items[i][0] == '_') cands_add(out, names.items[i], NULL, CK_VAR);
    sv_free(&names);
}

static void add_users(CandList *out, bool tilde) {
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent())) {
        char t[300];
        snprintf(t, sizeof(t), "%s%s%s", tilde ? "~" : "", pw->pw_name, tilde ? "/" : "");
        cands_add(out, t, pw->pw_gecos && *pw->pw_gecos ? pw->pw_gecos : NULL, tilde ? CK_DIR : CK_ARG);
    }
    endpwent();
}

static void add_jobs(CandList *out) {
    for (Job *j = jobs_head(); j; j = j->next) {
        char t[32]; snprintf(t, sizeof(t), "%%%d", j->id);
        cands_add(out, t, j->cmdline, CK_ARG);
    }
}

static void add_words(CandList *out, const char *const *words, CandKind kind) {
    for (int i = 0; words[i]; i += 2) cands_add(out, words[i], words[i + 1], kind);
}

/* ======================= `complete` rules ======================= */

typedef struct {
    char *cmd;
    char *shortopt, *longopt, *oldopt;
    char *desc;
    char *args;       /* -a / -W word list (expanded at completion time) */
    char *func;       /* -F bash function */
    char *cond;       /* -n condition command */
    bool no_files;    /* -f / -x */
    bool dirs;        /* bash -d */
    bool files;       /* bash -f: force files */
} CompRule;

static CompRule *g_rules = NULL;
static size_t g_nrules = 0, g_caprules = 0;

static void rule_free(CompRule *r) {
    free(r->cmd); free(r->shortopt); free(r->longopt); free(r->oldopt);
    free(r->desc); free(r->args); free(r->func); free(r->cond);
}

static void rules_erase(const char *cmd) {
    size_t w = 0;
    for (size_t i = 0; i < g_nrules; i++) {
        if (strcmp(g_rules[i].cmd, cmd) == 0) { rule_free(&g_rules[i]); continue; }
        g_rules[w++] = g_rules[i];
    }
    g_nrules = w;
}

static void rule_add(CompRule *r) {
    if (g_nrules == g_caprules) { g_caprules = g_caprules ? g_caprules * 2 : 16; g_rules = xrealloc(g_rules, sizeof(CompRule) * g_caprules); }
    g_rules[g_nrules++] = *r;
}

static bool rules_exist_for(const char *cmd) {
    for (size_t i = 0; i < g_nrules; i++) if (strcmp(g_rules[i].cmd, cmd) == 0) return true;
    return false;
}

static void print_rule(const CompRule *r) {
    if (r->func) { printf("complete -F %s %s\n", r->func, r->cmd); return; }
    printf("complete -c %s", r->cmd);
    if (r->cond) printf(" -n '%s'", r->cond);
    if (r->shortopt) printf(" -s %s", r->shortopt);
    if (r->longopt) printf(" -l %s", r->longopt);
    if (r->oldopt) printf(" -o %s", r->oldopt);
    if (r->no_files) printf(" -f");
    if (r->args) printf(" -a '%s'", r->args);
    if (r->desc) printf(" -d '%s'", r->desc);
    printf("\n");
}

/* complete -c CMD [-s X] [-l LONG] [-o OLD] [-a ARGS] [-d DESC] [-n COND] [-f] [-e]
 * complete [-W WORDS] [-F FUNC] [-f] [-d] [-r] [-p] CMD...           (bash) */
int complete_builtin(int argc, char **argv) {
    CompRule r; memset(&r, 0, sizeof(r));
    strvec_t cmds; sv_init(&cmds);
    bool erase = false, print = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define TAKE(field) do { if (!v) goto usage; free(r.field); r.field = xstrdup(v); i++; } while (0)
        if (strcmp(a, "-c") == 0 || strcmp(a, "--command") == 0) { if (!v) goto usage; sv_push_dup(&cmds, v); i++; }
        else if (strcmp(a, "-s") == 0 || strcmp(a, "--short-option") == 0) TAKE(shortopt);
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--long-option") == 0) TAKE(longopt);
        else if (strcmp(a, "-o") == 0 || strcmp(a, "--old-option") == 0) TAKE(oldopt);
        else if (strcmp(a, "-a") == 0 || strcmp(a, "--arguments") == 0 || strcmp(a, "-W") == 0) TAKE(args);
        else if (strcmp(a, "-d") == 0 || strcmp(a, "--description") == 0) {
            /* fish -d DESC vs bash -d (dirs): bash form has no value or is followed by the command */
            if (v && cmds.count > 0) TAKE(desc); else if (v && (i + 2 < argc)) TAKE(desc); else r.dirs = true;
        }
        else if (strcmp(a, "-n") == 0 || strcmp(a, "--condition") == 0) TAKE(cond);
        else if (strcmp(a, "-F") == 0) TAKE(func);
        else if (strcmp(a, "-f") == 0 || strcmp(a, "--no-files") == 0 || strcmp(a, "-x") == 0 || strcmp(a, "--exclusive") == 0) {
            /* fish: -f = no files. bash: -f = files. We treat bare `complete -f CMD` (no -c) as bash. */
            if (cmds.count == 0 && i + 1 < argc && argv[i + 1][0] != '-' && !r.args) r.files = true;
            else r.no_files = true;
        }
        else if (strcmp(a, "-e") == 0 || strcmp(a, "--erase") == 0 || strcmp(a, "-r") == 0) erase = true;
        else if (strcmp(a, "-p") == 0 || strcmp(a, "--print") == 0) print = true;
        else if (strcmp(a, "-r") == 0 || strcmp(a, "--require-parameter") == 0 || strcmp(a, "-k") == 0 || strcmp(a, "--keep-order") == 0) { /* accepted */ }
        else if (a[0] != '-') sv_push_dup(&cmds, a);
#undef TAKE
    }
    if (print || (cmds.count == 0 && !erase)) {
        for (size_t i = 0; i < g_nrules; i++) {
            bool want = cmds.count == 0;
            for (size_t k = 0; k < cmds.count; k++) if (strcmp(cmds.items[k], g_rules[i].cmd) == 0) want = true;
            if (want) print_rule(&g_rules[i]);
        }
        rule_free(&r); sv_free(&cmds);
        return 0;
    }
    if (erase) {
        for (size_t k = 0; k < cmds.count; k++) rules_erase(cmds.items[k]);
        rule_free(&r); sv_free(&cmds);
        return 0;
    }
    for (size_t k = 0; k < cmds.count; k++) {
        CompRule c = r;
        c.cmd = xstrdup(cmds.items[k]);
        c.shortopt = r.shortopt ? xstrdup(r.shortopt) : NULL;
        c.longopt = r.longopt ? xstrdup(r.longopt) : NULL;
        c.oldopt = r.oldopt ? xstrdup(r.oldopt) : NULL;
        c.desc = r.desc ? xstrdup(r.desc) : NULL;
        c.args = r.args ? xstrdup(r.args) : NULL;
        c.func = r.func ? xstrdup(r.func) : NULL;
        c.cond = r.cond ? xstrdup(r.cond) : NULL;
        rule_add(&c);
    }
    rule_free(&r); sv_free(&cmds);
    return 0;
usage:
    sh_err("complete: usage: complete -c CMD [-s X] [-l LONG] [-a ARGS] [-d DESC] [-n COND] [-f]\n"
           "                complete [-W WORDS] [-F FUNC] [-d] [-f] CMD...\n");
    rule_free(&r); sv_free(&cmds);
    return 2;
}

/* evaluates a -a word list: each lexer word is expanded (so command
 * substitutions and $vars work); fish-style "word\tdescription" lines from
 * a command substitution are split into text + description */
static void add_rule_args(const char *args, const char *desc, CandList *out) {
    Lexer lx; lexer_init(&lx, args);
    for (;;) {
        Token t; lexer_next(&lx, &t);
        if (t.type == TOK_EOF) { token_free(&t); break; }
        if (t.type != TOK_WORD) { token_free(&t); continue; }
        if (t.text[0] == '(' || strstr(t.text, "$(")) {
            char *src = t.text[0] == '(' ? xstrndup(t.text + 1, strlen(t.text) - 2) : NULL;
            char *res = NULL;
            if (src) { exec_capture_stdout(src, &res); free(src); }
            else res = expand_pattern_single(t.text);
            for (char *line = res, *save = NULL, *tok = strtok_r(line, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save)) {
                char *tab = strchr(tok, '\t');
                if (tab) { *tab = '\0'; cands_add(out, tok, tab + 1, CK_ARG); }
                else cands_add(out, tok, desc, CK_ARG);
            }
            free(res);
        } else {
            strvec_t w; sv_init(&w);
            expand_word(t.text, &w);
            for (size_t i = 0; i < w.count; i++) cands_add(out, w.items[i], desc, CK_ARG);
            sv_free(&w);
        }
        token_free(&t);
    }
}

static bool rule_condition_ok(const CompRule *r) {
    if (!r->cond) return true;
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) { dup2(dn, STDOUT_FILENO); close(dn); }
    int st = exec_eval_string(r->cond);
    fflush(stdout);
    if (saved >= 0) { dup2(saved, STDOUT_FILENO); close(saved); }
    return st == 0;
}

typedef struct {
    strvec_t words;     /* dequoted words of the current command, before the cursor word */
    const char *cmd;    /* words.items[0] after skipping prefixes, or NULL */
    size_t argi;        /* index of the word being completed (1 = first argument) */
} CmdCtx;

/* bash -F: run the function with COMP_WORDS/COMP_CWORD set, read COMPREPLY */
static void run_bash_completion_func(const CompRule *r, CmdCtx *ctx, const char *cur, const char *line, CandList *out) {
    Node *fn = func_get(r->func);
    if (!fn) return;
    strvec_t words; sv_init(&words);
    for (size_t i = 0; i < ctx->words.count; i++) sv_push_dup(&words, ctx->words.items[i]);
    sv_push_dup(&words, cur);
    array_set("COMP_WORDS", &words);
    char buf[32];
    snprintf(buf, sizeof(buf), "%zu", words.count - 1); var_set("COMP_CWORD", buf, false);
    var_set("COMP_LINE", line, false);
    snprintf(buf, sizeof(buf), "%zu", strlen(line)); var_set("COMP_POINT", buf, false);
    array_unset("COMPREPLY"); var_unset("COMPREPLY");
    char *fargv[] = { r->func, (char *)ctx->cmd, (char *)cur, words.count >= 2 ? words.items[words.count - 2] : (char *)"", NULL };
    exec_call_function(fn, 4, fargv);
    strvec_t reply; sv_init(&reply);
    if (array_exists("COMPREPLY")) array_get_all("COMPREPLY", &reply);
    else if (var_get("COMPREPLY")) sv_push_dup(&reply, var_get("COMPREPLY"));
    for (size_t i = 0; i < reply.count; i++) cands_add(out, reply.items[i], NULL, CK_ARG);
    sv_free(&reply);
    sv_free(&words);
}

/* returns true if user rules decided the candidate set (no file fallback) */
static bool apply_rules(CmdCtx *ctx, const char *cur, const char *line, CandList *out, bool *want_files) {
    bool any = false, no_files = false;
    for (size_t i = 0; i < g_nrules; i++) {
        CompRule *r = &g_rules[i];
        if (strcmp(r->cmd, ctx->cmd) != 0) continue;
        if (!rule_condition_ok(r)) continue;
        any = true;
        if (r->func) { run_bash_completion_func(r, ctx, cur, line, out); no_files = true; continue; }
        if (r->dirs) { add_files(cur, out, true); no_files = true; continue; }
        if (r->files) { add_files(cur, out, false); no_files = true; continue; }
        if (cur[0] == '-') {
            if (r->shortopt) { char t[64]; snprintf(t, sizeof(t), "-%s", r->shortopt); cands_add(out, t, r->desc, CK_OPTION); }
            if (r->longopt) { char t[256]; snprintf(t, sizeof(t), "--%s", r->longopt); cands_add(out, t, r->desc, CK_OPTION); }
            if (r->oldopt) { char t[256]; snprintf(t, sizeof(t), "-%s", r->oldopt); cands_add(out, t, r->desc, CK_OPTION); }
        }
        if (r->args && !r->shortopt && !r->longopt && !r->oldopt) add_rule_args(r->args, r->desc, out);
        if (r->args && (r->shortopt || r->longopt || r->oldopt) && ctx->words.count > 0) {
            /* option that takes an argument: offer args right after it */
            const char *prev = ctx->words.items[ctx->words.count - 1];
            char s1[64] = "", s2[256] = "";
            if (r->shortopt) snprintf(s1, sizeof(s1), "-%s", r->shortopt);
            if (r->longopt) snprintf(s2, sizeof(s2), "--%s", r->longopt);
            if ((s1[0] && strcmp(prev, s1) == 0) || (s2[0] && strcmp(prev, s2) == 0)) add_rule_args(r->args, NULL, out);
        }
        if (r->no_files) no_files = true;
    }
    *want_files = !no_files;
    return any;
}

/* ======================= --help option parsing ======================= */

typedef struct { char *cmd; CandList opts; } HelpCache;
static HelpCache g_help_cache[64];
static int g_help_n = 0;

static bool is_native_binary(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    unsigned char m[4];
    bool ok = read(fd, m, 4) == 4 && ((m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F') ||
                                       (m[0] == 0xcf && m[1] == 0xfa && m[2] == 0xed && m[3] == 0xfe));
    close(fd);
    return ok;
}

static bool find_in_path(const char *name, char *out, size_t cap) {
    const char *path = var_get("PATH");
    if (!path || strchr(name, '/')) return false;
    char *copy = xstrdup(path);
    bool found = false;
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        snprintf(out, cap, "%s/%s", dir, name);
        if (access(out, X_OK) == 0) { found = true; break; }
    }
    free(copy);
    return found;
}

static bool opt_char(char c) { return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '+' || c == '.'; }

/* scans GNU-style help text: lines like "  -a, --all   do not ignore entries" */
static void parse_help_text(const char *text, CandList *out) {
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        char *line = xstrndup(p, ll);
        p = eol ? eol + 1 : p + ll;

        char *q = line;
        int indent = 0;
        while (*q == ' ' || *q == '\t') { q++; indent++; }
        if (*q != '-' || indent > 30 || q[1] == ' ' || q[1] == '\0') { free(line); continue; }

        strvec_t names; sv_init(&names);
        while (*q == '-') {
            char *s0 = q;
            q++;
            if (*q == '-') q++;
            if (!opt_char(*q)) break;
            while (opt_char(*q)) q++;
            sv_push(&names, xstrndup(s0, (size_t)(q - s0)));
            /* skip an argument spec: =ARG, [=ARG], <ARG>, ' ARG' */
            if (*q == '[') { while (*q && *q != ']') q++; if (*q) q++; }
            if (*q == '=') { while (*q && *q != ' ' && *q != ',') q++; }
            if (*q == ' ' && (q[1] == '<' || (isupper((unsigned char)q[1]) && q[2] != ' '))) {
                q++;
                while (*q && *q != ' ' && *q != ',') q++;
            }
            if (*q == ',') q++;
            while (*q == ' ' && q[1] != ' ') { q++; if (*q != '-') break; }
            if (*q != '-') break;
        }
        while (*q == ' ' || *q == '\t') q++;
        char desc[96];
        snprintf(desc, sizeof(desc), "%s", q);
        trunc_desc(desc, 60);
        for (size_t i = 0; i < names.count; i++) {
            const char *n = names.items[i];
            size_t nl = strlen(n);
            if (nl < 2 || n[nl - 1] == '-' || n[nl - 1] == '.') continue;
            cands_add(out, n, desc[0] ? desc : NULL, CK_OPTION);
        }
        sv_free(&names);
        free(line);
    }
}

static void add_help_options(const char *cmd, CandList *out) {
    /* ll and la wrap ls (like fish's --wraps) */
    if (strcmp(cmd, "ll") == 0 || strcmp(cmd, "la") == 0) cmd = "ls";
    const char *cfg = config_get("complete.parse_help", "yes");
    if (strcmp(cfg, "no") == 0) return;
    for (int i = 0; i < g_help_n; i++) {
        if (strcmp(g_help_cache[i].cmd, cmd) == 0) {
            for (size_t k = 0; k < g_help_cache[i].opts.count; k++)
                cands_add(out, g_help_cache[i].opts.items[k].text, g_help_cache[i].opts.items[k].desc, CK_OPTION);
            return;
        }
    }
    static const char *deny[] = { "reboot", "shutdown", "halt", "poweroff", "init", "telinit", "systemctl", "kill", "killall", "pkill", "su", "sudo", "login", "passwd", NULL };
    for (int i = 0; deny[i]; i++) if (strcmp(deny[i], cmd) == 0) return;
    char path[4096];
    if (!find_in_path(cmd, path, sizeof(path)) || !is_native_binary(path)) return;

    char *argv[] = { path, (char *)"--help", NULL };
    char *text = run_capture(argv, 1000, 512 * 1024);
    CandList opts; cands_init(&opts);
    parse_help_text(text, &opts);
    free(text);
    cands_sort_dedup(&opts);
    if (g_help_n == 64) { free(g_help_cache[0].cmd); cands_free(&g_help_cache[0].opts); memmove(g_help_cache, g_help_cache + 1, sizeof(HelpCache) * 63); g_help_n--; }
    g_help_cache[g_help_n].cmd = xstrdup(cmd);
    g_help_cache[g_help_n].opts = opts;
    g_help_n++;
    for (size_t k = 0; k < opts.count; k++) cands_add(out, opts.items[k].text, opts.items[k].desc, CK_OPTION);
}

/* ======================= tool-specific completers ======================= */

static const char *const g_git_cmds[] = {
    "add", "stage file contents", "am", "apply patches from a mailbox", "bisect", "binary search for a bad commit",
    "blame", "show who changed each line", "branch", "list, create or delete branches", "checkout", "switch branches or restore files",
    "cherry-pick", "apply changes from commits", "clean", "remove untracked files", "clone", "clone a repository",
    "commit", "record changes", "config", "get and set options", "describe", "name a commit from tags",
    "diff", "show changes", "fetch", "download objects and refs", "grep", "search tracked files",
    "init", "create an empty repository", "log", "show commit logs", "merge", "join histories",
    "mv", "move or rename a file", "pull", "fetch and integrate", "push", "update remote refs",
    "rebase", "reapply commits on another base", "reflog", "show reference logs", "remote", "manage remotes",
    "reset", "reset HEAD to a state", "restore", "restore working tree files", "revert", "revert commits",
    "rm", "remove files", "show", "show objects", "stash", "stash away changes", "status", "show working tree status",
    "submodule", "manage submodules", "switch", "switch branches", "tag", "create, list or delete tags",
    "worktree", "manage worktrees", NULL, NULL
};

static void add_lines_from(char *const argv[], CandList *out, const char *desc, CandKind kind) {
    char *text = run_capture(argv, 1500, 256 * 1024);
    for (char *save = NULL, *tok = strtok_r(text, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save)) {
        while (*tok == ' ' || *tok == '*') tok++;
        if (*tok && !strstr(tok, "fatal:") && !strstr(tok, "->")) cands_add(out, tok, desc, kind);
    }
    free(text);
}

static void add_git_refs(CandList *out) {
    char *a1[] = { (char *)"git", (char *)"for-each-ref", (char *)"--format=%(refname:short)", (char *)"refs/heads", NULL };
    add_lines_from(a1, out, "branch", CK_ARG);
    char *a2[] = { (char *)"git", (char *)"for-each-ref", (char *)"--format=%(refname:short)", (char *)"refs/remotes", NULL };
    add_lines_from(a2, out, "remote branch", CK_ARG);
    char *a3[] = { (char *)"git", (char *)"for-each-ref", (char *)"--format=%(refname:short)", (char *)"refs/tags", NULL };
    add_lines_from(a3, out, "tag", CK_ARG);
}

static void add_git_changed_files(CandList *out, bool untracked) {
    char *argv[] = { (char *)"git", (char *)"status", (char *)"--porcelain", (char *)"-uall", NULL };
    char *text = run_capture(argv, 1500, 256 * 1024);
    for (char *save = NULL, *tok = strtok_r(text, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save)) {
        if (strlen(tok) < 4) continue;
        const char *desc = "modified";
        if (tok[0] == '?' ) { if (!untracked) continue; desc = "untracked"; }
        else if (tok[0] == 'A' || tok[1] == 'A') desc = "added";
        else if (tok[0] == 'D' || tok[1] == 'D') desc = "deleted";
        else if (tok[0] == 'R') desc = "renamed";
        else if (tok[0] != ' ' && tok[1] == ' ') desc = "staged";
        char *path = tok + 3;
        char *arrow = strstr(path, " -> ");
        if (arrow) path = arrow + 4;
        cands_add(out, path, desc, CK_FILE);
    }
    free(text);
}

/* returns true if it fully handled the word (no file fallback) */
static bool complete_git(CmdCtx *ctx, const char *cur, CandList *out) {
    /* find the subcommand: first non-option word after `git` */
    const char *sub = NULL;
    size_t subi = 0;
    for (size_t i = 1; i < ctx->words.count; i++) {
        const char *w = ctx->words.items[i];
        if (w[0] == '-') { if (strcmp(w, "-C") == 0 || strcmp(w, "-c") == 0) i++; continue; }
        sub = w; subi = i; break;
    }
    if (!sub) {
        if (cur[0] == '-') return false;
        add_words(out, g_git_cmds, CK_ARG);
        /* user aliases from git config */
        char *argv[] = { (char *)"git", (char *)"config", (char *)"--get-regexp", (char *)"^alias\\.", NULL };
        char *text = run_capture(argv, 1000, 64 * 1024);
        for (char *save = NULL, *tok = strtok_r(text, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save)) {
            if (strncmp(tok, "alias.", 6) != 0) continue;
            char *sp = strchr(tok, ' ');
            if (sp) *sp = '\0';
            char d[96]; snprintf(d, sizeof(d), "alias: %s", sp ? sp + 1 : ""); trunc_desc(d, 50);
            cands_add(out, tok + 6, d, CK_ARG);
        }
        free(text);
        return true;
    }
    if (cur[0] == '-') {
        /* `git sub --<tab>`: options from `git sub -h` */
        char *argv[] = { (char *)"git", (char *)sub, (char *)"-h", NULL };
        char *text = run_capture(argv, 1000, 128 * 1024);
        parse_help_text(text, out);
        free(text);
        return true;
    }
    size_t argn = ctx->words.count - subi; /* 1 = first arg of the subcommand */
    bool after_dashdash = false;
    for (size_t i = subi; i < ctx->words.count; i++) if (strcmp(ctx->words.items[i], "--") == 0) after_dashdash = true;
    if (after_dashdash) return false;
    if (!strcmp(sub, "checkout") || !strcmp(sub, "switch") || !strcmp(sub, "merge") || !strcmp(sub, "rebase") ||
        !strcmp(sub, "cherry-pick") || !strcmp(sub, "log") || !strcmp(sub, "show") || !strcmp(sub, "reset") ||
        !strcmp(sub, "branch") || !strcmp(sub, "diff") || !strcmp(sub, "tag") || !strcmp(sub, "revert") || !strcmp(sub, "describe")) {
        add_git_refs(out);
        if (!strcmp(sub, "diff") || !strcmp(sub, "checkout") || !strcmp(sub, "reset")) add_git_changed_files(out, false);
        return strcmp(sub, "diff") != 0 && strcmp(sub, "log") != 0;
    }
    if (!strcmp(sub, "add") || !strcmp(sub, "restore") || !strcmp(sub, "rm") || !strcmp(sub, "stage")) {
        add_git_changed_files(out, strcmp(sub, "add") == 0 || strcmp(sub, "stage") == 0);
        return out->count > 0;
    }
    if (!strcmp(sub, "push") || !strcmp(sub, "pull") || !strcmp(sub, "fetch")) {
        if (argn == 1) { char *argv[] = { (char *)"git", (char *)"remote", NULL }; add_lines_from(argv, out, "remote", CK_ARG); }
        else add_git_refs(out);
        return true;
    }
    if (!strcmp(sub, "remote")) {
        static const char *const rs[] = { "add", "add a remote", "remove", "remove a remote", "rename", "rename a remote",
            "set-url", "change a remote's URL", "show", "show remote info", "prune", "prune stale branches", "-v", "verbose list", NULL, NULL };
        if (argn == 1) add_words(out, rs, CK_ARG);
        else { char *argv[] = { (char *)"git", (char *)"remote", NULL }; add_lines_from(argv, out, "remote", CK_ARG); }
        return true;
    }
    if (!strcmp(sub, "stash")) {
        static const char *const ss[] = { "push", "stash changes", "pop", "apply and drop", "apply", "apply a stash",
            "list", "list stashes", "show", "show a stash", "drop", "drop a stash", "clear", "drop all stashes", NULL, NULL };
        if (argn == 1) add_words(out, ss, CK_ARG);
        return true;
    }
    return false;
}

static void add_make_targets(CandList *out) {
    const char *files[] = { "GNUmakefile", "makefile", "Makefile", NULL };
    for (int f = 0; files[f]; f++) {
        char *text = read_small_file(files[f], 1024 * 1024);
        if (!text) continue;
        for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (!isalnum((unsigned char)line[0]) && line[0] != '_' && line[0] != '/') continue;
            char *colon = strchr(line, ':');
            if (!colon || colon[1] == '=' || (strchr(line, '=') && strchr(line, '=') < colon)) continue;
            *colon = '\0';
            for (char *s2 = NULL, *t = strtok_r(line, " \t", &s2); t; t = strtok_r(NULL, " \t", &s2))
                if (!strchr(t, '%') && !strchr(t, '$')) cands_add(out, t, "target", CK_ARG);
        }
        free(text);
        break;
    }
}

static void add_ssh_hosts(CandList *out) {
    const char *home = var_get("HOME");
    char path[4096];
    if (home) {
        snprintf(path, sizeof(path), "%s/.ssh/config", home);
        char *text = read_small_file(path, 512 * 1024);
        if (text) {
            for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                while (*line == ' ' || *line == '\t') line++;
                if (strncasecmp(line, "host ", 5) != 0 && strncasecmp(line, "host\t", 5) != 0) continue;
                for (char *s2 = NULL, *t = strtok_r(line + 5, " \t", &s2); t; t = strtok_r(NULL, " \t", &s2))
                    if (!strpbrk(t, "*?!")) cands_add(out, t, "ssh config", CK_ARG);
            }
            free(text);
        }
        snprintf(path, sizeof(path), "%s/.ssh/known_hosts", home);
        text = read_small_file(path, 1024 * 1024);
        if (text) {
            for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                if (line[0] == '|' || line[0] == '#' || line[0] == '@') continue;
                char *sp = strchr(line, ' ');
                if (sp) *sp = '\0';
                for (char *s2 = NULL, *h = strtok_r(line, ",", &s2); h; h = strtok_r(NULL, ",", &s2)) {
                    if (h[0] == '[') { h++; char *rb = strchr(h, ']'); if (rb) *rb = '\0'; }
                    cands_add(out, h, "known host", CK_ARG);
                }
            }
            free(text);
        }
    }
    char *text = read_small_file("/etc/hosts", 256 * 1024);
    if (text) {
        for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (line[0] == '#') continue;
            char *s2 = NULL;
            char *t = strtok_r(line, " \t", &s2); /* address */
            for (t = strtok_r(NULL, " \t", &s2); t && t[0] != '#'; t = strtok_r(NULL, " \t", &s2)) cands_add(out, t, "/etc/hosts", CK_ARG);
        }
        free(text);
    }
}

static void add_processes(CandList *out) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    uid_t me = getuid();
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[300];
        snprintf(path, sizeof(path), "/proc/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || (me != 0 && st.st_uid != me)) continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        char *comm = read_small_file(path, 256);
        if (!comm) continue;
        size_t n = strlen(comm);
        while (n && comm[n - 1] == '\n') comm[--n] = '\0';
        cands_add(out, e->d_name, comm, CK_ARG);
        free(comm);
    }
    closedir(d);
}

static void add_npm_scripts(CandList *out) {
    char *text = read_small_file("package.json", 1024 * 1024);
    if (!text) return;
    char *s = strstr(text, "\"scripts\"");
    if (s && (s = strchr(s, '{'))) {
        s++;
        while (*s && *s != '}') {
            char *k = strchr(s, '"');
            if (!k) break;
            char *ke = strchr(k + 1, '"');
            if (!ke) break;
            char *colon = strchr(ke, ':');
            char *v = colon ? strchr(colon, '"') : NULL;
            char *ve = v ? v + 1 : NULL;
            while (ve && *ve && *ve != '"') { if (*ve == '\\' && ve[1]) ve++; ve++; }
            if (!ve || !*ve) break;
            char *key = xstrndup(k + 1, (size_t)(ke - k - 1));
            char desc[96]; snprintf(desc, sizeof(desc), "%.*s", (int)(ve - v - 1), v + 1); trunc_desc(desc, 50);
            cands_add(out, key, desc, CK_ARG);
            free(key);
            s = ve + 1;
            while (*s && *s != ',' && *s != '}') s++;
            if (*s == ',') s++;
        }
    }
    free(text);
}

static const char *const g_colorschemes[] = { "dark", "default dark palette", "light", "for light terminals", "mono", "no colors",
    "solarized", "solarized", "nord", "nord", "gruvbox", "gruvbox", "dracula", "dracula", "list", "show presets", NULL, NULL };

static const char *const g_string_subs[] = { "length", "string length", "lower", "lowercase", "upper", "uppercase", "trim", "strip whitespace",
    "join", "join with separator", "split", "split by separator", "sub", "substring", "repeat", "repeat a string", "escape", "shell-escape",
    "replace", "replace text", "match", "match a pattern", "pad", "pad to width", NULL, NULL };

static const char *const g_status_subs[] = { "is-interactive", "interactive shell?", "is-login", "login shell?", "is-command-substitution", "in $(...)?",
    "current-command", "running command", "filename", "current script", NULL, NULL };

static void add_signals(CandList *out, bool dash) {
    static const char *sigs[] = { "HUP", "INT", "QUIT", "KILL", "TERM", "USR1", "USR2", "STOP", "CONT", "TSTP", "ALRM", "CHLD", "PIPE", "WINCH", NULL };
    for (int i = 0; sigs[i]; i++) { char t[16]; snprintf(t, sizeof(t), "%s%s", dash ? "-" : "", sigs[i]); cands_add(out, t, "signal", CK_ARG); }
}

/* built-in knowledge about common commands. Returns true when the result
 * should not fall back to files. */
static bool complete_known_command(CmdCtx *ctx, const char *cur, CandList *out, bool *dirs_only) {
    const char *c = ctx->cmd;
    if (!strcmp(c, "cd") || !strcmp(c, "pushd") || !strcmp(c, "rmdir") || !strcmp(c, "mkdir")) {
        *dirs_only = true;
        if (!strcmp(c, "cd") && !strchr(cur, '/')) {
            /* CDPATH entries */
            const char *cdpath = var_get("CDPATH");
            if (cdpath) {
                char *copy = xstrdup(cdpath);
                for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
                    DIR *d = opendir(dir);
                    if (!d) continue;
                    struct dirent *e;
                    while ((e = readdir(d))) {
                        if (e->d_name[0] == '.') continue;
                        char p[8300]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
                        struct stat st;
                        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) { char t[300]; snprintf(t, sizeof(t), "%s/", e->d_name); cands_add(out, t, dir, CK_DIR); }
                    }
                    closedir(d);
                }
                free(copy);
            }
        }
        return false;
    }
    if (!strcmp(c, "git")) return complete_git(ctx, cur, out);
    if (!strcmp(c, "make") || !strcmp(c, "gmake")) { if (cur[0] != '-') { add_make_targets(out); return out->count > 0; } return false; }
    if (!strcmp(c, "ssh") || !strcmp(c, "sftp") || !strcmp(c, "mosh") || !strcmp(c, "ping") || !strcmp(c, "telnet") || !strcmp(c, "scp") || !strcmp(c, "rsync")) {
        if (cur[0] == '-') return false;
        if ((!strcmp(c, "scp") || !strcmp(c, "rsync")) && (strchr(cur, '/') || cur[0] == '.')) return false;
        add_ssh_hosts(out);
        if (!strcmp(c, "scp") || !strcmp(c, "rsync")) {
            /* also local files; hosts get a ':' */
            for (size_t i = 0; i < out->count; i++) {
                size_t n = strlen(out->items[i].text);
                out->items[i].text = xrealloc(out->items[i].text, n + 2);
                out->items[i].text[n] = ':'; out->items[i].text[n + 1] = '\0';
            }
            return false;
        }
        return true;
    }
    if (!strcmp(c, "kill")) {
        if (cur[0] == '-') add_signals(out, true);
        else { add_jobs(out); add_processes(out); }
        return true;
    }
    if (!strcmp(c, "killall") || !strcmp(c, "pkill")) {
        CandList procs; cands_init(&procs);
        add_processes(&procs);
        for (size_t i = 0; i < procs.count; i++) if (procs.items[i].desc) cands_add(out, procs.items[i].desc, "process", CK_ARG);
        cands_free(&procs);
        return true;
    }
    if (!strcmp(c, "trap")) { if (ctx->argi >= 2) { add_signals(out, false); cands_add(out, "EXIT", "on shell exit", CK_ARG); cands_add(out, "ERR", "on command failure", CK_ARG); return true; } return false; }
    if (!strcmp(c, "npm") || !strcmp(c, "yarn") || !strcmp(c, "pnpm") || !strcmp(c, "bun")) {
        if (ctx->argi == 1 && cur[0] != '-') {
            static const char *const nc[] = { "install", "install dependencies", "run", "run a script", "test", "run tests", "start", "start the app",
                "build", "build", "add", "add a dependency", "remove", "remove a dependency", "init", "create package.json", "exec", "run a package binary", NULL, NULL };
            add_words(out, nc, CK_ARG);
            if (strcmp(c, "npm") != 0) add_npm_scripts(out);
            return true;
        }
        if (ctx->argi == 2 && ctx->words.count >= 2 && (!strcmp(ctx->words.items[1], "run") || !strcmp(ctx->words.items[1], "run-script"))) { add_npm_scripts(out); return true; }
        return false;
    }
    if (!strcmp(c, "man") || !strcmp(c, "whatis") || !strcmp(c, "which") || !strcmp(c, "type") || !strcmp(c, "command")) {
        if (cur[0] == '-') return false;
        add_commands(out, true);
        return true;
    }
    if (!strcmp(c, "help") || !strcmp(c, "builtin")) {
        strvec_t names; sv_init(&names); builtins_list_names(&names);
        for (size_t i = 0; i < names.count; i++) cands_add(out, names.items[i], "builtin", CK_BUILTIN);
        sv_free(&names);
        return true;
    }
    if (!strcmp(c, "export") || !strcmp(c, "unset") || !strcmp(c, "local") || !strcmp(c, "declare") || !strcmp(c, "typeset") ||
        !strcmp(c, "readonly") || !strcmp(c, "read") || !strcmp(c, "set")) {
        if (cur[0] == '-') return false;
        if (!strcmp(c, "set") && ctx->argi > 1) {
            bool opt_only = true;
            for (size_t i = 1; i < ctx->words.count; i++) if (ctx->words.items[i][0] != '-') opt_only = false;
            if (!opt_only) return false;
        }
        if (!strcmp(c, "set") && ctx->words.count >= 2 && strcmp(ctx->words.items[ctx->words.count - 1], "-o") == 0) {
            cands_add(out, "pipefail", "fail if any pipeline stage fails", CK_ARG);
            cands_add(out, "errexit", "exit on error (-e)", CK_ARG);
            cands_add(out, "xtrace", "trace commands (-x)", CK_ARG);
            cands_add(out, "nounset", "error on unset variables (-u)", CK_ARG);
            return true;
        }
        add_plain_var_names(out);
        return true;
    }
    if (!strcmp(c, "unalias") || (!strcmp(c, "alias") && !strchr(cur, '='))) {
        strvec_t n, v; sv_init(&n); sv_init(&v); alias_list(&n, &v);
        for (size_t i = 0; i < n.count; i++) cands_add(out, n.items[i], v.items[i], CK_ALIAS);
        sv_free(&n); sv_free(&v);
        return true;
    }
    if (!strcmp(c, "functions") || !strcmp(c, "funced") || !strcmp(c, "funcsave")) {
        strvec_t n; sv_init(&n); func_list_names(&n);
        for (size_t i = 0; i < n.count; i++) cands_add(out, n.items[i], "function", CK_FUNCTION);
        sv_free(&n);
        return true;
    }
    if (!strcmp(c, "abbr")) {
        strvec_t n; sv_init(&n); abbr_list(&n);
        for (size_t i = 0; i < n.count; i++) cands_add(out, n.items[i], abbr_get(n.items[i]), CK_ABBR);
        sv_free(&n);
        return true;
    }
    if (!strcmp(c, "fg") || !strcmp(c, "bg") || !strcmp(c, "disown") || !strcmp(c, "wait")) { add_jobs(out); return true; }
    if (!strcmp(c, "colorscheme")) { add_words(out, g_colorschemes, CK_ARG); return true; }
    if (!strcmp(c, "string") && ctx->argi == 1) { add_words(out, g_string_subs, CK_ARG); return true; }
    if (!strcmp(c, "status") && ctx->argi == 1) { add_words(out, g_status_subs, CK_ARG); return true; }
    if (!strcmp(c, "history") && ctx->argi == 1) { cands_add(out, "search", "search history", CK_ARG); cands_add(out, "clear", "erase history", CK_ARG); return true; }
    if (!strcmp(c, "module")) {
        if (ctx->argi == 1) { cands_add(out, "list", "show enabled modules", CK_ARG); cands_add(out, "load", "load a module now", CK_ARG); return true; }
        DIR *d = opendir(config_module_dir());
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                size_t n = strlen(e->d_name);
                if (n > 6 && strcmp(e->d_name + n - 6, ".spire") == 0) { char *m = xstrndup(e->d_name, n - 6); cands_add(out, m, "module", CK_ARG); free(m); }
            }
            closedir(d);
        }
        return true;
    }
    if (!strcmp(c, "chown") && ctx->argi == 1) { add_users(out, false); return true; }
    return false;
}

/* ======================= context analysis ======================= */

static bool is_sep_char(char c) { return c == '|' || c == '&' || c == ';' || c == '(' || c == ')' || c == '\n'; }

static bool is_cmd_prefix(const char *w) {
    static const char *p[] = { "if", "while", "until", "then", "do", "else", "elif", "!", "time", "and", "or", "not",
        "begin", "exec", "command", "builtin", "sudo", "doas", "nohup", "env", "nice", "xargs", "watch", "strace",
        "ltrace", "time", "stdbuf", "timeout", "unbuffer", "{", NULL };
    for (int i = 0; p[i]; i++) if (strcmp(p[i], w) == 0) return true;
    return false;
}

static bool looks_like_assign(const char *s) {
    if (!isalpha((unsigned char)*s) && *s != '_') return false;
    while (isalnum((unsigned char)*s) || *s == '_') s++;
    return *s == '=';
}

/* splits line[seg..end) into dequoted words */
static void split_words(const char *line, size_t from, size_t to, strvec_t *out) {
    size_t i = from;
    while (i < to) {
        while (i < to && isspace((unsigned char)line[i])) i++;
        if (i >= to) break;
        if (line[i] == '<' || line[i] == '>') { i++; continue; }
        dstr_t w; ds_init(&w);
        char q = 0;
        while (i < to && (q || (!isspace((unsigned char)line[i]) && line[i] != '<' && line[i] != '>'))) {
            char c = line[i];
            if (!q && c == '\\' && i + 1 < to) { ds_append_c(&w, line[i + 1]); i += 2; continue; }
            if (!q && (c == '\'' || c == '"')) { q = c; i++; continue; }
            if (q && c == q) { q = 0; i++; continue; }
            ds_append_c(&w, c);
            i++;
        }
        sv_push(out, xstrdup(w.data));
        ds_free(&w);
    }
}

void completion_free(Completion *c) { cands_free(&c->cands); }

void complete_line(const char *line, size_t cursor, Completion *res) {
    cands_init(&res->cands);
    res->quote = 0;

    /* 1. locate the word under the cursor and the start of its command
     *    segment, tracking quotes so `"my fi<Tab>` works */
    size_t seg_start = 0, word_start = 0;
    char q = 0;
    size_t quote_pos = 0;
    int paren = 0;
    for (size_t i = 0; i < cursor; i++) {
        char c = line[i];
        if (q == '\'') { if (c == '\'') q = 0; continue; }
        if (c == '\\' && i + 1 < cursor) { i++; continue; }
        if (q == '"') { if (c == '"') q = 0; continue; }
        if (c == '\'' || c == '"') { q = c; quote_pos = i; continue; }
        if (c == '$' && i + 1 < cursor && line[i + 1] == '(') { paren++; seg_start = word_start = i + 2; i++; continue; }
        if (is_sep_char(c)) { seg_start = word_start = i + 1; if (c == '(') paren++; continue; }
        if (isspace((unsigned char)c) || c == '<' || c == '>') word_start = i + 1;
    }
    (void)paren;
    if (q) res->quote = q;
    (void)quote_pos;
    res->word_start = word_start;

    /* the dequoted word text */
    strvec_t tmpw; sv_init(&tmpw);
    dstr_t wd; ds_init(&wd);
    {
        char qq = 0;
        for (size_t i = word_start; i < cursor; i++) {
            char c = line[i];
            if (!qq && c == '\\' && i + 1 < cursor) { ds_append_c(&wd, line[++i]); continue; }
            if (!qq && (c == '\'' || c == '"')) { qq = c; continue; }
            if (qq && c == qq) { qq = 0; continue; }
            ds_append_c(&wd, c);
        }
    }
    char *word = xstrdup(wd.data);
    ds_free(&wd);
    sv_free(&tmpw);

    /* 2. command context */
    CmdCtx ctx; sv_init(&ctx.words);
    split_words(line, seg_start, word_start, &ctx.words);
    size_t skip = 0;
    while (skip < ctx.words.count) {
        const char *w = ctx.words.items[skip];
        if (looks_like_assign(w) || is_cmd_prefix(w)) {
            skip++;
            /* sudo/env/timeout options & args */
            while (skip < ctx.words.count && ctx.words.items[skip][0] == '-') skip++;
            if (skip > 0 && strcmp(ctx.words.items[skip - 1], "timeout") == 0 && skip < ctx.words.count) skip++;
            continue;
        }
        break;
    }
    if (skip) {
        for (size_t i = 0; i < skip; i++) free(ctx.words.items[i]);
        memmove(ctx.words.items, ctx.words.items + skip, sizeof(char *) * (ctx.words.count - skip));
        ctx.words.count -= skip;
    }
    ctx.cmd = ctx.words.count ? ctx.words.items[0] : NULL;
    ctx.argi = ctx.words.count;

    bool after_redirect = false;
    {
        size_t b = word_start;
        while (b > 0 && isspace((unsigned char)line[b - 1])) b--;
        if (b > 0 && (line[b - 1] == '<' || line[b - 1] == '>')) after_redirect = true;
    }

    CandList all; cands_init(&all);
    char *match_word = xstrdup(word);
    const char *dollar = strrchr(word, '$');

    if (!res->quote || res->quote == '"') {
        if (dollar && (dollar[1] == '{' ? true : true)) {
            const char *nm = dollar + 1;
            bool brace = *nm == '{';
            if (brace) nm++;
            bool ident = true;
            for (const char *p = nm; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_') { ident = false; break; }
            if (ident) {
                char *prefix = xstrndup(word, (size_t)(nm - word));
                add_var_names(&all, prefix, brace);
                free(prefix);
                goto filter;
            }
        }
    }
    if (word[0] == '~' && !strchr(word, '/')) { add_users(&all, true); goto filter; }

    if (!ctx.cmd && !after_redirect) {
        if (strchr(word, '/')) add_files(word, &all, false);
        else add_commands(&all, true);
        goto filter;
    }

    if (ctx.cmd && !after_redirect) {
        /* --opt=value and VAR=value: complete the value as a file */
        const char *eq = strchr(word, '=');
        if (eq && (word[0] == '-' || looks_like_assign(word))) {
            char *pre = xstrndup(word, (size_t)(eq - word + 1));
            CandList files; cands_init(&files);
            add_files(eq + 1, &files, false);
            for (size_t i = 0; i < files.count; i++) {
                dstr_t t; ds_init(&t); ds_append(&t, pre); ds_append(&t, files.items[i].text);
                cands_add(&all, t.data, NULL, files.items[i].kind);
                ds_free(&t);
            }
            cands_free(&files);
            free(pre);
            goto filter;
        }
        bool want_files = true;
        fish_autoload_completion(ctx.cmd);
        if (rules_exist_for(ctx.cmd) && apply_rules(&ctx, word, line, &all, &want_files)) {
            if (word[0] == '-' && all.count == 0) add_help_options(ctx.cmd, &all);
            if (want_files && word[0] != '-') add_files(word, &all, false);
            goto filter;
        }
        bool dirs_only = false;
        if (complete_known_command(&ctx, word, &all, &dirs_only)) goto filter;
        if (word[0] == '-') {
            add_help_options(ctx.cmd, &all);
            if (all.count) goto filter;
        }
        add_files(word, &all, dirs_only);
        goto filter;
    }
    add_files(word, &all, false);

filter:
    cands_sort_dedup(&all);
    filter_cands(&all, match_word, &res->cands);
    cands_free(&all);
    free(match_word);
    free(word);
    sv_free(&ctx.words);
}

/* ======================= escaping ======================= */

char *complete_escape(const char *text, char quote) {
    dstr_t d; ds_init(&d);
    if (quote) {
        for (const char *p = text; *p; p++) {
            if (quote == '"' && (*p == '"' || *p == '\\' || *p == '`')) ds_append_c(&d, '\\');
            if (quote == '\'' && *p == '\'') { ds_append(&d, "'\\''"); continue; }
            ds_append_c(&d, *p);
        }
    } else {
        for (const char *p = text; *p; p++) {
            if (strchr(" \t\n'\"\\|&;<>()$`*?[]{}!#", *p) && !(p == text && *p == '~')) {
                /* keep a leading $VAR and ${...} unescaped */
                if (*p == '$' && (isalpha((unsigned char)p[1]) || p[1] == '{' || p[1] == '_')) { ds_append_c(&d, *p); continue; }
                if ((*p == '{' || *p == '}') && strstr(text, "${")) { ds_append_c(&d, *p); continue; }
                ds_append_c(&d, '\\');
            }
            ds_append_c(&d, *p);
        }
    }
    char *r = xstrdup(d.data);
    ds_free(&d);
    return r;
}

/* ======================= did you mean ======================= */

/* optimal-string-alignment distance (Levenshtein + adjacent transposition) */
static size_t edit_distance(const char *a, const char *b, size_t cap) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 64 || lb > 64) return cap + 1;
    if ((la > lb ? la - lb : lb - la) > cap) return cap + 1;
    size_t d[65][65];
    for (size_t i = 0; i <= la; i++) d[i][0] = i;
    for (size_t j = 0; j <= lb; j++) d[0][j] = j;
    for (size_t i = 1; i <= la; i++) {
        for (size_t j = 1; j <= lb; j++) {
            size_t cost = a[i-1] == b[j-1] ? 0 : 1;
            size_t v = d[i-1][j] + 1;
            if (d[i][j-1] + 1 < v) v = d[i][j-1] + 1;
            if (d[i-1][j-1] + cost < v) v = d[i-1][j-1] + cost;
            if (i > 1 && j > 1 && a[i-1] == b[j-2] && a[i-2] == b[j-1] && d[i-2][j-2] + 1 < v) v = d[i-2][j-2] + 1;
            d[i][j] = v;
        }
    }
    return d[la][lb];
}

char *complete_suggest_command(const char *name) {
    size_t nl = strlen(name);
    if (nl < 2 || strchr(name, '/')) return NULL;
    CandList all; cands_init(&all);
    add_commands(&all, true);
    size_t cap = nl <= 3 ? 1 : 2;
    size_t best = cap + 1;
    const char *pick = NULL;
    for (size_t i = 0; i < all.count; i++) {
        if (all.items[i].kind == CK_KEYWORD) continue;
        size_t dist = edit_distance(name, all.items[i].text, cap);
        if (dist < best || (dist == best && pick && strlen(all.items[i].text) < strlen(pick))) {
            best = dist;
            pick = all.items[i].text;
        }
    }
    char *r = pick ? xstrdup(pick) : NULL;
    cands_free(&all);
    return r;
}
