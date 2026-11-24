#include "exec.h"
#include "expand.h"
#include "vars.h"
#include "arrays.h"
#include "arith.h"
#include "funcs.h"
#include "aliases.h"
#include "jobs.h"
#include "builtins.h"
#include "lexer.h"
#include "parser.h"
#include <ctype.h>
#include <sys/stat.h>

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fnmatch.h>
#include <regex.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "highlight.h"
#include "complete.h"
#include "config.h"
#include "fishfuncs.h"

int g_exec_depth = 0;
volatile bool g_should_exit = false;
int g_exit_code = 0;
bool g_shell_is_interactive = false;
pid_t g_shell_pgid = 0;

bool g_opt_errexit = false;
bool g_opt_xtrace = false;
bool g_opt_pipefail = false;
bool g_opt_nounset = false;
long g_last_duration_ms = 0;

/* >0 while evaluating an if/while condition or the left side of &&/||,
 * where a failing command must not trigger `set -e` */
static int g_cond_depth = 0;

static bool g_break_flag = false;
static bool g_continue_flag = false;
static bool g_return_flag = false;

void exec_signal_break(void) { g_break_flag = true; }
void exec_signal_continue(void) { g_continue_flag = true; }
void exec_signal_return(int code) { g_return_flag = true; g_exit_code = code; }

static int exec_stmt(Node *n);
static int exec_simple_command(Node *n);
static int exec_cond_wrapped_cmd(Node *n) { g_cond_depth++; int s = exec_simple_command(n); g_cond_depth--; return s; }

static void flush_exit(int code) { fflush(NULL); _exit(code); }

static void set_status(int st) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", st);
    var_set("?", buf, false);
    var_set("status", buf, false);
    g_exit_code = st;
}

static void set_pipestatus(int *sts, size_t n) {
    strvec_t v; sv_init(&v);
    char buf[16];
    for (size_t i = 0; i < n; i++) { snprintf(buf, sizeof(buf), "%d", sts[i]); sv_push_dup(&v, buf); }
    array_set("PIPESTATUS", &v);
    array_set("pipestatus", &v);
    sv_free(&v);
}

static int wait_status_code(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}

/* ---------- positional parameters ---------- */

static strvec_t g_pos;
static bool g_pos_init = false;

void exec_set_positional(int count, char **params) {
    if (!g_pos_init) { sv_init(&g_pos); g_pos_init = true; }
    char buf[32];
    for (size_t i = (size_t)count; i < g_pos.count; i++) { snprintf(buf, sizeof(buf), "%zu", i + 1); var_unset(buf); }
    strvec_t fresh; sv_init(&fresh);
    for (int i = 0; i < count; i++) sv_push_dup(&fresh, params[i]);
    sv_free(&g_pos);
    g_pos = fresh;
    dstr_t joined; ds_init(&joined);
    for (size_t i = 0; i < g_pos.count; i++) {
        snprintf(buf, sizeof(buf), "%zu", i + 1);
        var_set(buf, g_pos.items[i], false);
        if (i) ds_append_c(&joined, ' ');
        ds_append(&joined, g_pos.items[i]);
    }
    snprintf(buf, sizeof(buf), "%zu", g_pos.count);
    var_set("#", buf, false);
    var_set("@", joined.data, false);
    var_set("*", joined.data, false);
    array_set("argv", &g_pos);
    ds_free(&joined);
}

size_t exec_positional_count(void) { return g_pos_init ? g_pos.count : 0; }
const char *exec_positional_get(size_t i) { return (g_pos_init && i < g_pos.count) ? g_pos.items[i] : NULL; }

/* ---------- traps ---------- */

static char *g_traps[TRAP_SLOTS];
static volatile sig_atomic_t g_trap_pending[TRAP_SLOTS];
static volatile sig_atomic_t g_any_trap_pending = 0;
static bool g_in_trap = false;

static void trap_handler(int sig) {
    if (sig > 0 && sig < TRAP_SLOTS) { g_trap_pending[sig] = 1; g_any_trap_pending = 1; }
}

void exec_set_trap(int slot, const char *cmd) {
    if (slot < 0 || slot >= TRAP_SLOTS) return;
    free(g_traps[slot]);
    g_traps[slot] = cmd ? xstrdup(cmd) : NULL;
    if (slot == 0 || slot == TRAP_ERR) return;
    if (!cmd) {
        bool shell_ignores = g_shell_is_interactive &&
            (slot == SIGINT || slot == SIGQUIT || slot == SIGTSTP || slot == SIGTTIN || slot == SIGTTOU);
        signal(slot, shell_ignores ? SIG_IGN : SIG_DFL);
    } else if (!*cmd) {
        signal(slot, SIG_IGN);
    } else {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = trap_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(slot, &sa, NULL);
    }
}

const char *exec_get_trap(int slot) { return (slot >= 0 && slot < TRAP_SLOTS) ? g_traps[slot] : NULL; }

static void run_trap_slot(int slot) {
    if (!g_traps[slot] || !*g_traps[slot] || g_in_trap) return;
    g_in_trap = true;
    int saved = g_exit_code;
    exec_source_string(g_traps[slot]);
    set_status(saved);
    g_in_trap = false;
}

void exec_run_pending_traps(void) {
    if (!g_any_trap_pending) return;
    g_any_trap_pending = 0;
    for (int i = 1; i < TRAP_SLOTS; i++) {
        if (g_trap_pending[i]) { g_trap_pending[i] = 0; run_trap_slot(i); }
    }
}

void exec_run_exit_trap(void) {
    if (g_traps[0]) {
        char *cmd = g_traps[0];
        g_traps[0] = NULL; /* run once */
        g_in_trap = true;
        bool was_exit = g_should_exit;
        g_should_exit = false;
        int code = g_exit_code;
        exec_source_string(cmd);
        g_exit_code = code;
        g_should_exit = was_exit;
        g_in_trap = false;
        free(cmd);
    }
}

/* in a forked child, traps set by the parent must not fire */
static void reset_child_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    for (int i = 1; i < TRAP_SLOTS && i < NSIG; i++) {
        if (g_traps[i] && *g_traps[i]) signal(i, SIG_DFL);
    }
}

/* A foreground child takes the terminal for its own process group itself,
 * right after creating it. The parent does the same after fork(), but
 * without this the child can reach a terminal read (or a termios change)
 * first, be stopped with SIGTTIN/SIGTTOU, and leave a stopped copy behind
 * holding its locks: the classic job-control race that bash and fish also
 * close this way. */
static void take_terminal(void) {
    if (!g_shell_is_interactive) return;
    signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(STDIN_FILENO, getpgrp());
    signal(SIGTTOU, SIG_DFL);
}

/* ---------- process substitution ---------- */

static int g_procsub_fds[64];
static int g_procsub_n = 0;

char *exec_procsub(const char *src, bool want_read) {
    int pfd[2];
    if (pipe(pfd) != 0) { sh_err("pipe: %s\n", strerror(errno)); return xstrdup("/dev/null"); }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        reset_child_signals();
        if (want_read) { dup2(pfd[1], STDOUT_FILENO); }
        else { dup2(pfd[0], STDIN_FILENO); }
        close(pfd[0]); close(pfd[1]);
        g_shell_is_interactive = false;
        g_exec_depth++;
        int st = exec_source_string(src);
        flush_exit(st);
    }
    int keep = want_read ? pfd[0] : pfd[1];
    close(want_read ? pfd[1] : pfd[0]);
    if (g_procsub_n < 64) g_procsub_fds[g_procsub_n++] = keep;
    char buf[32];
    snprintf(buf, sizeof(buf), "/dev/fd/%d", keep);
    return xstrdup(buf);
}

static void procsub_cleanup(void) {
    for (int i = 0; i < g_procsub_n; i++) close(g_procsub_fds[i]);
    g_procsub_n = 0;
}

/* ---------- alias-aware raw-word -> argv expansion ---------- */

static void expand_argv_with_aliases(strvec_t *raw_words, strvec_t *out_argv, int depth) {
    if (depth > 20 || raw_words->count == 0) {
        for (size_t i = 0; i < raw_words->count; i++) expand_word(raw_words->items[i], out_argv);
        return;
    }
    const char *first = raw_words->items[0];
    /* only bare, unquoted-looking identifiers are eligible for alias lookup */
    bool eligible = true;
    for (const char *p = first; *p; p++) {
        if (*p == '\'' || *p == '"' || *p == '$' || *p == '\\') { eligible = false; break; }
    }
    const char *aval = eligible ? alias_get(first) : NULL;
    if (!aval) {
        for (size_t i = 0; i < raw_words->count; i++) expand_word(raw_words->items[i], out_argv);
        return;
    }
    strvec_t alias_words; sv_init(&alias_words);
    Lexer lx; lexer_init(&lx, aval);
    for (;;) {
        Token t; lexer_next(&lx, &t);
        if (t.type == TOK_EOF || t.type == TOK_NEWLINE) { token_free(&t); break; }
        if (t.type == TOK_WORD) sv_push_dup(&alias_words, t.text);
        token_free(&t);
    }
    strvec_t merged; sv_init(&merged);
    for (size_t i = 0; i < alias_words.count; i++) sv_push_dup(&merged, alias_words.items[i]);
    for (size_t i = 1; i < raw_words->count; i++) sv_push_dup(&merged, raw_words->items[i]);
    bool self_ref = merged.count > 0 && strcmp(merged.items[0], first) == 0;
    sv_free(&alias_words);
    expand_argv_with_aliases(&merged, out_argv, self_ref ? depth + 1000 : depth + 1);
    sv_free(&merged);
}

/* ---------- assignment parsing: NAME=v, NAME+=v, NAME[idx]=v, NAME=(a b) ---------- */

typedef struct {
    char *name;
    char *index_expr; /* NULL if not an indexed assignment */
    bool append;       /* += form */
    const char *rawval; /* points inside the original raw string */
} Assignment;

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void parse_assignment_lhs(const char *raw, Assignment *a) {
    const char *p = raw;
    const char *namestart = p;
    while (ident_char(*p)) p++;
    a->name = xstrndup(namestart, (size_t)(p - namestart));
    a->index_expr = NULL;
    if (*p == '[') {
        const char *istart = p + 1;
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') { depth--; if (depth == 0) break; }
            p++;
        }
        a->index_expr = xstrndup(istart, (size_t)(p - istart));
        if (*p == ']') p++;
    }
    a->append = false;
    if (*p == '+' && p[1] == '=') { a->append = true; p += 2; }
    else if (*p == '=') p += 1;
    a->rawval = p;
}

/* true if `val` is exactly one balanced, unquoted "(...)" group spanning
 * the whole string -- an array literal like "(a b c)". */
static bool is_paren_literal(const char *val) {
    if (val[0] != '(') return false;
    int depth = 0;
    bool insq = false, indq = false;
    size_t n = strlen(val);
    for (size_t i = 0; i < n; i++) {
        char c = val[i];
        if (c == '\\' && !insq) { i++; continue; }
        if (c == '\'' && !indq) { insq = !insq; continue; }
        if (c == '"' && !insq) { indq = !indq; continue; }
        if (c == '(' && !insq) depth++;
        else if (c == ')' && !insq) { depth--; if (depth == 0) return i == n - 1; }
    }
    return false;
}

static void parse_array_literal_into(const char *val, strvec_t *out) {
    size_t n = strlen(val);
    char *inner = xstrndup(val + 1, n >= 2 ? n - 2 : 0);
    Lexer lx; lexer_init(&lx, inner);
    for (;;) {
        Token t; lexer_next(&lx, &t);
        if (t.type == TOK_EOF || t.type == TOK_NEWLINE) { token_free(&t); break; }
        if (t.type == TOK_WORD) expand_word(t.text, out);
        token_free(&t);
    }
    free(inner);
}

/* Permanent assignment (a bare `NAME=value` statement, or `NAME[i]=value`,
 * or `NAME=(array literal)`, or any of those with a leading `+`). */
static void do_permanent_assignment(const char *raw) {
    Assignment a; parse_assignment_lhs(raw, &a);

    if (a.index_expr) {
        long idx = arith_eval(a.index_expr);
        char *val = expand_word_single(a.rawval);
        if (a.append) {
            const char *old = array_get_index(a.name, idx);
            dstr_t d; ds_init(&d);
            if (old) ds_append(&d, old);
            ds_append(&d, val);
            array_set_index(a.name, idx, d.data);
            ds_free(&d);
        } else {
            array_set_index(a.name, idx, val);
        }
        free(val);
    } else if (is_paren_literal(a.rawval)) {
        strvec_t elems; sv_init(&elems);
        parse_array_literal_into(a.rawval, &elems);
        if (a.append) array_append(a.name, &elems);
        else array_set(a.name, &elems);
        sv_free(&elems);
    } else {
        char *val = expand_word_single(a.rawval);
        if (a.append) {
            const char *old = var_get(a.name);
            dstr_t d; ds_init(&d);
            if (old) ds_append(&d, old);
            ds_append(&d, val);
            var_set(a.name, d.data, var_is_exported(a.name));
            ds_free(&d);
        } else {
            var_set(a.name, val, var_is_exported(a.name));
        }
        free(val);
    }
    free(a.name);
    free(a.index_expr);
}

/* ---------- env prefix assignments (FOO=bar cmd) — scalars only ---------- */

typedef struct { char *name; char *old; bool had; bool exported; } SavedVar;

static void apply_env_assigns(strvec_t *assigns, SavedVar **out_saved, int *out_n) {
    int n = (int)assigns->count;
    SavedVar *saved = n ? xmalloc(sizeof(SavedVar) * n) : NULL;
    for (int i = 0; i < n; i++) {
        Assignment a; parse_assignment_lhs(assigns->items[i], &a);
        char *val = expand_word_single(a.rawval);
        if (a.append) {
            const char *old = var_get(a.name);
            dstr_t d; ds_init(&d);
            if (old) ds_append(&d, old);
            ds_append(&d, val);
            free(val);
            val = xstrdup(d.data);
            ds_free(&d);
        }
        const char *old = var_get(a.name);
        saved[i].name = a.name; /* transfer ownership */
        saved[i].had = old != NULL;
        saved[i].old = old ? xstrdup(old) : NULL;
        saved[i].exported = var_is_exported(a.name);
        var_set(a.name, val, true);
        free(a.index_expr);
        free(val);
    }
    *out_saved = saved;
    *out_n = n;
}

static void restore_env_assigns(SavedVar *saved, int n) {
    for (int i = 0; i < n; i++) {
        if (saved[i].had) var_set(saved[i].name, saved[i].old, saved[i].exported);
        else var_unset(saved[i].name);
        free(saved[i].name);
        free(saved[i].old);
    }
    free(saved);
}

/* ---------- redirection ---------- */

typedef struct { int fd; int saved; } FdSave;
typedef struct { FdSave items[32]; int n; } FdSaveList;

/* remember fd's current state (once) before it is changed in-process */
static void save_fd(FdSaveList *sv, int fd) {
    if (!sv) return;
    for (int i = 0; i < sv->n; i++) if (sv->items[i].fd == fd) return;
    if (sv->n >= 32) return;
    int saved = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    sv->items[sv->n].fd = fd;
    sv->items[sv->n].saved = saved; /* -1: fd was closed before */
    sv->n++;
}

static void restore_redirects(FdSaveList *sv) {
    if (!sv) return;
    fflush(stdout); fflush(stderr);
    for (int i = sv->n - 1; i >= 0; i--) {
        if (sv->items[i].saved >= 0) { dup2(sv->items[i].saved, sv->items[i].fd); close(sv->items[i].saved); }
        else close(sv->items[i].fd);
    }
    sv->n = 0;
}

static bool all_digits(const char *s) {
    if (!*s) return false;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

/* feeds `body` to `fd` through a pipe; small bodies are written directly,
 * large ones from a child so the pipe buffer can't deadlock us */
static int feed_text_to_fd(const char *body, int fd, FdSaveList *sv) {
    int pfd[2];
    if (pipe(pfd) != 0) { sh_err("pipe: %s\n", strerror(errno)); return -1; }
    size_t len = strlen(body);
    if (len < 60000) {
        size_t off = 0;
        while (off < len) { ssize_t w = write(pfd[1], body + off, len - off); if (w <= 0) break; off += (size_t)w; }
    } else {
        pid_t wpid = fork();
        if (wpid == 0) {
            close(pfd[0]);
            size_t off = 0;
            while (off < len) { ssize_t w = write(pfd[1], body + off, len - off); if (w <= 0) break; off += (size_t)w; }
            _exit(0);
        }
    }
    close(pfd[1]);
    save_fd(sv, fd);
    dup2(pfd[0], fd);
    close(pfd[0]);
    return 0;
}

/* Applies redirections to the current process. With `sv` non-NULL, every
 * touched fd is saved first so restore_redirects() can undo it (used for
 * builtins, functions and compound commands that run in the shell itself). */
static int apply_redirects_ex(Redirect *r, FdSaveList *sv) {
    fflush(stdout); fflush(stderr);
    for (; r; r = r->next) {
        if (r->type == R_HEREDOC) {
            char *body = r->heredoc_no_expand ? xstrdup(r->heredoc_body) : expand_heredoc_body(r->heredoc_body);
            int rc = feed_text_to_fd(body, r->fd, sv);
            free(body);
            if (rc != 0) return -1;
            continue;
        }
        if (r->type == R_HERESTRING) {
            char *val = expand_word_single(r->target);
            dstr_t d; ds_init(&d); ds_append(&d, val); ds_append_c(&d, '\n');
            int rc = feed_text_to_fd(d.data, r->fd, sv);
            ds_free(&d); free(val);
            if (rc != 0) return -1;
            continue;
        }

        char *target = expand_word_single(r->target);
        if (r->type == R_DUP) {
            if (strcmp(target, "-") == 0) { save_fd(sv, r->fd); close(r->fd); free(target); continue; }
            if (all_digits(target)) {
                int src = atoi(target);
                if (fcntl(src, F_GETFD) < 0) { sh_err("%s: bad file descriptor\n", target); free(target); return -1; }
                save_fd(sv, r->fd);
                if (src != r->fd) dup2(src, r->fd);
                free(target);
                continue;
            }
            /* `>&file` is the same as `&>file` */
        }
        int flags;
        bool both = false;
        switch (r->type) {
            case R_IN:   flags = O_RDONLY; break;
            case R_RDWR: flags = O_RDWR | O_CREAT; break;
            case R_OUT:  flags = O_WRONLY | O_CREAT | O_TRUNC; break;
            case R_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
            case R_OUT_ERR_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; both = true; break;
            default: /* R_OUT_ERR, R_DUP-to-file */ flags = O_WRONLY | O_CREAT | O_TRUNC; both = true; break;
        }
        int fd = open(target, flags, 0644);
        if (fd < 0) {
            sh_err("%s: %s\n", target, strerror(errno));
            free(target);
            return -1;
        }
        free(target);
        if (both) {
            save_fd(sv, STDOUT_FILENO); save_fd(sv, STDERR_FILENO);
            dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO);
            close(fd);
        } else {
            save_fd(sv, r->fd);
            if (fd != r->fd) { dup2(fd, r->fd); close(fd); }
        }
    }
    return 0;
}

static int apply_redirects(Redirect *r) { return apply_redirects_ex(r, NULL); }

/* ---------- command resolution & execution ---------- */

static char **argv_from_strvec(strvec_t *v) { return sv_to_argv(v); }

static void report_exec_failure(const char *name, int err) {
    if (err == ENOENT && !strchr(name, '/')) {
        sh_err("command not found: %s\n", name);
    } else {
        sh_err("%s: %s\n", name, strerror(err));
    }
}

/* waits for a foreground child/process group, handing it the terminal */
static int wait_foreground(pid_t pid, const char *desc) {
    if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, pid);
    int status;
    while (waitpid(pid, &status, WUNTRACED) < 0 && errno == EINTR) {}
    if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    if (WIFSTOPPED(status)) {
        Job *j = job_add(pid, desc);
        j->state = JOB_STOPPED;
        fprintf(stderr, "\n[%d]+  Stopped                 %s\n", j->id, desc);
        return 128 + WSTOPSIG(status);
    }
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT && g_shell_is_interactive) fputc('\n', stderr);
    return wait_status_code(status);
}

/* Runs a resolved command (builtin, function, or external) in the CURRENT
 * process. For external commands this calls execve and does not return on
 * success; caller must be prepared to _exit() afterward when used as a
 * pipeline stage. */
static int run_in_current_process(strvec_t *argv, bool allow_exec_replace) {
    char **cargv = argv_from_strvec(argv);
    const char *name = argv->items[0];

    Node *fn = func_get(name);
    if (fn) {
        int st = exec_call_function(fn, (int)argv->count, cargv);
        free(cargv);
        return st;
    }
    BuiltinFn b = builtin_lookup(name);
    if (b) {
        int st = b((int)argv->count, cargv);
        fflush(stdout);
        free(cargv);
        return st;
    }
    if (allow_exec_replace) {
        execvp(name, cargv);
        int err = errno;
        report_exec_failure(name, err);
        free(cargv);
        flush_exit(err == ENOENT ? 127 : 126);
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { sh_err("fork: %s\n", strerror(errno)); free(cargv); return 1; }
    if (pid == 0) {
        reset_child_signals();
        setpgid(0, 0);
        take_terminal();
        execvp(name, cargv);
        int err = errno;
        report_exec_failure(name, err);
        flush_exit(err == ENOENT ? 127 : 126);
    }
    setpgid(pid, pid);
    free(cargv);
    return wait_foreground(pid, name);
}

static bool is_arith_cmd(Node *n) {
    return n->type == N_CMD && n->argv.count == 2 && strcmp(n->argv.items[0], "((") == 0;
}

static void xtrace_print(strvec_t *argv) {
    const char *ps4 = var_get("PS4");
    dstr_t d; ds_init(&d);
    bool color = isatty(STDERR_FILENO);
    if (color) ds_append(&d, "\x1b[90m");
    ds_append(&d, ps4 ? ps4 : "+ ");
    for (size_t i = 0; i < argv->count; i++) { if (i) ds_append_c(&d, ' '); ds_append(&d, argv->items[i]); }
    if (color) ds_append(&d, "\x1b[0m");
    ds_append_c(&d, '\n');
    fflush(stdout);
    fputs(d.data, stderr);
    ds_free(&d);
}

/* The command isn't a function/builtin/alias and can't be found on $PATH.
 * Tries, in order: autocd into a directory of that name, a user-defined
 * handler function (bash's command_not_found_handle or fish's
 * fish_command_not_found), then a "did you mean" hint. */
static int handle_not_found(strvec_t *argv, int *handled) {
    const char *name = argv->items[0];
    *handled = 1;
    struct stat st;
    const char *autocd = config_get("autocd", "yes");
    if (argv->count == 1 && g_shell_is_interactive && strcmp(autocd, "yes") == 0 &&
        stat(name, &st) == 0 && S_ISDIR(st.st_mode)) {
        char *cdargv[] = { (char *)"cd", (char *)name, NULL };
        return builtin_lookup("cd")(2, cdargv);
    }
    const char *handlers[] = { "command_not_found_handle", "fish_command_not_found", NULL };
    for (int i = 0; handlers[i]; i++) {
        Node *fn = func_get(handlers[i]);
        if (!fn) continue;
        strvec_t hv; sv_init(&hv);
        sv_push_dup(&hv, handlers[i]);
        for (size_t k = 0; k < argv->count; k++) sv_push_dup(&hv, argv->items[k]);
        char **c = sv_to_argv(&hv);
        int r = exec_call_function(fn, (int)hv.count, c);
        free(c); sv_free(&hv);
        return r;
    }
    sh_err("command not found: %s\n", name);
    char *sugg = complete_suggest_command(name);
    if (sugg) {
        bool color = isatty(STDERR_FILENO);
        fprintf(stderr, "       did you mean %s%s%s?\n", color ? "\x1b[1;32m" : "", sugg, color ? "\x1b[0m" : "");
        free(sugg);
    }
    return 127;
}

void exec_assign(const char *raw) { do_permanent_assignment(raw); }

bool exec_looks_like_assignment(const char *s) {
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    size_t i = 1;
    while (s[i] && ident_char(s[i])) i++;
    if (s[i] == '[') { while (s[i] && s[i] != ']') i++; if (s[i] == ']') i++; }
    if (s[i] == '+' && s[i+1] == '=') return true;
    return s[i] == '=';
}

/* declaration builtins receive `name=value` words unexpanded so the
 * builtin can expand them exactly once (and keep `a=(1 2)` intact) */
static bool is_decl_builtin(const char *w) {
    return strcmp(w, "local") == 0 || strcmp(w, "declare") == 0 || strcmp(w, "typeset") == 0 ||
           strcmp(w, "export") == 0 || strcmp(w, "readonly") == 0;
}

static void expand_decl_argv(strvec_t *raw, strvec_t *out) {
    for (size_t i = 0; i < raw->count; i++) {
        if (i > 0 && exec_looks_like_assignment(raw->items[i])) sv_push_dup(out, raw->items[i]);
        else expand_word(raw->items[i], out);
    }
}

/* bind's words keep fish key escapes (\\cf, \\e[A): unquoted words that
 * contain a backslash pass through as written, quotes are just removed */
static void expand_bind_argv(strvec_t *raw, strvec_t *out) {
    for (size_t i = 0; i < raw->count; i++) {
        const char *w = raw->items[i];
        size_t n = strlen(w);
        if (n >= 2 && w[0] == '\'' && w[n - 1] == '\'') { sv_push(out, xstrndup(w + 1, n - 2)); continue; }
        if (i > 0 && strchr(w, '\\') && w[0] != '"') { sv_push_dup(out, w); continue; }
        expand_word(w, out);
    }
}

int exec_eval_string(const char *src) {
    Node *root; char *err = NULL;
    ParseStatus st = parse_program(src, &root, &err);
    if (st == PARSE_EMPTY) return 0;
    if (st == PARSE_INCOMPLETE) { sh_err("eval: unexpected end of input\n"); return 2; }
    if (st == PARSE_ERROR) { sh_err("eval: %s\n", err); free(err); return 2; }
    int status = exec_block(root);
    /* function definitions transferred their bodies; the rest is ours */
    node_free(root);
    return status;
}

int exec_run_argv_nofunc(int argc, char **argv) {
    if (argc < 1) return 0;
    BuiltinFn b = builtin_lookup(argv[0]);
    if (b) return b(argc, argv);
    strvec_t v; sv_init(&v);
    for (int i = 0; i < argc; i++) sv_push_dup(&v, argv[i]);
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { sv_free(&v); return 1; }
    if (pid == 0) {
        reset_child_signals();
        setpgid(0, 0);
        take_terminal();
        char **c = sv_to_argv(&v);
        execvp(c[0], c);
        int err = errno;
        report_exec_failure(c[0], err);
        flush_exit(err == ENOENT ? 127 : 126);
    }
    setpgid(pid, pid);
    int st = wait_foreground(pid, argv[0]);
    sv_free(&v);
    return st;
}

static int exec_simple_command(Node *n) {
    if (n->argv.count == 0) {
        /* pure variable/array assignment(s), persists in the shell; or a
         * bare redirection like `> file` (truncate/create) */
        for (size_t i = 0; i < n->env_assigns.count; i++)
            do_permanent_assignment(n->env_assigns.items[i]);
        if (n->redirects) {
            FdSaveList sv = {0};
            int rc = apply_redirects_ex(n->redirects, &sv);
            restore_redirects(&sv);
            procsub_cleanup();
            return rc == 0 ? 0 : 1;
        }
        return 0;
    }

    if (is_arith_cmd(n)) {
        /* `(( expr ))`: evaluate directly, bypassing normal word expansion
         * so arithmetic operators (*, <, >, &, |, ...) aren't mistaken for
         * globs/redirection by the ordinary expansion pipeline. Exit
         * status follows bash: 0 (success) if the result is non-zero. */
        long v = arith_eval(n->argv.items[1]);
        return v != 0 ? 0 : 1;
    }

    strvec_t argv; sv_init(&argv);
    if (is_decl_builtin(n->argv.items[0]) && func_get(n->argv.items[0]) == NULL) expand_decl_argv(&n->argv, &argv);
    else if (strcmp(n->argv.items[0], "bind") == 0 && func_get("bind") == NULL) expand_bind_argv(&n->argv, &argv);
    else expand_argv_with_aliases(&n->argv, &argv, 0);
    if (argv.count == 0) {
        sv_free(&argv);
        for (size_t i = 0; i < n->env_assigns.count; i++) do_permanent_assignment(n->env_assigns.items[i]);
        procsub_cleanup();
        return 0;
    }
    if (g_opt_xtrace) xtrace_print(&argv);

    /* `exec` with only redirections: make them permanent in this shell */
    if (argv.count == 1 && strcmp(argv.items[0], "exec") == 0 && func_get("exec") == NULL) {
        int rc = apply_redirects(n->redirects);
        sv_free(&argv);
        return rc == 0 ? 0 : 1;
    }

    SavedVar *saved; int nsaved;
    apply_env_assigns(&n->env_assigns, &saved, &nsaved);

    int status;
    if (func_get(argv.items[0]) == NULL && builtin_lookup(argv.items[0]) == NULL && !strchr(argv.items[0], '/'))
        fish_autoload_function(argv.items[0]); /* fish: functions/NAME.fish */
    bool is_ext = func_get(argv.items[0]) == NULL && builtin_lookup(argv.items[0]) == NULL;

    if (is_ext && !command_exists(argv.items[0])) {
        int handled = 0;
        FdSaveList sv = {0};
        if (apply_redirects_ex(n->redirects, &sv) == 0) status = handle_not_found(&argv, &handled);
        else status = 1;
        restore_redirects(&sv);
    } else if (!is_ext) {
        /* builtins and functions run inside the shell, so `cd`, `read`,
         * `export` ... keep their effect even with redirections attached */
        FdSaveList sv = {0};
        if (apply_redirects_ex(n->redirects, &sv) == 0) status = run_in_current_process(&argv, false);
        else status = 1;
        restore_redirects(&sv);
    } else if (n->redirects == NULL) {
        status = run_in_current_process(&argv, false);
    } else {
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) {
            sh_err("fork: %s\n", strerror(errno));
            status = 1;
        } else if (pid == 0) {
            reset_child_signals();
            setpgid(0, 0);
            take_terminal();
            if (apply_redirects(n->redirects) != 0) flush_exit(1);
            int st = run_in_current_process(&argv, true);
            flush_exit(st);
        } else {
            setpgid(pid, pid);
            status = wait_foreground(pid, argv.items[0]);
        }
    }

    restore_env_assigns(saved, nsaved);
    sv_free(&argv);
    procsub_cleanup();
    set_pipestatus(&status, 1);
    if (status != 0 && g_traps[TRAP_ERR] && g_cond_depth == 0) run_trap_slot(TRAP_ERR);
    return status;
}

/* single node used as one stage of a pipeline; called already inside a
 * forked child, so builtins/functions are safe to run in-process, and
 * external commands may exec() directly. */
static int exec_pipeline_stage(Node *n) {
    if (n->type != N_CMD) return exec_stmt(n);
    if (is_arith_cmd(n)) { long v = arith_eval(n->argv.items[1]); return v != 0 ? 0 : 1; }
    strvec_t argv; sv_init(&argv);
    expand_argv_with_aliases(&n->argv, &argv, 0);
    if (apply_redirects(n->redirects) != 0) flush_exit(1);
    if (argv.count == 0) { sv_free(&argv); return 0; }
    if (g_opt_xtrace) xtrace_print(&argv);
    SavedVar *saved; int nsaved;
    apply_env_assigns(&n->env_assigns, &saved, &nsaved);
    int st = run_in_current_process(&argv, true);
    restore_env_assigns(saved, nsaved);
    sv_free(&argv);
    return st;
}

static char *node_to_string(Node *n); /* best-effort, forward declared */

static int exec_pipeline(Node *n) {
    size_t k = n->nchildren;
    if (k == 1) return exec_stmt(n->children[0]);

    int (*pipes)[2] = xmalloc(sizeof(int[2]) * (k - 1));
    for (size_t i = 0; i < k - 1; i++) {
        if (pipe(pipes[i]) != 0) { sh_err("pipe: %s\n", strerror(errno)); free(pipes); return 1; }
    }
    pid_t *pids = xmalloc(sizeof(pid_t) * k);
    pid_t pgid = 0;
    fflush(NULL);

    /* like fish and zsh, a trailing builtin/function runs in the shell
     * itself, so `cmd | source`, `cmd | read x` and `cmd | set` keep their
     * effect */
    Node *last = n->children[k - 1];
    bool lastpipe = false;
    if (last->type == N_CMD && last->argv.count > 0 && !is_arith_cmd(last) && !strpbrk(last->argv.items[0], "$`'\"\\(")) {
        const char *w = last->argv.items[0];
        lastpipe = !alias_get(w) && (builtin_lookup(w) != NULL || func_get(w) != NULL);
    }
    size_t nfork = lastpipe ? k - 1 : k;

    for (size_t i = 0; i < nfork; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            setpgid(0, pgid); /* first stage: pgid==0 -> becomes its own leader */
            reset_child_signals();
            if (!lastpipe) take_terminal(); /* with lastpipe the shell keeps the terminal */
            g_shell_is_interactive = false; /* no job control inside a stage */
            if (i > 0) dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < k - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
                if (n->children[i]->pipe_stderr) dup2(pipes[i][1], STDERR_FILENO);
            }
            for (size_t j = 0; j < k - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            int st = exec_pipeline_stage(n->children[i]);
            flush_exit(st);
        }
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid);
        pids[i] = pid;
    }
    int *sts = xmalloc(sizeof(int) * k);
    if (lastpipe) {
        int saved_in = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 10);
        dup2(pipes[k - 2][0], STDIN_FILENO);
        for (size_t j = 0; j < k - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); }
        sts[k - 1] = exec_simple_command(last);
        fflush(stdout);
        if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
    } else {
        for (size_t j = 0; j < k - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); }
    }
    procsub_cleanup();

    if (g_shell_is_interactive && !lastpipe) tcsetpgrp(STDIN_FILENO, pgid);
    bool stopped = false;
    for (size_t i = 0; i < nfork; i++) {
        int st;
        while (waitpid(pids[i], &st, WUNTRACED) < 0 && errno == EINTR) {}
        if (WIFSTOPPED(st)) { stopped = true; sts[i] = 128 + WSTOPSIG(st); }
        else sts[i] = wait_status_code(st);
    }
    if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    if (stopped) {
        char *desc = node_to_string(n);
        Job *j = job_add(pgid, desc);
        j->state = JOB_STOPPED;
        fprintf(stderr, "\n[%d]+  Stopped                 %s\n", j->id, desc);
        free(desc);
    }

    int status = sts[k - 1];
    if (g_opt_pipefail) for (size_t i = 0; i < k; i++) if (sts[i] != 0) status = sts[i];
    set_pipestatus(sts, k);
    free(sts);
    free(pipes);
    free(pids);
    jobs_reap(false);
    return status;
}

/* ---------- background jobs ---------- */

static int exec_background(Node *n) {
    n->background = false; /* avoid re-triggering once forked */
    pid_t pid = fork();
    if (pid < 0) { sh_err("fork: %s\n", strerror(errno)); n->background = true; return 1; }
    if (pid == 0) {
        setpgid(0, 0);
        reset_child_signals();
        g_shell_is_interactive = false;
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        int st = exec_stmt(n);
        flush_exit(st);
    }
    setpgid(pid, pid);
    char *desc = node_to_string(n);
    Job *j = job_add(pid, desc);
    if (g_shell_is_interactive) fprintf(stderr, "[%d] %d\n", j->id, (int)pid);
    char pidbuf[16]; snprintf(pidbuf, sizeof(pidbuf), "%d", (int)pid);
    var_set("!", pidbuf, false);
    procsub_cleanup();
    free(desc);
    n->background = true;
    return 0;
}

/* ---------- control flow ---------- */

static bool errexit_applies(Node *n) {
    if (n->negate) return false;
    return n->type == N_CMD || n->type == N_PIPELINE || n->type == N_SUBSHELL || n->type == N_TIME;
}

int exec_block(Node *block) {
    int status = 0;
    for (size_t i = 0; i < block->nchildren; i++) {
        status = exec_stmt(block->children[i]);
        set_status(status);
        exec_run_pending_traps();
        if (status != 0 && g_opt_errexit && g_cond_depth == 0 && errexit_applies(block->children[i])) {
            g_should_exit = true;
            g_exit_code = status;
        }
        if (g_should_exit || g_break_flag || g_continue_flag || g_return_flag) return status;
    }
    return status;
}

static int exec_cond(Node *n) {
    g_cond_depth++;
    int st = exec_stmt(n);
    g_cond_depth--;
    return st;
}

static int exec_if(Node *n) {
    int cond = exec_cond(n->children[0]);
    if (g_should_exit || g_break_flag || g_continue_flag || g_return_flag) return cond;
    if (cond == 0) return exec_block(n->children[1]);
    if (n->nchildren > 2) return exec_block(n->children[2]);
    return 0;
}

static int exec_while(Node *n) {
    int status = 0;
    for (;;) {
        int cond = exec_cond(n->children[0]);
        if (g_should_exit || g_return_flag) return cond;
        if (cond != 0) break;
        status = exec_block(n->children[1]);
        if (g_should_exit || g_return_flag) return status;
        if (g_break_flag) { g_break_flag = false; break; }
        if (g_continue_flag) { g_continue_flag = false; continue; }
    }
    return status;
}

static int exec_until(Node *n) {
    int status = 0;
    for (;;) {
        int cond = exec_cond(n->children[0]);
        if (g_should_exit || g_return_flag) return cond;
        if (cond == 0) break; /* condition became true -> stop looping */
        status = exec_block(n->children[1]);
        if (g_should_exit || g_return_flag) return status;
        if (g_break_flag) { g_break_flag = false; break; }
        if (g_continue_flag) { g_continue_flag = false; continue; }
    }
    return status;
}

/* ---------- [[ ... ]] extended test ---------- */

static int exec_cond_atom(Node *n) {
    strvec_t words; sv_init(&words);
    for (size_t i = 0; i < n->argv.count; i++)
        sv_push(&words, expand_pattern_single(n->argv.items[i]));

    int result;
    if (words.count == 3 && (strcmp(words.items[1], "==") == 0 || strcmp(words.items[1], "=") == 0)) {
        result = fnmatch(words.items[2], words.items[0], 0) == 0 ? 0 : 1;
    } else if (words.count == 3 && strcmp(words.items[1], "!=") == 0) {
        result = fnmatch(words.items[2], words.items[0], 0) != 0 ? 0 : 1;
    } else if (words.count == 3 && strcmp(words.items[1], "=~") == 0) {
        regex_t re;
        if (regcomp(&re, words.items[2], REG_EXTENDED) != 0) {
            result = 1;
        } else {
            result = regexec(&re, words.items[0], 0, NULL, 0) == 0 ? 0 : 1;
            regfree(&re);
        }
    } else if (words.count == 0) {
        result = 1;
    } else {
        char **cargv = xmalloc(sizeof(char *) * (words.count + 2));
        cargv[0] = (char *)"test";
        for (size_t i = 0; i < words.count; i++) cargv[i + 1] = words.items[i];
        cargv[words.count + 1] = NULL;
        result = test_eval((int)words.count + 1, cargv);
        free(cargv);
    }
    sv_free(&words);
    if (n->negate) result = (result == 0) ? 1 : 0;
    return result;
}

static int exec_for(Node *n) {
    strvec_t items; sv_init(&items);
    for (size_t i = 0; i < n->for_words.count; i++) expand_word(n->for_words.items[i], &items);
    int status = 0;
    bool was_exported = var_is_exported(n->for_var);
    for (size_t i = 0; i < items.count; i++) {
        var_set(n->for_var, items.items[i], was_exported);
        status = exec_block(n->children[0]);
        if (g_should_exit || g_return_flag) { sv_free(&items); return status; }
        if (g_break_flag) { g_break_flag = false; break; }
        if (g_continue_flag) { g_continue_flag = false; continue; }
    }
    sv_free(&items);
    return status;
}

int exec_call_function(Node *body, int argc, char **argv) {
    if (g_exec_depth > 200) { sh_err("function call depth exceeded\n"); return 1; }
    g_exec_depth++;
    var_push_scope();

    /* save the caller's positional parameters, install the function's */
    strvec_t saved_pos; sv_init(&saved_pos);
    for (size_t i = 0; i < exec_positional_count(); i++) sv_push_dup(&saved_pos, exec_positional_get(i));
    const char *old0 = var_get("0");
    char *saved0 = old0 ? xstrdup(old0) : NULL;
    var_set("0", argv[0], false);
    exec_set_positional(argc - 1, argv + 1);

    /* fish `function f -a first second`: name the arguments as locals */
    for (size_t i = 0; i < body->for_words.count; i++) {
        var_declare_local(body->for_words.items[i]);
        var_set(body->for_words.items[i], (int)i + 1 < argc ? argv[i + 1] : "", false);
    }

    int status = exec_block(body);
    if (g_return_flag) { g_return_flag = false; status = g_exit_code; }
    /* break/continue escaping a function body is a script bug; swallow it
     * at the function boundary rather than corrupting the caller's loop */
    g_break_flag = false;
    g_continue_flag = false;

    exec_set_positional((int)saved_pos.count, saved_pos.items);
    sv_free(&saved_pos);
    if (saved0) { var_set("0", saved0, false); free(saved0); }

    var_pop_scope();
    g_exec_depth--;
    return status;
}

static int exec_case(Node *n) {
    char *subject = expand_word_single(n->case_subject);
    int status = 0;
    for (size_t i = 0; i + 1 < n->nchildren; i += 2) {
        Node *pats = n->children[i];
        Node *body = n->children[i + 1];
        bool arm_matches = false;
        for (size_t k = 0; k < pats->argv.count; k++) {
            char *pat = expand_pattern_single(pats->argv.items[k]);
            if (fnmatch(pat, subject, 0) == 0) arm_matches = true;
            free(pat);
            if (arm_matches) break;
        }
        if (arm_matches) { status = exec_block(body); break; }
    }
    free(subject);
    return status;
}

static int exec_subshell(Node *n) {
    pid_t pid = fork();
    if (pid < 0) { sh_err("fork: %s\n", strerror(errno)); return 1; }
    if (pid == 0) {
        reset_child_signals();
        setpgid(0, 0);
        take_terminal();
        g_shell_is_interactive = false;
        int st = exec_block(n->children[0]);
        exec_run_exit_trap();
        flush_exit(g_should_exit ? g_exit_code : st);
    }
    setpgid(pid, pid);
    return wait_foreground(pid, "(subshell)");
}

static double tv_secs(struct timeval tv) { return (double)tv.tv_sec + (double)tv.tv_usec / 1e6; }

static void print_time_line(const char *label, double secs) {
    long m = (long)(secs / 60);
    double s = secs - (double)m * 60;
    fprintf(stderr, "%s\t%ldm%.3fs\n", label, m, s);
}

static int exec_time(Node *n) {
    struct timespec t0, t1;
    struct rusage c0, c1, s0, s1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    getrusage(RUSAGE_CHILDREN, &c0);
    getrusage(RUSAGE_SELF, &s0);
    int st = exec_stmt(n->children[0]);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    getrusage(RUSAGE_CHILDREN, &c1);
    getrusage(RUSAGE_SELF, &s1);
    double real = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double user = tv_secs(c1.ru_utime) - tv_secs(c0.ru_utime) + tv_secs(s1.ru_utime) - tv_secs(s0.ru_utime);
    double sys = tv_secs(c1.ru_stime) - tv_secs(c0.ru_stime) + tv_secs(s1.ru_stime) - tv_secs(s0.ru_stime);
    fflush(stdout);
    fputc('\n', stderr);
    print_time_line("real", real);
    print_time_line("user", user);
    print_time_line("sys", sys);
    return st;
}

/* ---------- dispatcher ---------- */

static int exec_stmt_inner(Node *n);

static int exec_stmt(Node *n) {
    if (!n) return 0;
    if (n->background && n->type != N_FUNCDEF) return exec_background(n);
    if (n->redirects && n->type != N_CMD) {
        /* compound command with trailing redirections: `while ...; done < f` */
        FdSaveList sv = {0};
        int st;
        if (apply_redirects_ex(n->redirects, &sv) == 0) st = exec_stmt_inner(n);
        else st = 1;
        restore_redirects(&sv);
        procsub_cleanup();
        return st;
    }
    return exec_stmt_inner(n);
}

static int negate_if(Node *n, int s) { return n->negate ? (s == 0) : s; }

static int exec_stmt_inner(Node *n) {
    switch (n->type) {
        case N_CMD: return negate_if(n, n->negate ? exec_cond_wrapped_cmd(n) : exec_simple_command(n));
        case N_PIPELINE: return negate_if(n, exec_pipeline(n));
        case N_AND: {
            int l = exec_cond(n->children[0]);
            if (g_should_exit || g_break_flag || g_continue_flag || g_return_flag) return l;
            int s = (l != 0) ? l : exec_stmt(n->children[1]);
            return negate_if(n, s);
        }
        case N_OR: {
            int l = exec_cond(n->children[0]);
            if (g_should_exit || g_break_flag || g_continue_flag || g_return_flag) return l;
            int s = (l == 0) ? l : exec_stmt(n->children[1]);
            return negate_if(n, s);
        }
        case N_FISH_AND: {
            int last = g_exit_code;
            if (last != 0) return last;
            return exec_stmt(n->children[0]);
        }
        case N_FISH_OR: {
            int last = g_exit_code;
            if (last == 0) return 0;
            return exec_stmt(n->children[0]);
        }
        case N_SEQ: return negate_if(n, exec_block(n));
        case N_BLOCK: return negate_if(n, exec_block(n));
        case N_IF: return exec_if(n);
        case N_WHILE: return exec_while(n);
        case N_UNTIL: return exec_until(n);
        case N_FOR: return exec_for(n);
        case N_CASE: return exec_case(n);
        case N_SUBSHELL: return negate_if(n, exec_subshell(n));
        case N_CONDATOM: return exec_cond_atom(n);
        case N_TIME: return exec_time(n);
        case N_FUNCDEF: {
            Node *body = n->children[0];
            n->children[0] = NULL; /* transfer ownership to the function table */
            func_define(n->func_name, body);
            return 0;
        }
    }
    return 0;
}

int exec_program(Node *root) {
    int status = exec_block(root);
    set_status(status);
    g_break_flag = g_continue_flag = g_return_flag = false;
    jobs_reap(true);
    return status;
}

int exec_source_string(const char *src) {
    Node *root; char *err;
    ParseStatus st = parse_program(src, &root, &err);
    if (st == PARSE_OK) {
        int status = exec_program(root);
        node_free(root);
        return status;
    }
    if (st == PARSE_EMPTY) return 0;
    if (st == PARSE_INCOMPLETE) { sh_err("syntax error: unexpected end of input\n"); return 2; }
    sh_err("%s\n", err);
    free(err);
    return 2;
}

int exec_source_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    dstr_t buf; ds_init(&buf);
    char chunk[4096];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) ds_append_n(&buf, chunk, r);
    fclose(f);
    int status = exec_source_string(buf.data);
    ds_free(&buf);
    return status;
}

int exec_capture_stdout(const char *src, char **out_buf) {
    if (g_exec_depth > 200) { *out_buf = xstrdup(""); return 1; }
    int pfd[2];
    if (pipe(pfd) != 0) { *out_buf = xstrdup(""); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        signal(SIGINT, SIG_DFL);
        g_shell_is_interactive = false;
        g_exec_depth++;
        int st = exec_source_string(src);
        flush_exit(st);
    }
    close(pfd[1]);
    dstr_t buf; ds_init(&buf);
    char chunk[4096];
    ssize_t r;
    while ((r = read(pfd[0], chunk, sizeof(chunk))) > 0) ds_append_n(&buf, chunk, (size_t)r);
    close(pfd[0]);
    int status;
    waitpid(pid, &status, 0);
    while (buf.len > 0 && buf.data[buf.len - 1] == '\n') buf.data[--buf.len] = '\0';
    *out_buf = xstrdup(buf.data);
    ds_free(&buf);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void exec_init(void) {
    set_status(0);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)getpid());
    var_set("$", buf, false);
    snprintf(buf, sizeof(buf), "%d", (int)getppid());
    var_set("PPID", buf, false);
    snprintf(buf, sizeof(buf), "%d", (int)getuid());
    var_set("UID", buf, false);
    const char *lvl = var_get("SHLVL");
    snprintf(buf, sizeof(buf), "%d", (lvl ? atoi(lvl) : 0) + 1);
    var_set("SHLVL", buf, true);
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) var_set("PWD", cwd, true);
    if (!g_pos_init) { sv_init(&g_pos); g_pos_init = true; array_set("argv", &g_pos); }
}

/* ---------- best-effort node -> string for job listings ---------- */

static void node_to_string_r(Node *n, dstr_t *out) {
    if (!n) return;
    switch (n->type) {
        case N_CMD:
            for (size_t i = 0; i < n->argv.count; i++) {
                if (i) ds_append_c(out, ' ');
                ds_append(out, n->argv.items[i]);
            }
            return;
        case N_PIPELINE:
            for (size_t i = 0; i < n->nchildren; i++) {
                if (i) ds_append(out, " | ");
                node_to_string_r(n->children[i], out);
            }
            return;
        case N_AND:
            node_to_string_r(n->children[0], out); ds_append(out, " && "); node_to_string_r(n->children[1], out);
            return;
        case N_OR:
            node_to_string_r(n->children[0], out); ds_append(out, " || "); node_to_string_r(n->children[1], out);
            return;
        default:
            ds_append(out, "...");
            return;
    }
}

static char *node_to_string(Node *n) {
    dstr_t d; ds_init(&d);
    node_to_string_r(n, &d);
    char *r = xstrdup(d.data);
    ds_free(&d);
    return r;
}

/* ---------- fish events ---------- */

static void emit_hook(const char *tag, const char *name, int argc, char **argv) {
    static int depth = 0;
    if (depth > 4) return;
    strvec_t names; sv_init(&names);
    func_list_names(&names);
    dstr_t want; ds_init(&want);
    ds_append(&want, tag); ds_append(&want, name);
    for (size_t i = 0; i < names.count; i++) {
        Node *fn = func_get(names.items[i]);
        if (!fn) continue;
        bool hit = false;
        for (size_t k = 0; k < fn->argv.count; k++) if (strcmp(fn->argv.items[k], want.data) == 0) hit = true;
        if (!hit) continue;
        strvec_t av; sv_init(&av);
        sv_push_dup(&av, names.items[i]);
        if (strcmp(tag, "var:") == 0) { sv_push_dup(&av, "VARIABLE"); sv_push_dup(&av, "SET"); sv_push_dup(&av, name); }
        else { for (int a = 0; a < argc; a++) sv_push_dup(&av, argv[a]); }
        char **c = sv_to_argv(&av);
        int saved = g_exit_code;
        depth++;
        exec_call_function(fn, (int)av.count, c);
        depth--;
        set_status(saved);
        free(c);
        sv_free(&av);
    }
    ds_free(&want);
    sv_free(&names);
}

void exec_emit_event(const char *name, int argc, char **argv) { emit_hook("event:", name, argc, argv); }
void exec_emit_var(const char *name) { emit_hook("var:", name, 0, NULL); }
