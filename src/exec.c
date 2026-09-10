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

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fnmatch.h>
#include <regex.h>

int g_exec_depth = 0;
volatile bool g_should_exit = false;
int g_exit_code = 0;
bool g_shell_is_interactive = false;
pid_t g_shell_pgid = 0;

static bool g_break_flag = false;
static bool g_continue_flag = false;
static bool g_return_flag = false;

void exec_signal_break(void) { g_break_flag = true; }
void exec_signal_continue(void) { g_continue_flag = true; }
void exec_signal_return(int code) { g_return_flag = true; g_exit_code = code; }

static int exec_stmt(Node *n);

static void flush_exit(int code) { fflush(NULL); _exit(code); }

static void set_status(int st) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", st);
    var_set("?", buf, false);
    g_exit_code = st;
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

static int apply_redirects(Redirect *r) {
    for (; r; r = r->next) {
        if (r->type == R_ERR_TO_OUT) { dup2(STDOUT_FILENO, STDERR_FILENO); continue; }

        if (r->type == R_HEREDOC) {
            char *body = r->heredoc_no_expand ? xstrdup(r->heredoc_body) : expand_heredoc_body(r->heredoc_body);
            int pfd[2];
            if (pipe(pfd) != 0) {
                fprintf(stderr, "spire: pipe: %s\n", strerror(errno));
                free(body);
                return -1;
            }
            /* write from a child so a heredoc body larger than the pipe
             * buffer can't deadlock us */
            pid_t wpid = fork();
            if (wpid == 0) {
                close(pfd[0]);
                size_t len = strlen(body);
                size_t off = 0;
                while (off < len) {
                    ssize_t w = write(pfd[1], body + off, len - off);
                    if (w <= 0) break;
                    off += (size_t)w;
                }
                close(pfd[1]);
                _exit(0);
            }
            close(pfd[1]);
            dup2(pfd[0], STDIN_FILENO);
            close(pfd[0]);
            free(body);
            continue;
        }

        char *target = expand_word_single(r->target);
        int fd, target_fd, flags;
        switch (r->type) {
            case R_IN:         flags = O_RDONLY; target_fd = STDIN_FILENO; break;
            case R_OUT:         flags = O_WRONLY | O_CREAT | O_TRUNC;  target_fd = STDOUT_FILENO; break;
            case R_APPEND:      flags = O_WRONLY | O_CREAT | O_APPEND; target_fd = STDOUT_FILENO; break;
            case R_ERR:         flags = O_WRONLY | O_CREAT | O_TRUNC;  target_fd = STDERR_FILENO; break;
            case R_ERR_APPEND:  flags = O_WRONLY | O_CREAT | O_APPEND; target_fd = STDERR_FILENO; break;
            case R_OUT_ERR:     flags = O_WRONLY | O_CREAT | O_TRUNC;  target_fd = -1; break;
            default:            flags = O_RDONLY; target_fd = STDIN_FILENO; break;
        }
        fd = open(target, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "spire: %s: %s\n", target, strerror(errno));
            free(target);
            return -1;
        }
        free(target);
        if (r->type == R_OUT_ERR) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
        else { dup2(fd, target_fd); close(fd); }
    }
    return 0;
}

/* ---------- command resolution & execution ---------- */

static char **argv_from_strvec(strvec_t *v) { return sv_to_argv(v); }

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
        free(cargv);
        return st;
    }
    if (allow_exec_replace) {
        execvp(name, cargv);
        fprintf(stderr, "spire: %s: %s\n", name, strerror(errno));
        free(cargv);
        _exit(127);
    } else {
        pid_t pid = fork();
        if (pid < 0) { fprintf(stderr, "spire: fork: %s\n", strerror(errno)); free(cargv); return 1; }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            setpgid(0, 0);
            execvp(name, cargv);
            fprintf(stderr, "spire: %s: %s\n", name, strerror(errno));
            flush_exit(127);
        }
        setpgid(pid, pid);
        if (g_shell_is_interactive) { tcsetpgrp(STDIN_FILENO, pid); }
        int status;
        waitpid(pid, &status, WUNTRACED);
        if (g_shell_is_interactive) { tcsetpgrp(STDIN_FILENO, g_shell_pgid); }
        free(cargv);
        if (WIFSTOPPED(status)) { job_add(pid, name)->state = JOB_STOPPED; return 128 + SIGTSTP; }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
}

static bool is_arith_cmd(Node *n) {
    return n->type == N_CMD && n->argv.count == 2 && strcmp(n->argv.items[0], "((") == 0;
}

static int exec_simple_command(Node *n) {
    if (n->argv.count == 0) {
        /* pure variable/array assignment(s), persists in the shell */
        for (size_t i = 0; i < n->env_assigns.count; i++)
            do_permanent_assignment(n->env_assigns.items[i]);
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
    expand_argv_with_aliases(&n->argv, &argv, 0);
    if (argv.count == 0) { sv_free(&argv); return 0; }

    SavedVar *saved; int nsaved;
    apply_env_assigns(&n->env_assigns, &saved, &nsaved);

    int status;
    bool is_ext = func_get(argv.items[0]) == NULL && builtin_lookup(argv.items[0]) == NULL;

    if (n->redirects == NULL && !is_ext) {
        status = run_in_current_process(&argv, false);
    } else if (n->redirects == NULL && is_ext) {
        status = run_in_current_process(&argv, false);
    } else {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "spire: fork: %s\n", strerror(errno));
            status = 1;
        } else if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            setpgid(0, 0);
            if (apply_redirects(n->redirects) != 0) flush_exit(1);
            int st = run_in_current_process(&argv, true);
            flush_exit(st);
        } else {
            setpgid(pid, pid);
            if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, pid);
            int wstatus;
            waitpid(pid, &wstatus, WUNTRACED);
            if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, g_shell_pgid);
            if (WIFSTOPPED(wstatus)) { job_add(pid, argv.items[0])->state = JOB_STOPPED; status = 128 + SIGTSTP; }
            else status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);
        }
    }

    restore_env_assigns(saved, nsaved);
    sv_free(&argv);
    return status;
}

/* single node used as one stage of a pipeline; called already inside a
 * forked child, so builtins/functions are safe to run in-process, and
 * external commands may exec() directly. */
static int exec_pipeline_stage(Node *n) {
    if (n->type != N_CMD) return exec_stmt(n);
    if (is_arith_cmd(n)) { long v = arith_eval(n->argv.items[1]); return v != 0 ? 0 : 1; }
    if (apply_redirects(n->redirects) != 0) flush_exit(1);
    strvec_t argv; sv_init(&argv);
    expand_argv_with_aliases(&n->argv, &argv, 0);
    if (argv.count == 0) { sv_free(&argv); return 0; }
    SavedVar *saved; int nsaved;
    apply_env_assigns(&n->env_assigns, &saved, &nsaved);
    int st = run_in_current_process(&argv, true);
    restore_env_assigns(saved, nsaved);
    sv_free(&argv);
    return st;
}

static int exec_pipeline(Node *n) {
    size_t k = n->nchildren;
    if (k == 1) return exec_stmt(n->children[0]);

    int (*pipes)[2] = xmalloc(sizeof(int[2]) * (k - 1));
    for (size_t i = 0; i < k - 1; i++) {
        if (pipe(pipes[i]) != 0) { fprintf(stderr, "spire: pipe: %s\n", strerror(errno)); free(pipes); return 1; }
    }
    pid_t *pids = xmalloc(sizeof(pid_t) * k);
    pid_t pgid = 0;

    for (size_t i = 0; i < k; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            setpgid(0, pgid); /* first stage: pgid==0 -> becomes its own leader */
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            if (i > 0) dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < k - 1) dup2(pipes[i][1], STDOUT_FILENO);
            for (size_t j = 0; j < k - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            int st = exec_pipeline_stage(n->children[i]);
            flush_exit(st);
        }
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid);
        pids[i] = pid;
    }
    for (size_t j = 0; j < k - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); }

    if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, pgid);
    int status = 0;
    for (size_t i = 0; i < k; i++) {
        int st;
        waitpid(pids[i], &st, 0);
        if (i == k - 1) status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, g_shell_pgid);

    free(pipes);
    free(pids);
    jobs_reap(false);
    return status;
}

/* ---------- background jobs ---------- */

static char *node_to_string(Node *n); /* best-effort, forward declared */

static int exec_background(Node *n) {
    n->background = false; /* avoid re-triggering once forked */
    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "spire: fork: %s\n", strerror(errno)); n->background = true; return 1; }
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        int st = exec_stmt(n);
        flush_exit(st);
    }
    setpgid(pid, pid);
    char *desc = node_to_string(n);
    Job *j = job_add(pid, desc);
    printf("[%d] %d\n", j->id, (int)pid);
    free(desc);
    n->background = true;
    return 0;
}

/* ---------- control flow ---------- */

int exec_block(Node *block) {
    int status = 0;
    for (size_t i = 0; i < block->nchildren; i++) {
        status = exec_stmt(block->children[i]);
        set_status(status);
        if (g_should_exit || g_break_flag || g_continue_flag || g_return_flag) return status;
    }
    return status;
}

static int exec_if(Node *n) {
    int cond = exec_stmt(n->children[0]);
    if (g_should_exit || g_break_flag || g_continue_flag || g_return_flag) return cond;
    if (cond == 0) return exec_block(n->children[1]);
    if (n->nchildren > 2) return exec_block(n->children[2]);
    return 0;
}

static int exec_while(Node *n) {
    int status = 0;
    for (;;) {
        int cond = exec_stmt(n->children[0]);
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
        int cond = exec_stmt(n->children[0]);
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
    if (g_exec_depth > 200) { fprintf(stderr, "spire: function call depth exceeded\n"); return 1; }
    g_exec_depth++;
    var_push_scope();

    /* save positional params to restore after the call (poor man's scoping) */
    strvec_t save_names, save_vals; sv_init(&save_names); sv_init(&save_vals);
    char buf[16];
    for (int i = 0; i <= argc && i < 64; i++) {
        snprintf(buf, sizeof(buf), "%d", i);
        const char *old = var_get(buf);
        sv_push_dup(&save_names, buf);
        sv_push_dup(&save_vals, old ? old : "\x01\x01NOTSET");
    }
    var_set("0", argv[0], false);
    for (int i = 1; i < argc; i++) { snprintf(buf, sizeof(buf), "%d", i); var_set(buf, argv[i], false); }
    snprintf(buf, sizeof(buf), "%d", argc > 0 ? argc - 1 : 0);
    var_set("#", buf, false);

    dstr_t joined; ds_init(&joined);
    for (int i = 1; i < argc; i++) { if (i > 1) ds_append_c(&joined, ' '); ds_append(&joined, argv[i]); }
    var_set("@", joined.data, false);
    var_set("*", joined.data, false);
    var_set("argv", joined.data, false);
    ds_free(&joined);

    int status = exec_block(body);
    if (g_return_flag) g_return_flag = false;
    /* break/continue escaping a function body is a script bug; swallow it
     * at the function boundary rather than corrupting the caller's loop */
    g_break_flag = false;
    g_continue_flag = false;

    for (size_t i = 0; i < save_names.count; i++) {
        if (strcmp(save_vals.items[i], "\x01\x01NOTSET") == 0) var_unset(save_names.items[i]);
        else var_set(save_names.items[i], save_vals.items[i], false);
    }
    sv_free(&save_names);
    sv_free(&save_vals);

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
    if (pid < 0) { fprintf(stderr, "spire: fork: %s\n", strerror(errno)); return 1; }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        setpgid(0, 0);
        int st = exec_block(n->children[0]);
        flush_exit(st);
    }
    setpgid(pid, pid);
    if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, pid);
    int status;
    waitpid(pid, &status, WUNTRACED);
    if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    if (WIFSTOPPED(status)) { job_add(pid, "(subshell)")->state = JOB_STOPPED; return 128 + SIGTSTP; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* ---------- dispatcher ---------- */

static int exec_stmt(Node *n) {
    if (!n) return 0;
    if (n->background && (n->type == N_CMD || n->type == N_PIPELINE || n->type == N_AND ||
                           n->type == N_OR || n->type == N_SUBSHELL || n->type == N_CASE ||
                           n->type == N_CONDATOM)) {
        return exec_background(n);
    }
    switch (n->type) {
        case N_CMD: { int s = exec_simple_command(n); return n->negate ? (s == 0) : s; }
        case N_PIPELINE: { int s = exec_pipeline(n); return n->negate ? (s == 0) : s; }
        case N_AND: {
            int l = exec_stmt(n->children[0]);
            if (g_should_exit || g_break_flag || g_continue_flag || g_return_flag) return l;
            int s = (l != 0) ? l : exec_stmt(n->children[1]);
            return n->negate ? (s == 0) : s;
        }
        case N_OR: {
            int l = exec_stmt(n->children[0]);
            if (g_should_exit || g_break_flag || g_continue_flag || g_return_flag) return l;
            int s = (l == 0) ? l : exec_stmt(n->children[1]);
            return n->negate ? (s == 0) : s;
        }
        case N_SEQ: return exec_block(n);
        case N_BLOCK: return exec_block(n);
        case N_IF: return exec_if(n);
        case N_WHILE: return exec_while(n);
        case N_UNTIL: return exec_until(n);
        case N_FOR: return exec_for(n);
        case N_CASE: return exec_case(n);
        case N_SUBSHELL: { int s = exec_subshell(n); return n->negate ? (s == 0) : s; }
        case N_CONDATOM: return exec_cond_atom(n);
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
    if (st == PARSE_INCOMPLETE) { fprintf(stderr, "spire: syntax error: unexpected end of input\n"); return 2; }
    fprintf(stderr, "spire: %s\n", err);
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
