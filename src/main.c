#include "common.h"
#include "vars.h"
#include "arrays.h"
#include "aliases.h"
#include "funcs.h"
#include "builtins.h"
#include "jobs.h"
#include "config.h"
#include "history.h"
#include "modules.h"
#include "fishfuncs.h"
#include "lineedit.h"
#include "exec.h"
#include "parser.h"
#include "ast.h"

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <ctype.h>
#include <locale.h>

#define SPIRE_VERSION "1.1.0"

static void init_shell_process(bool interactive) {
    if (interactive) {
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        if (getpgrp() != getpid()) setpgid(0, 0);
        g_shell_pgid = getpid();
        tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    }
    g_shell_is_interactive = interactive;
}

static void apply_scheme(const char *name) {
    BuiltinFn cs = builtin_lookup("colorscheme");
    char *argv[] = { (char *)"colorscheme", (char *)name, NULL };
    if (cs) cs(2, argv);
}

static void init_subsystems(void) {
    vars_init();
    exec_init();
    arrays_init();
    aliases_init();
    funcs_init();
    builtins_init();
    jobs_init();
    config_init();
    config_scheme_hook = apply_scheme;
    config_load_default();
    history_init(config_history_size(), config_history_path());
    lineedit_init();
    fishfuncs_install();
    modules_load_enabled();

    var_set("SPIRE_VERSION", SPIRE_VERSION, true);
    if (!var_get("PATH")) var_set("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", true);
}

/* bash-style history expansion on an interactive line: !! (last command),
 * !$ (its last word), !^ (its first argument), !* (all its arguments),
 * !n / !-n (by number), !prefix (most recent starting with prefix), and a
 * leading ^old^new quick substitution. Leaves the text alone inside single
 * quotes. Returns NULL if nothing was expanded, or a new string; sets
 * *failed when an event can't be found. */
static char *last_word_of(const char *line, bool first_arg, bool all_args) {
    strvec_t words; sv_init(&words);
    const char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s0 = p;
        char q = 0;
        while (*p && (q || !isspace((unsigned char)*p))) {
            if (!q && (*p == '\'' || *p == '"')) q = *p;
            else if (q && *p == q) q = 0;
            p++;
        }
        sv_push(&words, xstrndup(s0, (size_t)(p - s0)));
    }
    char *r;
    if (words.count == 0) r = xstrdup("");
    else if (all_args) {
        dstr_t d; ds_init(&d);
        for (size_t i = 1; i < words.count; i++) { if (i > 1) ds_append_c(&d, ' '); ds_append(&d, words.items[i]); }
        r = xstrdup(d.data); ds_free(&d);
    }
    else if (first_arg) r = xstrdup(words.count > 1 ? words.items[1] : "");
    else r = xstrdup(words.items[words.count - 1]);
    sv_free(&words);
    return r;
}

static char *history_expand(const char *line, bool *failed) {
    *failed = false;
    const char *prev = history_get_relative(1);
    if (line[0] == '^') {
        const char *mid = strchr(line + 1, '^');
        if (!mid || !prev) { *failed = true; return NULL; }
        char *old = xstrndup(line + 1, (size_t)(mid - line - 1));
        const char *rest = mid + 1;
        size_t rl = strlen(rest);
        char *neu = xstrndup(rest, (rl && rest[rl-1] == '^') ? rl - 1 : rl);
        const char *at = *old ? strstr(prev, old) : NULL;
        char *res = NULL;
        if (!at) *failed = true;
        else {
            dstr_t d; ds_init(&d);
            ds_append_n(&d, prev, (size_t)(at - prev));
            ds_append(&d, neu);
            ds_append(&d, at + strlen(old));
            res = xstrdup(d.data); ds_free(&d);
        }
        free(old); free(neu);
        return res;
    }
    if (!strchr(line, '!')) return NULL;
    dstr_t d; ds_init(&d);
    bool changed = false, insq = false;
    for (const char *p = line; *p; p++) {
        if (*p == '\\' && p[1]) { ds_append_c(&d, *p); ds_append_c(&d, *++p); continue; }
        if (*p == '\'') insq = !insq;
        if (*p != '!' || insq || !p[1] || isspace((unsigned char)p[1]) || p[1] == '=' || p[1] == '(') { ds_append_c(&d, *p); continue; }
        const char *ev = NULL;
        const char *q = p + 1;
        if (*q == '!') { ev = prev; q++; }
        else if (*q == '$' || *q == '^' || *q == '*') {
            if (!prev) { *failed = true; break; }
            char *w = last_word_of(prev, *q == '^', *q == '*');
            ds_append(&d, w); free(w);
            p = q; changed = true;
            continue;
        }
        else if (isdigit((unsigned char)*q) || (*q == '-' && isdigit((unsigned char)q[1]))) {
            bool rel = *q == '-';
            if (rel) q++;
            long n = strtol(q, NULL, 10);
            while (isdigit((unsigned char)*q)) q++;
            ev = rel ? history_get_relative((int)n) : history_get((size_t)(n - 1));
        }
        else if (isalpha((unsigned char)*q) || *q == '_' || *q == '.' || *q == '/') {
            const char *s0 = q;
            while (*q && !isspace((unsigned char)*q) && *q != ';' && *q != '|' && *q != '&') q++;
            char *prefix = xstrndup(s0, (size_t)(q - s0));
            for (int back = 1; (ev = history_get_relative(back)); back++) if (str_has_prefix(ev, prefix)) break;
            free(prefix);
        }
        else { ds_append_c(&d, *p); continue; }
        if (!ev) { *failed = true; break; }
        /* optional word designator :$ :^ :* */
        if (q[0] == ':' && (q[1] == '$' || q[1] == '^' || q[1] == '*')) {
            char *w = last_word_of(ev, q[1] == '^', q[1] == '*');
            ds_append(&d, w); free(w);
            q += 2;
        } else ds_append(&d, ev);
        p = q - 1;
        changed = true;
    }
    if (*failed || !changed) { ds_free(&d); return NULL; }
    char *r = xstrdup(d.data);
    ds_free(&d);
    return r;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void source_rc_files(void) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/config.spire", config_dir());
    exec_source_file(path);
    const char *home = var_get("HOME");
    if (home) { snprintf(path, sizeof(path), "%s/.spirerc", home); exec_source_file(path); }
}

static void repl(void) {
    init_shell_process(true);
    source_rc_files();
    while (!g_should_exit) {
        jobs_reap(true);
        exec_run_pending_traps();
        char *line = lineedit_read(config_prompt());
        if (!line) {
            if (jobs_count() > 0 && !getenv("SPIRE_NO_JOB_WARNING")) {
                static bool warned = false;
                if (!warned) { warned = true; fprintf(stderr, "\nthere are stopped/running jobs; press Ctrl-D again to exit\n"); continue; }
            }
            printf("\n");
            break;
        }

        dstr_t acc; ds_init(&acc);
        ds_append(&acc, line);
        free(line);

        Node *root = NULL;
        char *err = NULL;
        ParseStatus st;
        for (;;) {
            st = parse_program(acc.data, &root, &err);
            if (st != PARSE_INCOMPLETE) break;
            char *cont = lineedit_read(NULL);
            if (!cont) break;
            ds_append_c(&acc, '\n');
            ds_append(&acc, cont);
            free(cont);
        }

        /* history expansion happens on the complete text, before parsing for real */
        bool hfail = false;
        char *hx = history_expand(acc.data, &hfail);
        if (hfail) {
            sh_err("event not found\n");
            if (root) node_free(root);
            free(err);
            ds_free(&acc);
            continue;
        }
        if (hx) {
            if (root) { node_free(root); root = NULL; }
            free(err); err = NULL;
            ds_clear(&acc); ds_append(&acc, hx); free(hx);
            printf("%s\n", acc.data);
            st = parse_program(acc.data, &root, &err);
        }

        if (acc.len > 0 && acc.data[0] != ' ') history_add(acc.data); /* leading space: keep out of history */

        if (st == PARSE_OK) {
            long t0 = now_ms();
            exec_program(root);
            g_last_duration_ms = now_ms() - t0;
            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", g_last_duration_ms);
            var_set("CMD_DURATION", buf, false);
            node_free(root);
        } else if (st == PARSE_ERROR) {
            sh_err("%s\n", err);
            free(err);
            var_set("?", "2", false);
            var_set("status", "2", false);
            g_exit_code = 2;
        } else if (st == PARSE_INCOMPLETE) {
            sh_err("syntax error: unexpected end of input\n");
        }
        ds_free(&acc);
    }
    lineedit_shutdown();
}

static void set_positional(int argc, char **argv, int start) {
    exec_set_positional(argc > start ? argc - start : 0, argv + start);
}

static void print_help(void) {
    printf(
        "spire " SPIRE_VERSION " - Syntax-Polyglot Interactive Runtime Environment\n\n"
        "usage:\n"
        "  spire                    start an interactive shell\n"
        "  spire -c 'commands'      run a command string and exit\n"
        "  spire script.sp [args]   run a script file\n"
        "  spire -i | -l            start interactively (login shells are accepted)\n"
        "  spire --version          print the version\n"
        "  spire --help             this message\n");
}

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    setvbuf(stdout, NULL, _IOLBF, 0);
    init_subsystems();

    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_help();
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "--login") == 0 ||
                      strcmp(argv[1], "-i") == 0 || strcmp(argv[1], "--interactive") == 0)) {
        argc = 1; /* just start interactively */
    }
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("spire %s\n", SPIRE_VERSION);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        init_shell_process(false);
        set_positional(argc, argv, 3);
        var_set("0", "spire", false);
        int st = exec_source_string(argv[2]);
        st = g_should_exit ? g_exit_code : st;
        g_exit_code = st;
        exec_run_exit_trap();
        return st;
    }
    if (argc >= 2 && argv[1][0] != '-') {
        init_shell_process(false);
        var_set("0", argv[1], false);
        set_positional(argc, argv, 2);
        int st = exec_source_file(argv[1]);
        if (st < 0) { sh_err("%s: no such file\n", argv[1]); return 127; }
        st = g_should_exit ? g_exit_code : st;
        g_exit_code = st;
        exec_run_exit_trap();
        return st;
    }

    if (!isatty(STDIN_FILENO)) {
        init_shell_process(false);
        var_set("0", "spire", false);
        dstr_t buf; ds_init(&buf);
        char chunk[4096]; size_t r;
        while ((r = fread(chunk, 1, sizeof(chunk), stdin)) > 0) ds_append_n(&buf, chunk, r);
        int st = exec_source_string(buf.data);
        ds_free(&buf);
        st = g_should_exit ? g_exit_code : st;
        g_exit_code = st;
        exec_run_exit_trap();
        return st;
    }

    repl();
    exec_run_exit_trap();
    return g_exit_code;
}
