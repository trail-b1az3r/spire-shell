/* builtins_extra.c - the larger set of bash- and fish-compatible builtins:
 * declarations (set/declare/local/export), I/O (echo/printf/read),
 * evaluation (eval/exec/command/builtin/source), job & signal control
 * (jobs/fg/bg/wait/kill/disown/trap), the directory stack, getopts, and
 * fish's string/math/count/contains/random/status/abbr. */
#include "builtins.h"
#include "exec.h"
#include "vars.h"
#include "arrays.h"
#include "arith.h"
#include "aliases.h"
#include "funcs.h"
#include "jobs.h"
#include "history.h"
#include "highlight.h"
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <math.h>
#include <regex.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static bool out_color(void) { return isatty(STDOUT_FILENO); }

/* =========================== signals =========================== */

typedef struct { const char *name; int num; } SigName;
static const SigName g_signals[] = {
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL}, {"TRAP", SIGTRAP},
    {"ABRT", SIGABRT}, {"BUS", SIGBUS}, {"FPE", SIGFPE}, {"KILL", SIGKILL}, {"USR1", SIGUSR1},
    {"SEGV", SIGSEGV}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM},
    {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN},
    {"TTOU", SIGTTOU}, {"URG", SIGURG}, {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ}, {"VTALRM", SIGVTALRM},
    {"PROF", SIGPROF}, {"WINCH", SIGWINCH}, {"SYS", SIGSYS},
    {NULL, 0}
};

/* "TERM", "SIGTERM", "term", "15" -> 15; "EXIT" -> 0; "ERR" -> TRAP_ERR; -1 if unknown */
static int signal_from_name(const char *s) {
    if (isdigit((unsigned char)s[0])) return atoi(s);
    char up[32];
    size_t i = 0;
    for (; s[i] && i < sizeof(up) - 1; i++) up[i] = (char)toupper((unsigned char)s[i]);
    up[i] = '\0';
    const char *n = up;
    if (strncmp(n, "SIG", 3) == 0) n += 3;
    if (strcmp(n, "EXIT") == 0) return 0;
    if (strcmp(n, "ERR") == 0) return TRAP_ERR;
    for (int k = 0; g_signals[k].name; k++) if (strcmp(g_signals[k].name, n) == 0) return g_signals[k].num;
    return -1;
}

static const char *signal_name(int num) {
    if (num == 0) return "EXIT";
    if (num == TRAP_ERR) return "ERR";
    for (int k = 0; g_signals[k].name; k++) if (g_signals[k].num == num) return g_signals[k].name;
    return "?";
}

static void list_signals(void) {
    for (int k = 0; g_signals[k].name; k++) printf("%2d) SIG%-8s%s", g_signals[k].num, g_signals[k].name, (k % 5 == 4) ? "\n" : " ");
    printf("\n");
}

/* =========================== set =========================== */

static void print_var_line(const char *name) {
    const char *v = var_get(name);
    if (out_color()) printf("\x1b[36m%s\x1b[0m %s\n", name, v ? v : "");
    else printf("%s %s\n", name, v ? v : "");
}

static int cmp_strp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void set_list_all(bool names_only) {
    strvec_t names; sv_init(&names);
    vars_dump(&names);
    qsort(names.items, names.count, sizeof(char *), cmp_strp);
    for (size_t i = 0; i < names.count; i++) {
        if (!isalpha((unsigned char)names.items[i][0]) && names.items[i][0] != '_') continue;
        if (names_only) printf("%s\n", names.items[i]); else print_var_line(names.items[i]);
    }
    sv_free(&names);
    strvec_t anames; sv_init(&anames);
    arrays_dump(&anames);
    qsort(anames.items, anames.count, sizeof(char *), cmp_strp);
    for (size_t i = 0; i < anames.count; i++) {
        if (names_only) { printf("%s\n", anames.items[i]); continue; }
        strvec_t elems; sv_init(&elems);
        array_get_all(anames.items[i], &elems);
        if (out_color()) printf("\x1b[35m%s\x1b[0m", anames.items[i]); else printf("%s", anames.items[i]);
        for (size_t k = 0; k < elems.count; k++) printf(" '%s'", elems.items[k]);
        printf("\n");
        sv_free(&elems);
    }
    sv_free(&anames);
}

static bool apply_shell_option(char flag, bool on) {
    switch (flag) {
        case 'e': g_opt_errexit = on; return true;
        case 'x': g_opt_xtrace = on; return true;
        case 'u': g_opt_nounset = on; return true;
        case 'v': case 'f': case 'h': case 'm': case 'B': case 'H': case 'b': case 'C': return true; /* accepted */
        default: return false;
    }
}

static bool apply_long_option(const char *name, bool on) {
    if (strcmp(name, "pipefail") == 0) { g_opt_pipefail = on; return true; }
    if (strcmp(name, "errexit") == 0) { g_opt_errexit = on; return true; }
    if (strcmp(name, "xtrace") == 0) { g_opt_xtrace = on; return true; }
    if (strcmp(name, "nounset") == 0) { g_opt_nounset = on; return true; }
    /* emacs/vi/noclobber/... accepted for script compatibility */
    return true;
}

static void print_shell_options(void) {
    printf("errexit   %s\npipefail  %s\nxtrace    %s\nnounset   %s\n",
           g_opt_errexit ? "on" : "off", g_opt_pipefail ? "on" : "off",
           g_opt_xtrace ? "on" : "off", g_opt_nounset ? "on" : "off");
}

/* Is argv[1..] a bash-style `set` option invocation (set -e, set -euo
 * pipefail, set +x, set -o name) rather than fish's `set -x NAME val`? */
static bool is_bash_set(int argc, char **argv) {
    const char *a = argv[1];
    if (a[0] == '+') return true;
    if (a[0] != '-' || !a[1] || a[1] == '-') return false;
    bool has_o = false;
    for (const char *p = a + 1; *p; p++) {
        if (!strchr("euxvfhmBHbCo", *p)) return false;
        if (*p == 'o') has_o = true;
    }
    if (has_o) return true;
    return argc == 2; /* `set -e` alone; `set -e NAME` is fish erase */
}

static int b_set(int argc, char **argv) {
    if (argc == 1) { set_list_all(false); return 0; }

    if (strcmp(argv[1], "--") == 0) { exec_set_positional(argc - 2, argv + 2); return 0; }

    if (is_bash_set(argc, argv)) {
        int i = 1;
        for (; i < argc; i++) {
            const char *a = argv[i];
            if (strcmp(a, "--") == 0) { exec_set_positional(argc - i - 1, argv + i + 1); return 0; }
            if (a[0] != '-' && a[0] != '+') { exec_set_positional(argc - i, argv + i); return 0; }
            bool on = a[0] == '-';
            for (const char *p = a + 1; *p; p++) {
                if (*p == 'o') {
                    if (i + 1 < argc) apply_long_option(argv[++i], on);
                    else print_shell_options();
                    continue;
                }
                if (!apply_shell_option(*p, on)) { sh_err("set: -%c: invalid option\n", *p); return 2; }
            }
        }
        return 0;
    }

    /* fish-style: set [-lgxuUeqanpS] NAME [VALUES...] */
    int i = 1;
    bool exported = false, unexport = false, erase = false, query = false, append = false, prepend = false;
    bool local = false, names = false, show = false;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[1] == '-') {
            if (strcmp(a, "--export") == 0) exported = true;
            else if (strcmp(a, "--unexport") == 0) unexport = true;
            else if (strcmp(a, "--erase") == 0) erase = true;
            else if (strcmp(a, "--query") == 0) query = true;
            else if (strcmp(a, "--append") == 0) append = true;
            else if (strcmp(a, "--prepend") == 0) prepend = true;
            else if (strcmp(a, "--local") == 0 || strcmp(a, "--function") == 0) local = true;
            else if (strcmp(a, "--names") == 0) names = true;
            else if (strcmp(a, "--show") == 0) show = true;
            continue;
        }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
                case 'x': exported = true; break;
                case 'u': unexport = true; break;
                case 'e': erase = true; break;
                case 'q': query = true; break;
                case 'a': append = true; break;
                case 'p': prepend = true; break;
                case 'l': case 'f': local = true; break;
                case 'n': names = true; break;
                case 'S': show = true; break;
                default: break; /* -g -U -L: scope hints, accepted */
            }
        }
    }
    if (names) { set_list_all(true); return 0; }
    if (i >= argc) { if (!query) set_list_all(false); return query ? 255 : 0; }

    if (query) {
        int missing = 0;
        for (; i < argc; i++) if (!var_get(argv[i]) && !array_exists(argv[i])) missing++;
        return missing;
    }
    const char *name = argv[i++];
    if (show) {
        if (array_exists(name)) {
            strvec_t e; sv_init(&e); array_get_all(name, &e);
            printf("$%s: set with %zu elements\n", name, e.count);
            for (size_t k = 0; k < e.count; k++) printf("$%s[%zu]: |%s|\n", name, k + 1, e.items[k]);
            sv_free(&e);
        } else if (var_get(name)) printf("$%s: set with 1 element\n$%s[1]: |%s|\n", name, name, var_get(name));
        else printf("$%s: not set\n", name);
        return 0;
    }
    if (erase) {
        bool had = var_get(name) || array_exists(name);
        var_unset(name); array_unset(name);
        return had ? 0 : 1;
    }
    if (local) var_declare_local(name);

    strvec_t vals; sv_init(&vals);
    if (append || prepend) {
        strvec_t old; sv_init(&old);
        if (array_exists(name)) array_get_all(name, &old);
        else if (var_get(name)) sv_push_dup(&old, var_get(name));
        if (prepend) for (int k = i; k < argc; k++) sv_push_dup(&vals, argv[k]);
        for (size_t k = 0; k < old.count; k++) sv_push_dup(&vals, old.items[k]);
        if (append) for (int k = i; k < argc; k++) sv_push_dup(&vals, argv[k]);
        sv_free(&old);
    } else {
        for (int k = i; k < argc; k++) sv_push_dup(&vals, argv[k]);
    }

    bool exp = (exported || var_is_exported(name)) && !unexport;
    if (vals.count == 1 || (vals.count == 0 && !array_exists(name))) {
        array_unset(name);
        if (unexport) var_unset(name);
        var_set(name, vals.count ? vals.items[0] : "", exp);
    } else {
        /* fish lists: a multi-value `set` makes an array (exported joined by ':' would
         * be fish's PATH-style behavior; we export space-joined for simplicity) */
        array_set(name, &vals);
        if (exp) {
            dstr_t d; ds_init(&d);
            for (size_t k = 0; k < vals.count; k++) { if (k) ds_append_c(&d, ' '); ds_append(&d, vals.items[k]); }
            setenv(name, d.data, 1);
            ds_free(&d);
        }
        var_unset(name);
    }
    sv_free(&vals);
    return 0;
}

/* =========================== declarations =========================== */

static void assignment_name(const char *raw, char *name, size_t cap) {
    size_t i = 0;
    while (raw[i] && (isalnum((unsigned char)raw[i]) || raw[i] == '_') && i < cap - 1) { name[i] = raw[i]; i++; }
    name[i] = '\0';
}

static void print_declaration(const char *name, bool exported_only) {
    if (array_exists(name)) {
        strvec_t e; sv_init(&e); array_get_all(name, &e);
        printf("declare -a %s=(", name);
        for (size_t k = 0; k < e.count; k++) printf("%s\"%s\"", k ? " " : "", e.items[k]);
        printf(")\n");
        sv_free(&e);
        return;
    }
    const char *v = var_get(name);
    if (!v) return;
    bool exp = var_is_exported(name);
    if (exported_only && !exp) return;
    printf("declare %s %s=\"%s\"\n", exp ? "-x" : "--", name, v);
}

/* shared by declare/typeset/local/export/readonly */
static int do_declare(int argc, char **argv, bool force_local, bool force_export) {
    bool exported = force_export, array = false, print = false, funcs = false, unexport = false;
    int i = 1;
    for (; i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        bool on = argv[i][0] == '-';
        for (const char *p = argv[i] + 1; *p; p++) {
            switch (*p) {
                case 'x': if (on) exported = true; else unexport = true; break;
                case 'a': case 'A': array = true; break;
                case 'p': print = true; break;
                case 'f': case 'F': funcs = true; break;
                case 'n': if (force_export && !on) unexport = true; else unexport = true; break;
                default: break; /* -i -r -g -l -u -t: accepted */
            }
        }
    }
    if (funcs) {
        strvec_t names; sv_init(&names);
        func_list_names(&names);
        for (size_t k = 0; k < names.count; k++) printf("declare -f %s\n", names.items[k]);
        sv_free(&names);
        return 0;
    }
    if (i >= argc || print) {
        strvec_t names; sv_init(&names);
        if (i < argc) { for (; i < argc; i++) sv_push_dup(&names, argv[i]); }
        else { vars_dump(&names); arrays_dump(&names); qsort(names.items, names.count, sizeof(char *), cmp_strp); }
        for (size_t k = 0; k < names.count; k++) print_declaration(names.items[k], force_export);
        sv_free(&names);
        return 0;
    }
    bool local = force_local || (var_in_function_scope() && !force_export && strcmp(argv[0], "readonly") != 0);
    int status = 0;
    for (; i < argc; i++) {
        char name[256];
        bool assign = exec_looks_like_assignment(argv[i]);
        if (assign) assignment_name(argv[i], name, sizeof(name));
        else snprintf(name, sizeof(name), "%s", argv[i]);
        if (!name[0]) { sh_err("%s: `%s': not a valid identifier\n", argv[0], argv[i]); status = 1; continue; }
        if (local) {
            if (!var_declare_local(name) && force_local) {
                sh_err("local: can only be used inside a function\n");
                return 1;
            }
        }
        if (assign) exec_assign(argv[i]);
        else if (array && !array_exists(name)) { strvec_t e; sv_init(&e); array_set(name, &e); sv_free(&e); }
        else if (local && !var_get(name) && !array_exists(name)) var_set(name, "", false);
        if (unexport) {
            const char *v = var_get(name);
            char *copy = v ? xstrdup(v) : NULL;
            var_unset(name);
            if (copy) { var_set(name, copy, false); free(copy); }
        } else if (exported) var_export(name);
    }
    return status;
}

static int b_declare(int argc, char **argv) { return do_declare(argc, argv, false, false); }
static int b_local(int argc, char **argv) { return do_declare(argc, argv, true, false); }
static int b_export(int argc, char **argv) { return do_declare(argc, argv, false, true); }

/* =========================== echo / printf =========================== */

/* interprets one backslash escape at *pp (pointing just after '\').
 * Returns false for \c (stop all output). */
static bool emit_escape(const char **pp, dstr_t *out, bool echo_style) {
    const char *p = *pp;
    char c = *p++;
    switch (c) {
        case 'n': ds_append_c(out, '\n'); break;
        case 't': ds_append_c(out, '\t'); break;
        case 'r': ds_append_c(out, '\r'); break;
        case 'a': ds_append_c(out, '\a'); break;
        case 'b': ds_append_c(out, '\b'); break;
        case 'f': ds_append_c(out, '\f'); break;
        case 'v': ds_append_c(out, '\v'); break;
        case 'e': case 'E': ds_append_c(out, 27); break;
        case '\\': ds_append_c(out, '\\'); break;
        case '"': ds_append_c(out, '"'); break;
        case 'c': *pp = p; return false;
        case 'x': {
            int v = 0, k = 0;
            while (k < 2 && isxdigit((unsigned char)*p)) { v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10); p++; k++; }
            if (k == 0) { ds_append(out, "\\x"); break; }
            ds_append_c(out, (char)v);
            break;
        }
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            /* echo: \0nnn ; printf: \nnn */
            int v = 0, k = 0, maxd = 3;
            if (echo_style && c == '0') { /* digits follow the 0 */ }
            else { v = c - '0'; k = 1; }
            while (k < maxd && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; k++; }
            ds_append_c(out, (char)v);
            break;
        }
        case '\0': ds_append_c(out, '\\'); p--; break;
        default: ds_append_c(out, '\\'); ds_append_c(out, c); break;
    }
    *pp = p;
    return true;
}

static int b_echo(int argc, char **argv) {
    bool nl = true, interpret = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        bool valid = true;
        for (const char *p = argv[i] + 1; *p; p++) if (!strchr("neEs", *p)) { valid = false; break; }
        if (!valid) break;
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'n') nl = false;
            else if (*p == 'e') interpret = true;
            else if (*p == 'E') interpret = false;
        }
    }
    dstr_t out; ds_init(&out);
    bool stop = false;
    for (int first = i; i < argc && !stop; i++) {
        if (i > first) ds_append_c(&out, ' ');
        if (!interpret) { ds_append(&out, argv[i]); continue; }
        for (const char *p = argv[i]; *p;) {
            if (*p == '\\' && p[1]) { p++; if (!emit_escape(&p, &out, true)) { stop = true; break; } }
            else ds_append_c(&out, *p++);
        }
    }
    if (nl && !stop) ds_append_c(&out, '\n');
    fwrite(out.data, 1, out.len, stdout);
    ds_free(&out);
    return 0;
}

static long printf_int_arg(const char *s) {
    if ((s[0] == '\'' || s[0] == '"') && s[1]) return (unsigned char)s[1]; /* 'c -> char code */
    char *end;
    long v = strtol(s, &end, 0);
    if (*s && *end) { sh_err("printf: %s: invalid number\n", s); }
    return v;
}

/* printf [-v var] FORMAT [ARGS...]; the format is reused until all
 * arguments are consumed, like bash/coreutils. */
static int b_printf(int argc, char **argv) {
    int i = 1;
    const char *target_var = NULL;
    if (i + 1 < argc && strcmp(argv[i], "-v") == 0) { target_var = argv[i + 1]; i += 2; }
    if (i < argc && strcmp(argv[i], "--") == 0) i++;
    if (i >= argc) { sh_err("printf: usage: printf [-v var] format [arguments]\n"); return 2; }
    const char *fmt = argv[i++];
    int argi = i;
    dstr_t out; ds_init(&out);
    int status = 0;
    bool stop = false;

    do {
        int consumed_before = argi;
        for (const char *p = fmt; *p && !stop;) {
            if (*p == '\\' && p[1]) { p++; if (!emit_escape(&p, &out, false)) stop = true; continue; }
            if (*p != '%') { ds_append_c(&out, *p++); continue; }
            p++;
            if (*p == '%') { ds_append_c(&out, '%'); p++; continue; }
            /* collect the spec */
            char spec[64];
            size_t sl = 0;
            spec[sl++] = '%';
            while (*p && strchr("-+ #0'", *p) && sl < 40) spec[sl++] = *p++;
            if (*p == '*') { int w = argi < argc ? (int)printf_int_arg(argv[argi++]) : 0; sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", w); p++; }
            else while (isdigit((unsigned char)*p) && sl < 50) spec[sl++] = *p++;
            if (*p == '.') {
                spec[sl++] = *p++;
                if (*p == '*') { int pr = argi < argc ? (int)printf_int_arg(argv[argi++]) : 0; sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", pr); p++; }
                else while (isdigit((unsigned char)*p) && sl < 58) spec[sl++] = *p++;
            }
            while (*p == 'l' || *p == 'h' || *p == 'L' || *p == 'q' || *p == 'j' || *p == 'z') { if (*p == 'q') break; p++; }
            char conv = *p ? *p++ : 's';
            const char *arg = argi < argc ? argv[argi++] : NULL;
            char buf[4096];
            switch (conv) {
                case 'd': case 'i': {
                    spec[sl++] = 'l'; spec[sl++] = 'd'; spec[sl] = '\0';
                    snprintf(buf, sizeof(buf), spec, arg ? printf_int_arg(arg) : 0L);
                    ds_append(&out, buf);
                    break;
                }
                case 'u': case 'o': case 'x': case 'X': {
                    spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = '\0';
                    snprintf(buf, sizeof(buf), spec, (unsigned long)(arg ? printf_int_arg(arg) : 0L));
                    ds_append(&out, buf);
                    break;
                }
                case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                    spec[sl++] = conv; spec[sl] = '\0';
                    snprintf(buf, sizeof(buf), spec, arg ? strtod(arg, NULL) : 0.0);
                    ds_append(&out, buf);
                    break;
                }
                case 'c': {
                    spec[sl++] = 'c'; spec[sl] = '\0';
                    snprintf(buf, sizeof(buf), spec, arg && arg[0] ? arg[0] : '\0');
                    ds_append(&out, buf);
                    break;
                }
                case 'b': {
                    dstr_t tmp; ds_init(&tmp);
                    for (const char *q = arg ? arg : ""; *q;) {
                        if (*q == '\\' && q[1]) { q++; if (!emit_escape(&q, &tmp, true)) { stop = true; break; } }
                        else ds_append_c(&tmp, *q++);
                    }
                    spec[sl++] = 's'; spec[sl] = '\0';
                    snprintf(buf, sizeof(buf), spec, tmp.data);
                    ds_append(&out, buf);
                    ds_free(&tmp);
                    break;
                }
                case 'q': {
                    const char *v = arg ? arg : "";
                    bool plain = *v != '\0';
                    for (const char *q = v; *q; q++) if (!isalnum((unsigned char)*q) && !strchr("_-./=:,+@%", *q)) { plain = false; break; }
                    if (plain) ds_append(&out, v);
                    else {
                        ds_append_c(&out, '\'');
                        for (const char *q = v; *q; q++) { if (*q == '\'') ds_append(&out, "'\\''"); else ds_append_c(&out, *q); }
                        ds_append_c(&out, '\'');
                    }
                    break;
                }
                case 's': default: {
                    spec[sl++] = 's'; spec[sl] = '\0';
                    const char *v = arg ? arg : "";
                    if (strlen(v) < sizeof(buf) - 64) { snprintf(buf, sizeof(buf), spec, v); ds_append(&out, buf); }
                    else ds_append(&out, v);
                    if (conv != 's') { sh_err("printf: %%%c: invalid directive\n", conv); status = 1; }
                    break;
                }
            }
        }
        if (argi == consumed_before) break; /* format consumes no args: print once */
    } while (argi < argc && !stop);

    if (target_var) var_set(target_var, out.data, var_is_exported(target_var));
    else fwrite(out.data, 1, out.len, stdout);
    ds_free(&out);
    return status;
}

/* =========================== read =========================== */

static int b_read(int argc, char **argv) {
    const char *prompt = NULL;
    bool silent = false, raw = false, to_array = false, local = false;
    int nchars = -1;
    char delim = '\n';
    double timeout = -1;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if ((strcmp(a, "-p") == 0 || strcmp(a, "-P") == 0 || strcmp(a, "--prompt-str") == 0) && i + 1 < argc) { prompt = argv[++i]; continue; }
        if (strcmp(a, "-n") == 0 && i + 1 < argc) { nchars = atoi(argv[++i]); continue; }
        if ((strcmp(a, "-d") == 0 || strcmp(a, "--delimiter") == 0) && i + 1 < argc) { delim = argv[++i][0]; continue; }
        if (strcmp(a, "-t") == 0 && i + 1 < argc) { timeout = atof(argv[++i]); continue; }
        if (strcmp(a, "-a") == 0 || strcmp(a, "--list") == 0) { to_array = true; continue; }
        if (strcmp(a, "-l") == 0 || strcmp(a, "--local") == 0) { local = true; continue; }
        for (const char *p = a + 1; *p; p++) {
            if (*p == 's') silent = true;
            else if (*p == 'r') raw = true;
            else if (*p == 'a') to_array = true;
            else if (*p == 'e') { /* readline editing: not supported, plain read */ }
        }
    }
    if (prompt && isatty(STDIN_FILENO)) { fputs(prompt, stderr); fflush(stderr); }

    struct termios saved, rawt;
    bool restore_tty = false;
    if ((silent || nchars > 0) && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        rawt = saved;
        if (silent) rawt.c_lflag &= ~(tcflag_t)ECHO;
        if (nchars > 0) { rawt.c_lflag &= ~(tcflag_t)ICANON; rawt.c_cc[VMIN] = 1; rawt.c_cc[VTIME] = 0; }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &rawt);
        restore_tty = true;
    }

    dstr_t line; ds_init(&line);
    bool got_any = false, hit_delim = false, timed_out = false;
    fflush(stdout);
    for (;;) {
        if (nchars >= 0 && (int)line.len >= nchars) break;
        if (timeout >= 0) {
            fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
            struct timeval tv = { (long)timeout, (long)((timeout - (long)timeout) * 1e6) };
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) { timed_out = true; break; }
        }
        /* byte-at-a-time from fd 0 so we never over-read input a later
         * command (or the next `read` in a loop) should see */
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got_any = true;
        if (c == delim) { hit_delim = true; break; }
        if (!raw && c == '\\') {
            char c2;
            if (read(STDIN_FILENO, &c2, 1) == 1) { if (c2 != '\n') ds_append_c(&line, c2); }
            continue;
        }
        ds_append_c(&line, c);
    }
    if (restore_tty) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved); if (silent) fputc('\n', stderr); }
    if (timed_out && !got_any) { ds_free(&line); return 142; }
    if (!got_any) { ds_free(&line); return 1; }
    while (line.len > 0 && line.data[line.len - 1] == '\r') line.data[--line.len] = '\0';

    int nvars = argc - i;
    const char *ifs = var_get("IFS");
    if (!ifs) ifs = " \t\n";
    #define IS_IFS(ch) (strchr(ifs, (ch)) != NULL && (ch) != '\0')

    if (to_array) {
        const char *name = nvars > 0 ? argv[i] : "REPLY";
        strvec_t parts; sv_init(&parts);
        const char *p = line.data;
        while (*p) {
            while (*p && IS_IFS(*p)) p++;
            if (!*p) break;
            const char *s0 = p;
            while (*p && !IS_IFS(*p)) p++;
            sv_push(&parts, xstrndup(s0, (size_t)(p - s0)));
        }
        if (local) var_declare_local(name);
        array_set(name, &parts);
        sv_free(&parts);
        ds_free(&line);
        return 0;
    }
    if (nvars <= 0) { var_set("REPLY", line.data, false); ds_free(&line); return 0; }

    /* bash semantics: split on IFS across the given names, the last name
     * absorbing whatever's left (including embedded separators). */
    char *p = line.data;
    for (int k = 0; k < nvars; k++) {
        const char *name = argv[i + k];
        if (local) var_declare_local(name);
        while (*p && IS_IFS(*p) && isspace((unsigned char)*p)) p++;
        char *start = p;
        if (k == nvars - 1) {
            char *end = p + strlen(p);
            while (end > start && IS_IFS(end[-1]) && isspace((unsigned char)end[-1])) end--;
            char *val = xstrndup(start, (size_t)(end - start));
            var_set(name, val, var_is_exported(name));
            free(val);
            break;
        }
        while (*p && !IS_IFS(*p)) p++;
        char *val = xstrndup(start, (size_t)(p - start));
        var_set(name, val, var_is_exported(name));
        free(val);
        if (*p) p++;
    }
    #undef IS_IFS
    ds_free(&line);
    (void)hit_delim;
    return 0;
}

/* =========================== test / [ =========================== */

typedef struct { char **a; int n; int pos; } TestState;

static int test_or(TestState *t);

static bool is_unary_op(const char *s) {
    static const char *ops[] = {"-f","-d","-e","-r","-w","-x","-z","-n","-s","-L","-h","-b","-c","-p","-S","-t","-g","-u","-k","-O","-G","-N","-v", NULL};
    for (int i = 0; ops[i]; i++) if (strcmp(ops[i], s) == 0) return true;
    return false;
}

static bool is_binary_op(const char *s) {
    static const char *ops[] = {"=","==","!=","<",">","-eq","-ne","-lt","-le","-gt","-ge","-nt","-ot","-ef","=~", NULL};
    for (int i = 0; ops[i]; i++) if (strcmp(ops[i], s) == 0) return true;
    return false;
}

static bool test_unary(const char *op, const char *arg) {
    struct stat st;
    switch (op[1]) {
        case 'f': return stat(arg, &st) == 0 && S_ISREG(st.st_mode);
        case 'd': return stat(arg, &st) == 0 && S_ISDIR(st.st_mode);
        case 'e': return stat(arg, &st) == 0;
        case 'r': return access(arg, R_OK) == 0;
        case 'w': return access(arg, W_OK) == 0;
        case 'x': return access(arg, X_OK) == 0;
        case 'z': return arg[0] == '\0';
        case 'n': return arg[0] != '\0';
        case 's': return stat(arg, &st) == 0 && st.st_size > 0;
        case 'L': case 'h': return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
        case 'b': return stat(arg, &st) == 0 && S_ISBLK(st.st_mode);
        case 'c': return stat(arg, &st) == 0 && S_ISCHR(st.st_mode);
        case 'p': return stat(arg, &st) == 0 && S_ISFIFO(st.st_mode);
        case 'S': return stat(arg, &st) == 0 && S_ISSOCK(st.st_mode);
        case 't': return isatty(atoi(arg));
        case 'g': return stat(arg, &st) == 0 && (st.st_mode & S_ISGID);
        case 'u': return stat(arg, &st) == 0 && (st.st_mode & S_ISUID);
        case 'k': return stat(arg, &st) == 0 && (st.st_mode & S_ISVTX);
        case 'O': return stat(arg, &st) == 0 && st.st_uid == geteuid();
        case 'G': return stat(arg, &st) == 0 && st.st_gid == getegid();
        case 'N': return stat(arg, &st) == 0 && st.st_mtime > st.st_atime;
        case 'v': return var_get(arg) != NULL || array_exists(arg);
    }
    return false;
}

static bool test_binary(const char *l, const char *op, const char *r) {
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) return strcmp(l, r) == 0;
    if (strcmp(op, "!=") == 0) return strcmp(l, r) != 0;
    if (strcmp(op, "<") == 0) return strcmp(l, r) < 0;
    if (strcmp(op, ">") == 0) return strcmp(l, r) > 0;
    if (strcmp(op, "=~") == 0) {
        regex_t re;
        if (regcomp(&re, r, REG_EXTENDED) != 0) return false;
        bool m = regexec(&re, l, 0, NULL, 0) == 0;
        regfree(&re);
        return m;
    }
    if (strcmp(op, "-nt") == 0 || strcmp(op, "-ot") == 0 || strcmp(op, "-ef") == 0) {
        struct stat sl, sr;
        bool lok = stat(l, &sl) == 0, rok = stat(r, &sr) == 0;
        if (strcmp(op, "-nt") == 0) return lok && (!rok || sl.st_mtime > sr.st_mtime);
        if (strcmp(op, "-ot") == 0) return rok && (!lok || sl.st_mtime < sr.st_mtime);
        return lok && rok && sl.st_dev == sr.st_dev && sl.st_ino == sr.st_ino;
    }
    long li = arith_eval(l), ri = arith_eval(r);
    if (strcmp(op, "-eq") == 0) return li == ri;
    if (strcmp(op, "-ne") == 0) return li != ri;
    if (strcmp(op, "-lt") == 0) return li < ri;
    if (strcmp(op, "-le") == 0) return li <= ri;
    if (strcmp(op, "-gt") == 0) return li > ri;
    if (strcmp(op, "-ge") == 0) return li >= ri;
    return false;
}

static int test_primary(TestState *t) {
    if (t->pos >= t->n) return 1;
    const char *w = t->a[t->pos];
    if (strcmp(w, "!") == 0 && t->pos + 1 < t->n) { t->pos++; return !test_primary(t) ? 1 : 0; }
    if (strcmp(w, "(") == 0) {
        t->pos++;
        int r = test_or(t);
        if (t->pos < t->n && strcmp(t->a[t->pos], ")") == 0) t->pos++;
        return r;
    }
    if (t->pos + 2 < t->n && is_binary_op(t->a[t->pos + 1])) {
        bool r = test_binary(w, t->a[t->pos + 1], t->a[t->pos + 2]);
        t->pos += 3;
        return r ? 0 : 1;
    }
    if (is_unary_op(w) && t->pos + 1 < t->n) {
        bool r = test_unary(w, t->a[t->pos + 1]);
        t->pos += 2;
        return r ? 0 : 1;
    }
    t->pos++;
    return w[0] ? 0 : 1;
}

static int test_and(TestState *t) {
    int r = test_primary(t);
    while (t->pos < t->n && strcmp(t->a[t->pos], "-a") == 0) {
        t->pos++;
        int r2 = test_primary(t);
        r = (r == 0 && r2 == 0) ? 0 : 1;
    }
    return r;
}

static int test_or(TestState *t) {
    int r = test_and(t);
    while (t->pos < t->n && strcmp(t->a[t->pos], "-o") == 0) {
        t->pos++;
        int r2 = test_and(t);
        r = (r == 0 || r2 == 0) ? 0 : 1;
    }
    return r;
}

/* `test` / `[` with the full POSIX grammar (! ( ) -a -o) plus bash's extra
 * file operators. Also reused for the plain-operator atoms of `[[ ]]`. */
int test_eval(int argc, char **argv) {
    int n = argc - 1;
    char **a = argv + 1;
    if (strcmp(argv[0], "[") == 0) {
        if (n == 0 || strcmp(a[n - 1], "]") != 0) { sh_err("[: missing `]'\n"); return 2; }
        n--;
    }
    if (n == 0) return 1;
    /* POSIX special cases by argument count */
    if (n == 1) return a[0][0] ? 0 : 1;
    if (n == 2 && strcmp(a[0], "!") == 0) return a[1][0] ? 1 : 0;
    if (n == 2 && is_unary_op(a[0])) return test_unary(a[0], a[1]) ? 0 : 1;
    if (n == 3 && is_binary_op(a[1])) return test_binary(a[0], a[1], a[2]) ? 0 : 1;
    TestState t = { a, n, 0 };
    return test_or(&t);
}

/* =========================== command lookup =========================== */

static const char *g_keywords[] = { "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
    "case", "esac", "switch", "function", "begin", "end", "in", "time", "and", "or", "not", "[[", "]]", "!", "{", "}", NULL };

static bool is_keyword(const char *w) {
    for (int i = 0; g_keywords[i]; i++) if (strcmp(g_keywords[i], w) == 0) return true;
    return false;
}

/* all executable matches for `name` on $PATH (first one only unless all) */
static int path_lookup(const char *name, strvec_t *out, bool all) {
    if (strchr(name, '/')) { if (access(name, X_OK) == 0) { sv_push_dup(out, name); return 1; } return 0; }
    const char *path = var_get("PATH");
    if (!path) return 0;
    char *copy = xstrdup(path);
    int found = 0;
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", *dir ? dir : ".", name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0) {
            sv_push_dup(out, full);
            found++;
            if (!all) break;
        }
    }
    free(copy);
    return found;
}

static int b_type(int argc, char **argv) {
    bool type_only = false, all = false, path_only = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 't') type_only = true;
            else if (*p == 'a') all = true;
            else if (*p == 'p' || *p == 'P') path_only = true;
        }
    }
    int status = 0;
    bool c = out_color();
    for (; i < argc; i++) {
        const char *name = argv[i];
        bool found = false;
        if (!path_only && alias_get(name)) {
            found = true;
            if (type_only) printf("alias\n");
            else printf("%s%s%s is an alias for '%s'\n", c ? "\x1b[1m" : "", name, c ? "\x1b[0m" : "", alias_get(name));
            if (!all) continue;
        }
        if (!path_only && is_keyword(name)) {
            found = true;
            if (type_only) printf("keyword\n");
            else printf("%s%s%s is a shell keyword\n", c ? "\x1b[1m" : "", name, c ? "\x1b[0m" : "");
            if (!all) continue;
        }
        if (!path_only && func_get(name)) {
            found = true;
            Node *fn = func_get(name);
            if (type_only) printf("function\n");
            else printf("%s%s%s is a function%s%s\n", c ? "\x1b[1m" : "", name, c ? "\x1b[0m" : "",
                        fn->case_subject ? " - " : "", fn->case_subject ? fn->case_subject : "");
            if (!all) continue;
        }
        if (!path_only && builtin_exists(name)) {
            found = true;
            if (type_only) printf("builtin\n");
            else printf("%s%s%s is a shell builtin\n", c ? "\x1b[1m" : "", name, c ? "\x1b[0m" : "");
            if (!all) continue;
        }
        strvec_t paths; sv_init(&paths);
        if (path_lookup(name, &paths, all)) {
            found = true;
            for (size_t k = 0; k < paths.count; k++) {
                if (type_only) printf("file\n");
                else if (path_only) printf("%s\n", paths.items[k]);
                else printf("%s%s%s is %s%s%s\n", c ? "\x1b[1m" : "", name, c ? "\x1b[0m" : "", c ? "\x1b[32m" : "", paths.items[k], c ? "\x1b[0m" : "");
            }
        }
        sv_free(&paths);
        if (!found) { if (!type_only) sh_err("type: %s: not found\n", name); status = 1; }
    }
    return status;
}

static int b_which(int argc, char **argv) {
    bool all = false;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-a") == 0) { all = true; i++; }
    int status = 0;
    for (; i < argc; i++) {
        strvec_t paths; sv_init(&paths);
        if (path_lookup(argv[i], &paths, all)) for (size_t k = 0; k < paths.count; k++) printf("%s\n", paths.items[k]);
        else if (builtin_exists(argv[i])) printf("%s: shell builtin\n", argv[i]);
        else if (func_get(argv[i])) printf("%s: shell function\n", argv[i]);
        else { sh_err("which: no %s in PATH\n", argv[i]); status = 1; }
        sv_free(&paths);
    }
    return status;
}

static int b_command(int argc, char **argv) {
    int i = 1;
    bool v = false, V = false;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'v') v = true; else if (*p == 'V') V = true; }
    }
    if (i >= argc) return 0;
    if (v || V) {
        int status = 0;
        for (; i < argc; i++) {
            const char *name = argv[i];
            if (V) { char *tv[] = { (char *)"type", (char *)name, NULL }; status |= b_type(2, tv); continue; }
            if (alias_get(name)) printf("alias %s='%s'\n", name, alias_get(name));
            else if (func_get(name) || builtin_exists(name) || is_keyword(name)) printf("%s\n", name);
            else {
                strvec_t paths; sv_init(&paths);
                if (path_lookup(name, &paths, false)) printf("%s\n", paths.items[0]); else status = 1;
                sv_free(&paths);
            }
        }
        return status;
    }
    return exec_run_argv_nofunc(argc - i, argv + i);
}

static int b_builtin(int argc, char **argv) {
    if (argc < 2) return 0;
    BuiltinFn b = builtin_lookup(argv[1]);
    if (!b) { sh_err("builtin: %s: not a shell builtin\n", argv[1]); return 1; }
    return b(argc - 1, argv + 1);
}

static int b_eval(int argc, char **argv) {
    dstr_t d; ds_init(&d);
    for (int i = 1; i < argc; i++) { if (i > 1) ds_append_c(&d, ' '); ds_append(&d, argv[i]); }
    int st = exec_eval_string(d.data);
    ds_free(&d);
    return st;
}

static int b_exec(int argc, char **argv) {
    if (argc < 2) return 0;
    fflush(NULL);
    exec_run_exit_trap();
    execvp(argv[1], argv + 1);
    int err = errno;
    sh_err("exec: %s: %s\n", argv[1], strerror(err));
    if (!g_shell_is_interactive) { g_should_exit = true; g_exit_code = 127; }
    return err == ENOENT ? 127 : 126;
}

static int b_colon(int argc, char **argv) { (void)argc; (void)argv; return 0; }

static int b_source(int argc, char **argv) {
    if (argc < 2) { sh_err("source: filename required\n"); return 2; }
    const char *file = argv[1];
    char found[4096];
    if (!strchr(file, '/') && access(file, R_OK) != 0) {
        /* bash searches $PATH for a bare name */
        const char *path = var_get("PATH");
        if (path) {
            char *copy = xstrdup(path);
            for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
                snprintf(found, sizeof(found), "%s/%s", dir, file);
                if (access(found, R_OK) == 0) { file = found; break; }
            }
            free(copy);
        }
    }
    strvec_t saved; sv_init(&saved);
    bool has_args = argc > 2;
    if (has_args) {
        for (size_t i = 0; i < exec_positional_count(); i++) sv_push_dup(&saved, exec_positional_get(i));
        exec_set_positional(argc - 2, argv + 2);
    }
    int st = exec_source_file(file);
    if (has_args) exec_set_positional((int)saved.count, saved.items);
    sv_free(&saved);
    if (st < 0) { sh_err("source: %s: %s\n", argv[1], strerror(errno)); return 1; }
    return st;
}

static int b_shift(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    size_t cnt = exec_positional_count();
    if (n < 0 || (size_t)n > cnt) return 1;
    strvec_t rest; sv_init(&rest);
    for (size_t i = (size_t)n; i < cnt; i++) sv_push_dup(&rest, exec_positional_get(i));
    exec_set_positional((int)rest.count, rest.items);
    sv_free(&rest);
    return 0;
}

/* =========================== getopts =========================== */

static int b_getopts(int argc, char **argv) {
    static int charpos = 1;
    if (argc < 3) { sh_err("getopts: usage: getopts optstring name [arg ...]\n"); return 2; }
    const char *optstring = argv[1];
    const char *name = argv[2];
    bool silent = optstring[0] == ':';
    if (silent) optstring++;

    strvec_t args; sv_init(&args);
    if (argc > 3) for (int i = 3; i < argc; i++) sv_push_dup(&args, argv[i]);
    else for (size_t i = 0; i < exec_positional_count(); i++) sv_push_dup(&args, exec_positional_get(i));

    const char *oi = var_get("OPTIND");
    int optind = oi ? atoi(oi) : 1;
    if (optind < 1) optind = 1;
    if (optind == 1 && (!oi || strcmp(oi, "1") == 0)) { /* fresh scan */ }
    char buf[32];
    int result = 0;

    if ((size_t)optind > args.count) { result = 1; goto done; }
    const char *cur = args.items[optind - 1];
    if (charpos == 1) {
        if (cur[0] != '-' || cur[1] == '\0') { result = 1; goto done; }
        if (strcmp(cur, "--") == 0) { optind++; result = 1; goto done; }
    }
    char opt = cur[charpos];
    const char *spec = strchr(optstring, opt);
    charpos++;
    bool at_end = cur[charpos] == '\0';
    var_unset("OPTARG");
    if (!spec || opt == ':') {
        buf[0] = '?'; buf[1] = '\0';
        var_set(name, "?", false);
        if (silent) { char o[2] = { opt, 0 }; var_set("OPTARG", o, false); }
        else sh_err("getopts: illegal option -- %c\n", opt);
        if (at_end) { optind++; charpos = 1; }
        goto done_ok;
    }
    if (spec[1] == ':') {
        if (!at_end) { var_set("OPTARG", cur + charpos, false); optind++; charpos = 1; }
        else if ((size_t)optind < args.count) { var_set("OPTARG", args.items[optind], false); optind += 2; charpos = 1; }
        else {
            optind++; charpos = 1;
            if (silent) { var_set(name, ":", false); char o[2] = { opt, 0 }; var_set("OPTARG", o, false); }
            else { var_set(name, "?", false); sh_err("getopts: option requires an argument -- %c\n", opt); }
            goto done_ok;
        }
    } else if (at_end) { optind++; charpos = 1; }
    { char o[2] = { opt, 0 }; var_set(name, o, false); }
done_ok:
    snprintf(buf, sizeof(buf), "%d", optind);
    var_set("OPTIND", buf, false);
    sv_free(&args);
    return 0;
done:
    charpos = 1;
    snprintf(buf, sizeof(buf), "%d", optind);
    var_set("OPTIND", buf, false);
    var_set(name, "?", false);
    sv_free(&args);
    return result;
}

/* =========================== jobs & signals =========================== */

static int b_jobs(int argc, char **argv) {
    bool pids = false, longf = false;
    for (int i = 1; i < argc; i++) { if (strcmp(argv[i], "-p") == 0) pids = true; else if (strcmp(argv[i], "-l") == 0) longf = true; }
    jobs_reap(false);
    if (pids || longf) {
        for (Job *j = jobs_head(); j; j = j->next) {
            if (pids) printf("%d\n", (int)j->pgid);
            else printf("[%d] %d %s  %s\n", j->id, (int)j->pgid, j->state == JOB_RUNNING ? "Running" : j->state == JOB_STOPPED ? "Stopped" : "Done", j->cmdline);
        }
        return 0;
    }
    jobs_print();
    return 0;
}

static int b_fg(int argc, char **argv) {
    Job *j = job_from_spec(argc > 1 ? argv[1] : NULL);
    if (!j) { sh_err("fg: no such job\n"); return 1; }
    printf("%s\n", j->cmdline);
    fflush(stdout);
    if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, j->pgid);
    kill(-j->pgid, SIGCONT);
    j->state = JOB_RUNNING;
    int status;
    while (waitpid(-j->pgid, &status, WUNTRACED) < 0 && errno == EINTR) {}
    if (g_shell_is_interactive) tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    if (WIFSTOPPED(status)) { j->state = JOB_STOPPED; fprintf(stderr, "\n[%d]+  Stopped  %s\n", j->id, j->cmdline); return 128 + WSTOPSIG(status); }
    job_remove(j);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static int b_bg(int argc, char **argv) {
    Job *j = job_from_spec(argc > 1 ? argv[1] : NULL);
    if (!j) { sh_err("bg: no such job\n"); return 1; }
    kill(-j->pgid, SIGCONT);
    j->state = JOB_RUNNING;
    printf("[%d] %s &\n", j->id, j->cmdline);
    return 0;
}

static int b_wait(int argc, char **argv) {
    int status = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            pid_t pid;
            Job *j = NULL;
            if (argv[i][0] == '%') { j = job_from_spec(argv[i]); if (!j) { status = 127; continue; } pid = j->pgid; }
            else pid = (pid_t)atoi(argv[i]);
            int st;
            if (waitpid(pid, &st, 0) > 0) status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            else {
                /* already reaped by the job table */
                Job *jj = j ? j : job_find_by_pgid(pid);
                status = jj ? jj->last_status : 127;
            }
            if (!j) j = job_find_by_pgid(pid);
            if (j) job_remove(j);
        }
        return status;
    }
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, 0)) > 0 || (pid < 0 && errno == EINTR)) {
        if (pid < 0) continue;
        Job *j = job_find_by_pgid(pid);
        if (j) job_remove(j);
    }
    return 0;
}

static int b_kill(int argc, char **argv) {
    int sig = SIGTERM;
    int i = 1;
    if (i < argc && (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-L") == 0)) {
        if (i + 1 < argc) {
            int n = atoi(argv[i + 1]);
            if (n > 128) n -= 128;
            printf("%s\n", isdigit((unsigned char)argv[i + 1][0]) ? signal_name(n) : argv[i + 1]);
            return 0;
        }
        list_signals();
        return 0;
    }
    if (i < argc && (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-n") == 0) && i + 1 < argc) {
        sig = signal_from_name(argv[i + 1]); i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--") != 0) {
        sig = signal_from_name(argv[i] + 1); i++;
    }
    if (i < argc && strcmp(argv[i], "--") == 0) i++;
    if (sig < 0 || sig >= NSIG) { sh_err("kill: invalid signal specification\n"); return 1; }
    if (i >= argc) { sh_err("kill: usage: kill [-s sigspec | -sigspec] pid | %%job ...\n"); return 2; }
    int status = 0;
    for (; i < argc; i++) {
        pid_t target;
        if (argv[i][0] == '%') {
            Job *j = job_from_spec(argv[i]);
            if (!j) { sh_err("kill: %s: no such job\n", argv[i]); status = 1; continue; }
            target = -j->pgid;
            if (sig == SIGCONT || sig == SIGKILL || sig == SIGTERM) j->state = (sig == SIGCONT) ? JOB_RUNNING : j->state;
        } else target = (pid_t)atoi(argv[i]);
        if (kill(target, sig) != 0) { sh_err("kill: (%s) - %s\n", argv[i], strerror(errno)); status = 1; }
    }
    return status;
}

static int b_disown(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        while (jobs_head()) job_remove(jobs_head());
        return 0;
    }
    Job *j = job_from_spec(argc > 1 ? argv[1] : NULL);
    if (!j) { sh_err("disown: no such job\n"); return 1; }
    job_remove(j);
    return 0;
}

static int b_trap(int argc, char **argv) {
    int i = 1;
    if (i < argc && strcmp(argv[i], "-l") == 0) { list_signals(); return 0; }
    if (i < argc && strcmp(argv[i], "-p") == 0) i++;
    if (i >= argc) {
        for (int s = 0; s < TRAP_SLOTS; s++) {
            const char *t = exec_get_trap(s);
            if (t) printf("trap -- '%s' %s\n", t, signal_name(s));
        }
        return 0;
    }
    if (strcmp(argv[i], "--") == 0) i++;
    if (i >= argc) return 0;
    const char *action = argv[i];
    /* `trap SIG...` with a numeric first arg resets, like POSIX */
    bool reset = strcmp(action, "-") == 0;
    if (i + 1 >= argc) {
        int s = signal_from_name(action);
        if (s >= 0) { exec_set_trap(s, NULL); return 0; }
        sh_err("trap: usage: trap [-lp] [[action] signal ...]\n");
        return 2;
    }
    int status = 0;
    for (i++; i < argc; i++) {
        int s = signal_from_name(argv[i]);
        if (s < 0 || s >= TRAP_SLOTS || s == SIGKILL || s == SIGSTOP) { sh_err("trap: %s: invalid signal specification\n", argv[i]); status = 1; continue; }
        exec_set_trap(s, reset ? NULL : action);
    }
    return status;
}

static int b_umask(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "-S") == 0) {
        mode_t m = umask(0);
        umask(m);
        if (argc >= 2) {
            mode_t p = (mode_t)~m & 0777;
            printf("u=%s%s%s,g=%s%s%s,o=%s%s%s\n",
                   p & 0400 ? "r" : "", p & 0200 ? "w" : "", p & 0100 ? "x" : "",
                   p & 040 ? "r" : "", p & 020 ? "w" : "", p & 010 ? "x" : "",
                   p & 04 ? "r" : "", p & 02 ? "w" : "", p & 01 ? "x" : "");
        } else printf("%04o\n", (unsigned)m);
        return 0;
    }
    char *end;
    long v = strtol(argv[1], &end, 8);
    if (*end || v < 0 || v > 0777) { sh_err("umask: %s: octal number expected\n", argv[1]); return 1; }
    umask((mode_t)v);
    return 0;
}

/* =========================== directory stack =========================== */

static strvec_t g_dirstack;
static bool g_dirstack_init = false;

static void dirstack_ensure(void) { if (!g_dirstack_init) { sv_init(&g_dirstack); g_dirstack_init = true; } }

static void print_tilde_path(const char *p) {
    const char *home = var_get("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (home && hl && strncmp(p, home, hl) == 0 && (p[hl] == '/' || p[hl] == '\0')) printf("~%s", p + hl);
    else printf("%s", p);
}

static int b_dirs(int argc, char **argv) {
    dirstack_ensure();
    bool clear = false, vertical = false;
    for (int i = 1; i < argc; i++) { if (strcmp(argv[i], "-c") == 0) clear = true; else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "-p") == 0) vertical = true; }
    if (clear) { sv_clear(&g_dirstack); return 0; }
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    size_t n = g_dirstack.count;
    for (size_t k = 0; k <= n; k++) {
        const char *d = k == 0 ? cwd : g_dirstack.items[n - k];
        if (vertical) printf("%2zu  ", k);
        print_tilde_path(d);
        printf(vertical ? "\n" : (k == n ? "\n" : " "));
    }
    return 0;
}

static int run_cd(const char *dir, bool quiet) {
    char *cdargv[] = { (char *)"cd", (char *)dir, NULL };
    int saved = -1;
    if (quiet) { fflush(stdout); saved = dup(STDOUT_FILENO); int dn = open("/dev/null", 1); dup2(dn, 1); close(dn); }
    int st = builtin_lookup("cd")(2, cdargv);
    if (quiet) { fflush(stdout); dup2(saved, 1); close(saved); }
    return st;
}

static int b_pushd(int argc, char **argv) {
    dirstack_ensure();
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { sh_err("pushd: %s\n", strerror(errno)); return 1; }
    if (argc < 2) {
        if (g_dirstack.count == 0) { sh_err("pushd: no other directory\n"); return 1; }
        char *top = g_dirstack.items[g_dirstack.count - 1];
        g_dirstack.items[g_dirstack.count - 1] = xstrdup(cwd);
        int st = run_cd(top, true);
        free(top);
        if (st == 0) b_dirs(1, argv);
        return st;
    }
    if (run_cd(argv[1], true) != 0) return 1;
    sv_push_dup(&g_dirstack, cwd);
    b_dirs(1, argv);
    return 0;
}

static int b_popd(int argc, char **argv) {
    (void)argc;
    dirstack_ensure();
    if (g_dirstack.count == 0) { sh_err("popd: directory stack empty\n"); return 1; }
    char *top = g_dirstack.items[--g_dirstack.count];
    int st = run_cd(top, true);
    free(top);
    if (st == 0) b_dirs(1, argv);
    return st;
}

/* =========================== history / functions =========================== */

static int b_history(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "clear") == 0)) { history_clear(); return 0; }
    if (argc > 2 && strcmp(argv[1], "search") == 0) {
        bool c = out_color();
        for (size_t i = history_count(); i > 0; i--) {
            const char *e = history_get(i - 1);
            const char *m = strstr(e, argv[2]);
            if (!m) continue;
            if (c) printf("%.*s\x1b[1;33m%s\x1b[0m%s\n", (int)(m - e), e, argv[2], m + strlen(argv[2]));
            else printf("%s\n", e);
        }
        return 0;
    }
    if (argc > 1 && isdigit((unsigned char)argv[1][0])) {
        size_t n = (size_t)atol(argv[1]);
        size_t total = history_count();
        size_t start = n < total ? total - n : 0;
        bool c = out_color();
        for (size_t i = start; i < total; i++) {
            if (c) printf("\x1b[90m%5zu\x1b[0m  %s\n", i + 1, history_get(i));
            else printf("%5zu  %s\n", i + 1, history_get(i));
        }
        return 0;
    }
    history_print();
    return 0;
}

static int b_functions(int argc, char **argv) {
    if (argc > 2 && (strcmp(argv[1], "-e") == 0 || strcmp(argv[1], "--erase") == 0)) {
        for (int i = 2; i < argc; i++) func_undefine(argv[i]);
        return 0;
    }
    if (argc > 2 && (strcmp(argv[1], "-q") == 0 || strcmp(argv[1], "--query") == 0)) {
        for (int i = 2; i < argc; i++) if (!func_get(argv[i])) return 1;
        return 0;
    }
    strvec_t names; sv_init(&names);
    func_list_names(&names);
    qsort(names.items, names.count, sizeof(char *), cmp_strp);
    bool c = out_color();
    for (size_t i = 0; i < names.count; i++) {
        if (argc > 1 && strcmp(argv[1], names.items[i]) != 0) continue;
        Node *fn = func_get(names.items[i]);
        if (c) printf("\x1b[1;34m%s\x1b[0m", names.items[i]); else printf("%s", names.items[i]);
        if (fn && fn->for_words.count) {
            printf(" (");
            for (size_t k = 0; k < fn->for_words.count; k++) printf("%s%s", k ? " " : "", fn->for_words.items[k]);
            printf(")");
        }
        if (fn && fn->case_subject) printf(c ? "\x1b[90m  - %s\x1b[0m" : "  - %s", fn->case_subject);
        printf("\n");
    }
    sv_free(&names);
    return 0;
}

/* =========================== fish: count/contains/random/status =========================== */

static int b_count(int argc, char **argv) {
    (void)argv;
    int n = argc - 1;
    if (n == 0 && !isatty(STDIN_FILENO)) {
        /* `cmd | count` counts lines */
        char buf[4096]; ssize_t r; int lines = 0;
        while ((r = read(STDIN_FILENO, buf, sizeof(buf))) > 0) for (ssize_t k = 0; k < r; k++) if (buf[k] == '\n') lines++;
        n = lines;
    }
    printf("%d\n", n);
    return n > 0 ? 0 : 1;
}

static int b_contains(int argc, char **argv) {
    int i = 1;
    bool print_index = false;
    if (i < argc && (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--index") == 0)) { print_index = true; i++; }
    if (i < argc && strcmp(argv[i], "--") == 0) i++;
    if (i >= argc) return 1;
    const char *needle = argv[i++];
    for (int k = i; k < argc; k++) {
        if (strcmp(argv[k], needle) == 0) { if (print_index) printf("%d\n", k - i + 1); return 0; }
    }
    return 1;
}

static int b_random(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[1], "choice") == 0) { printf("%s\n", argv[2 + random() % (argc - 2)]); return 0; }
    long lo = 0, hi = 32767, step = 1;
    if (argc == 2) { srandom((unsigned)atol(argv[1])); return 0; }
    if (argc == 3) { lo = atol(argv[1]); hi = atol(argv[2]); }
    if (argc == 4) { lo = atol(argv[1]); step = atol(argv[2]); hi = atol(argv[3]); if (step <= 0) step = 1; }
    if (hi < lo) { long t = hi; hi = lo; lo = t; }
    long n = (hi - lo) / step + 1;
    printf("%ld\n", lo + (random() % n) * step);
    return 0;
}

static int b_status(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "is-interactive") == 0 || strcmp(sub, "-i") == 0) return g_shell_is_interactive ? 0 : 1;
    if (strcmp(sub, "is-login") == 0 || strcmp(sub, "-l") == 0) { const char *z = var_get("0"); return (z && z[0] == '-') ? 0 : 1; }
    if (strcmp(sub, "is-command-substitution") == 0) return g_exec_depth > 0 ? 0 : 1;
    if (strcmp(sub, "current-command") == 0) { printf("%s\n", var_get("0") ? var_get("0") : "spire"); return 0; }
    if (strcmp(sub, "filename") == 0 || strcmp(sub, "current-filename") == 0) { printf("%s\n", var_get("0") ? var_get("0") : "Standard input"); return 0; }
    if (strcmp(sub, "fish-path") == 0 || strcmp(sub, "spire-path") == 0) { char p[4096]; ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1); if (n > 0) { p[n] = 0; printf("%s\n", p); } return 0; }
    printf("This is %s shell.\n", g_shell_is_interactive ? "an interactive" : "a non-interactive");
    return 0;
}

/* =========================== fish: string =========================== */

static void string_inputs(int argc, char **argv, int i, strvec_t *out) {
    if (i < argc) { for (; i < argc; i++) sv_push_dup(out, argv[i]); return; }
    if (isatty(STDIN_FILENO)) return;
    dstr_t line; ds_init(&line);
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\n') { sv_push_dup(out, line.data); ds_clear(&line); }
        else ds_append_c(&line, c);
    }
    if (line.len) sv_push_dup(out, line.data);
    ds_free(&line);
}

static bool has_flag(int *i, int argc, char **argv, const char *s, const char *l) {
    if (*i < argc && (strcmp(argv[*i], s) == 0 || (l && strcmp(argv[*i], l) == 0))) { (*i)++; return true; }
    return false;
}

static int b_string(int argc, char **argv) {
    if (argc < 2) { sh_err("string: missing subcommand\n"); return 2; }
    const char *sub = argv[1];
    int i = 2;
    strvec_t in; sv_init(&in);
    int status = 1;

    if (strcmp(sub, "length") == 0) {
        bool quiet = has_flag(&i, argc, argv, "-q", "--quiet");
        string_inputs(argc, argv, i, &in);
        for (size_t k = 0; k < in.count; k++) {
            size_t n = 0;
            for (const char *p = in.items[k]; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) n++;
            if (!quiet) printf("%zu\n", n);
            if (n) status = 0;
        }
    } else if (strcmp(sub, "upper") == 0 || strcmp(sub, "lower") == 0) {
        bool up = sub[0] == 'u';
        string_inputs(argc, argv, i, &in);
        for (size_t k = 0; k < in.count; k++) {
            bool changed = false;
            for (char *p = in.items[k]; *p; p++) { char c = (char)(up ? toupper((unsigned char)*p) : tolower((unsigned char)*p)); if (c != *p) changed = true; *p = c; }
            printf("%s\n", in.items[k]);
            if (changed) status = 0;
        }
    } else if (strcmp(sub, "trim") == 0) {
        bool left = false, right = false;
        const char *chars = " \t\n\r";
        for (;;) {
            if (has_flag(&i, argc, argv, "-l", "--left")) left = true;
            else if (has_flag(&i, argc, argv, "-r", "--right")) right = true;
            else if (i + 1 < argc && (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--chars") == 0)) { chars = argv[i + 1]; i += 2; }
            else break;
        }
        if (!left && !right) left = right = true;
        string_inputs(argc, argv, i, &in);
        for (size_t k = 0; k < in.count; k++) {
            char *s = in.items[k], *e = s + strlen(s);
            char *b = s;
            if (left) while (*b && strchr(chars, *b)) b++;
            if (right) while (e > b && strchr(chars, e[-1])) e--;
            if (b != s || *e) status = 0;
            printf("%.*s\n", (int)(e - b), b);
        }
    } else if (strcmp(sub, "join") == 0 || strcmp(sub, "join0") == 0) {
        const char *sep = sub[4] == '0' ? "" : (i < argc ? argv[i++] : "");
        string_inputs(argc, argv, i, &in);
        for (size_t k = 0; k < in.count; k++) { if (k) fputs(sep, stdout); fputs(in.items[k], stdout); }
        if (sub[4] == '0') fputc('\0', stdout); else fputc('\n', stdout);
        status = in.count > 1 ? 0 : 1;
    } else if (strcmp(sub, "split") == 0) {
        long max = -1;
        bool right = false, no_empty = false;
        for (;;) {
            if (i + 1 < argc && (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--max") == 0)) { max = atol(argv[i + 1]); i += 2; }
            else if (has_flag(&i, argc, argv, "-r", "--right")) right = true;
            else if (has_flag(&i, argc, argv, "-n", "--no-empty")) no_empty = true;
            else break;
        }
        const char *sep = i < argc ? argv[i++] : " ";
        size_t sl = strlen(sep);
        string_inputs(argc, argv, i, &in);
        for (size_t k = 0; k < in.count; k++) {
            const char *s = in.items[k];
            strvec_t parts; sv_init(&parts);
            if (sl == 0) { for (const char *p = s; *p; p++) { char t[2] = { *p, 0 }; sv_push_dup(&parts, t); } }
            else if (!right) {
                long splits = 0;
                const char *p = s;
                for (;;) {
                    const char *m = (max < 0 || splits < max) ? strstr(p, sep) : NULL;
                    if (!m) { sv_push_dup(&parts, p); break; }
                    sv_push(&parts, xstrndup(p, (size_t)(m - p)));
                    p = m + sl; splits++;
                }
            } else {
                /* split from the right: collect then reverse */
                strvec_t rev; sv_init(&rev);
                size_t end = strlen(s);
                long splits = 0;
                for (;;) {
                    const char *m = NULL;
                    if (max < 0 || splits < max) for (size_t q = end >= sl ? end - sl + 1 : 0; q-- > 0;) if (strncmp(s + q, sep, sl) == 0) { m = s + q; break; }
                    if (!m) { sv_push(&rev, xstrndup(s, end)); break; }
                    sv_push(&rev, xstrndup(m + sl, end - (size_t)(m - s) - sl));
                    end = (size_t)(m - s); splits++;
                }
                for (size_t q = rev.count; q > 0; q--) sv_push_dup(&parts, rev.items[q - 1]);
                sv_free(&rev);
            }
            if (parts.count > 1) status = 0;
            for (size_t q = 0; q < parts.count; q++) if (!no_empty || parts.items[q][0]) printf("%s\n", parts.items[q]);
            sv_free(&parts);
        }
    } else if (strcmp(sub, "sub") == 0) {
        long start = 1, len = -1, end = 0;
        bool has_end = false;
        for (;;) {
            if (i + 1 < argc && (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--start") == 0)) { start = atol(argv[i + 1]); i += 2; }
            else if (i + 1 < argc && (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--length") == 0)) { len = atol(argv[i + 1]); i += 2; }
            else if (i + 1 < argc && (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--end") == 0)) { end = atol(argv[i + 1]); has_end = true; i += 2; }
            else break;
        }
        string_inputs(argc, argv, i, &in);
        for (size_t k = 0; k < in.count; k++) {
            long n = (long)strlen(in.items[k]);
            long b = start > 0 ? start - 1 : n + start;
            if (b < 0) b = 0;
            if (b > n) b = n;
            long e = n;
            if (has_end) e = end >= 0 ? end : n + end + 1;
            if (len >= 0 && b + len < e) e = b + len;
            if (e > n) e = n;
            if (e < b) e = b;
            printf("%.*s\n", (int)(e - b), in.items[k] + b);
            status = 0;
        }
    } else if (strcmp(sub, "repeat") == 0) {
        long n = 1, max = -1;
        bool no_nl = false;
        for (;;) {
            if (i + 1 < argc && (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--count") == 0)) { n = atol(argv[i + 1]); i += 2; }
            else if (i + 1 < argc && (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--max") == 0)) { max = atol(argv[i + 1]); i += 2; }
            else if (has_flag(&i, argc, argv, "-N", "--no-newline")) no_nl = true;
            else break;
        }
        string_inputs(argc, argv, i, &in);
        for (size_t k = 0; k < in.count; k++) {
            dstr_t d; ds_init(&d);
            for (long r = 0; r < n || (max > 0 && n == 1 && (long)d.len < max); r++) {
                ds_append(&d, in.items[k]);
                if (max > 0 && (long)d.len >= max) break;
                if (!*in.items[k]) break;
            }
            if (max > 0 && (long)d.len > max) d.data[max] = '\0';
            printf("%s%s", d.data, no_nl ? "" : "\n");
            if (d.data[0]) status = 0;
            ds_free(&d);
        }
    } else if (strcmp(sub, "escape") == 0) {
        string_inputs(argc, argv, i, &in);
        for (size_t k = 0; k < in.count; k++) {
            const char *v = in.items[k];
            bool plain = *v != '\0';
            for (const char *q = v; *q; q++) if (!isalnum((unsigned char)*q) && !strchr("_-./=:,+@%", *q)) { plain = false; break; }
            if (plain) printf("%s\n", v);
            else {
                putchar('\'');
                for (const char *q = v; *q; q++) { if (*q == '\'') fputs("\\'", stdout); else if (*q == '\\') fputs("\\\\", stdout); else putchar(*q); }
                puts("'");
            }
            status = 0;
        }
    } else if (strcmp(sub, "replace") == 0 || strcmp(sub, "match") == 0) {
        bool all = false, regex = false, quiet = false, icase = false, invert = false, filter = false, entire = false;
        for (;;) {
            if (has_flag(&i, argc, argv, "-a", "--all")) all = true;
            else if (has_flag(&i, argc, argv, "-r", "--regex")) regex = true;
            else if (has_flag(&i, argc, argv, "-q", "--quiet")) quiet = true;
            else if (has_flag(&i, argc, argv, "-i", "--ignore-case")) icase = true;
            else if (has_flag(&i, argc, argv, "-v", "--invert")) invert = true;
            else if (has_flag(&i, argc, argv, "-f", "--filter")) filter = true;
            else if (has_flag(&i, argc, argv, "-e", "--entire")) entire = true;
            else break;
        }
        bool is_replace = sub[0] == 'r';
        if (i >= argc) { sh_err("string %s: missing pattern\n", sub); return 2; }
        const char *pat = argv[i++];
        const char *rep = "";
        if (is_replace) { if (i >= argc) { sh_err("string replace: missing replacement\n"); return 2; } rep = argv[i++]; }
        string_inputs(argc, argv, i, &in);
        regex_t re;
        if (regex && regcomp(&re, pat, REG_EXTENDED | (icase ? REG_ICASE : 0)) != 0) { sh_err("string: invalid regex '%s'\n", pat); sv_free(&in); return 2; }
        for (size_t k = 0; k < in.count; k++) {
            const char *s = in.items[k];
            if (!is_replace) {
                bool matched;
                regmatch_t m[10];
                if (regex) matched = regexec(&re, s, 10, m, 0) == 0;
                else matched = fnmatch(pat, s, icase ? FNM_CASEFOLD : 0) == 0;
                if (matched != invert) {
                    status = 0;
                    if (quiet) continue;
                    if (regex && matched && !entire) {
                        printf("%.*s\n", (int)(m[0].rm_eo - m[0].rm_so), s + m[0].rm_so);
                        for (int g = 1; g < 10 && m[g].rm_so >= 0; g++) printf("%.*s\n", (int)(m[g].rm_eo - m[g].rm_so), s + m[g].rm_so);
                    } else printf("%s\n", s);
                }
                continue;
            }
            dstr_t d; ds_init(&d);
            bool changed = false;
            if (regex) {
                const char *p = s;
                regmatch_t m[10];
                while (*p || p == s) {
                    if (regexec(&re, p, 10, m, p == s ? 0 : REG_NOTBOL) != 0) break;
                    changed = true;
                    ds_append_n(&d, p, (size_t)m[0].rm_so);
                    for (const char *r = rep; *r; r++) {
                        if ((*r == '$' || *r == '\\') && isdigit((unsigned char)r[1])) {
                            int g = r[1] - '0';
                            if (m[g].rm_so >= 0) ds_append_n(&d, p + m[g].rm_so, (size_t)(m[g].rm_eo - m[g].rm_so));
                            r++;
                        } else ds_append_c(&d, *r);
                    }
                    if (m[0].rm_eo == m[0].rm_so) { if (p[m[0].rm_eo]) ds_append_c(&d, p[m[0].rm_eo]); p += m[0].rm_eo + 1; if (p > s + strlen(s)) break; }
                    else p += m[0].rm_eo;
                    if (!all) break;
                }
                if (p <= s + strlen(s)) ds_append(&d, p);
            } else {
                size_t pl = strlen(pat);
                const char *p = s;
                while (*p) {
                    bool hit = pl && (icase ? strncasecmp(p, pat, pl) == 0 : strncmp(p, pat, pl) == 0);
                    if (hit && (all || !changed)) { ds_append(&d, rep); p += pl; changed = true; }
                    else ds_append_c(&d, *p++);
                }
            }
            if (changed) status = 0;
            if (!quiet && (changed || !filter)) printf("%s\n", d.data);
            ds_free(&d);
        }
        if (regex) regfree(&re);
    } else if (strcmp(sub, "pad") == 0) {
        long width = 0; bool right = false; char ch = ' ';
        for (;;) {
            if (i + 1 < argc && (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--width") == 0)) { width = atol(argv[i + 1]); i += 2; }
            else if (i + 1 < argc && (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--char") == 0)) { ch = argv[i + 1][0]; i += 2; }
            else if (has_flag(&i, argc, argv, "-r", "--right")) right = true;
            else break;
        }
        string_inputs(argc, argv, i, &in);
        for (size_t k = 0; k < in.count; k++) { long l = (long)strlen(in.items[k]); if (l > width) width = l; }
        for (size_t k = 0; k < in.count; k++) {
            long pad = width - (long)strlen(in.items[k]);
            if (right) fputs(in.items[k], stdout);
            for (long q = 0; q < pad; q++) putchar(ch);
            if (!right) fputs(in.items[k], stdout);
            putchar('\n');
        }
        status = 0;
    } else {
        sh_err("string: unknown subcommand '%s' (length lower upper trim join split sub repeat escape replace match pad)\n", sub);
        status = 2;
    }
    sv_free(&in);
    fflush(stdout);
    return status;
}

/* =========================== fish: math =========================== */

typedef struct { const char *s; size_t pos; bool err; } MathState;

static double m_expr(MathState *m);

static void m_ws(MathState *m) { while (isspace((unsigned char)m->s[m->pos])) m->pos++; }

static double m_primary(MathState *m) {
    m_ws(m);
    const char *s = m->s;
    if (s[m->pos] == '(') {
        m->pos++;
        double v = m_expr(m);
        m_ws(m);
        if (s[m->pos] == ')') m->pos++; else m->err = true;
        return v;
    }
    if (isdigit((unsigned char)s[m->pos]) || s[m->pos] == '.') {
        char *end;
        double v;
        if (s[m->pos] == '0' && (s[m->pos + 1] == 'x' || s[m->pos + 1] == 'X')) v = (double)strtol(s + m->pos, &end, 16);
        else v = strtod(s + m->pos, &end);
        m->pos = (size_t)(end - s);
        return v;
    }
    if (isalpha((unsigned char)s[m->pos]) || s[m->pos] == '_') {
        size_t st = m->pos;
        while (isalnum((unsigned char)s[m->pos]) || s[m->pos] == '_') m->pos++;
        char name[32];
        size_t nl = m->pos - st < 31 ? m->pos - st : 31;
        memcpy(name, s + st, nl); name[nl] = '\0';
        if (strcmp(name, "pi") == 0) return M_PI;
        if (strcmp(name, "e") == 0) return M_E;
        if (strcmp(name, "tau") == 0) return 2 * M_PI;
        m_ws(m);
        if (s[m->pos] != '(') {
            /* bare variable name */
            const char *v = var_get(name);
            if (v) return strtod(v, NULL);
            m->err = true;
            return 0;
        }
        m->pos++;
        double args[4] = {0};
        int na = 0;
        m_ws(m);
        if (s[m->pos] != ')') {
            for (;;) {
                if (na < 4) args[na++] = m_expr(m); else m_expr(m);
                m_ws(m);
                if (s[m->pos] == ',') { m->pos++; continue; }
                break;
            }
        }
        if (s[m->pos] == ')') m->pos++; else m->err = true;
        double a = args[0], b = args[1];
        if (strcmp(name, "abs") == 0) return fabs(a);
        if (strcmp(name, "sqrt") == 0) return sqrt(a);
        if (strcmp(name, "cbrt") == 0) return cbrt(a);
        if (strcmp(name, "ceil") == 0) return ceil(a);
        if (strcmp(name, "floor") == 0) return floor(a);
        if (strcmp(name, "round") == 0) return round(a);
        if (strcmp(name, "exp") == 0) return exp(a);
        if (strcmp(name, "ln") == 0) return log(a);
        if (strcmp(name, "log") == 0 || strcmp(name, "log10") == 0) return log10(a);
        if (strcmp(name, "log2") == 0) return log2(a);
        if (strcmp(name, "sin") == 0) return sin(a);
        if (strcmp(name, "cos") == 0) return cos(a);
        if (strcmp(name, "tan") == 0) return tan(a);
        if (strcmp(name, "asin") == 0) return asin(a);
        if (strcmp(name, "acos") == 0) return acos(a);
        if (strcmp(name, "atan") == 0) return atan(a);
        if (strcmp(name, "atan2") == 0) return atan2(a, b);
        if (strcmp(name, "sinh") == 0) return sinh(a);
        if (strcmp(name, "cosh") == 0) return cosh(a);
        if (strcmp(name, "tanh") == 0) return tanh(a);
        if (strcmp(name, "pow") == 0) return pow(a, b);
        if (strcmp(name, "min") == 0) return na > 1 ? fmin(a, b) : a;
        if (strcmp(name, "max") == 0) return na > 1 ? fmax(a, b) : a;
        if (strcmp(name, "fac") == 0) { double r = 1; for (long k = 2; k <= (long)a; k++) r *= (double)k; return r; }
        m->err = true;
        return 0;
    }
    m->err = true;
    return 0;
}

static double m_unary(MathState *m) {
    m_ws(m);
    if (m->s[m->pos] == '-') { m->pos++; return -m_unary(m); }
    if (m->s[m->pos] == '+') { m->pos++; return m_unary(m); }
    return m_primary(m);
}

static double m_pow(MathState *m) {
    double v = m_unary(m);
    m_ws(m);
    if (m->s[m->pos] == '^' || (m->s[m->pos] == '*' && m->s[m->pos + 1] == '*')) {
        m->pos += m->s[m->pos] == '^' ? 1 : 2;
        double e = m_pow(m); /* right associative */
        return pow(v, e);
    }
    return v;
}

static double m_mul(MathState *m) {
    double v = m_pow(m);
    for (;;) {
        m_ws(m);
        char c = m->s[m->pos];
        if ((c == '*' && m->s[m->pos + 1] != '*') || c == 'x') { m->pos++; v *= m_pow(m); }
        else if (c == '/') { m->pos++; double r = m_pow(m); if (r == 0) { m->err = true; return 0; } v /= r; }
        else if (c == '%') { m->pos++; double r = m_pow(m); if (r == 0) { m->err = true; return 0; } v = fmod(v, r); }
        else break;
    }
    return v;
}

static double m_expr(MathState *m) {
    double v = m_mul(m);
    for (;;) {
        m_ws(m);
        char c = m->s[m->pos];
        if (c == '+') { m->pos++; v += m_mul(m); }
        else if (c == '-') { m->pos++; v -= m_mul(m); }
        else break;
    }
    return v;
}

static int b_math(int argc, char **argv) {
    int scale = 6;
    int i = 1;
    for (; i < argc; i++) {
        if (strncmp(argv[i], "-s", 2) == 0 && argv[i][2]) { scale = atoi(argv[i] + 2); continue; }
        if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--scale") == 0) && i + 1 < argc) { scale = atoi(argv[++i]); continue; }
        if (strncmp(argv[i], "--scale=", 8) == 0) { scale = atoi(argv[i] + 8); continue; }
        break;
    }
    dstr_t d; ds_init(&d);
    for (; i < argc; i++) { if (d.len) ds_append_c(&d, ' '); ds_append(&d, argv[i]); }
    if (!d.len) { sh_err("math: expected an expression\n"); ds_free(&d); return 2; }
    MathState m = { d.data, 0, false };
    double v = m_expr(&m);
    m_ws(&m);
    if (m.err || d.data[m.pos]) {
        sh_err("math: invalid expression: %s\n", d.data);
        ds_free(&d);
        return 3;
    }
    ds_free(&d);
    if (isnan(v) || isinf(v)) { printf("%s\n", isnan(v) ? "nan" : (v > 0 ? "inf" : "-inf")); return 0; }
    if (scale == 0) { printf("%.0f\n", trunc(v)); return 0; }
    char buf[128];
    snprintf(buf, sizeof(buf), "%.*f", scale, v);
    if (strchr(buf, '.')) {
        size_t n = strlen(buf);
        while (n > 0 && buf[n - 1] == '0') buf[--n] = '\0';
        if (n > 0 && buf[n - 1] == '.') buf[--n] = '\0';
    }
    if (strcmp(buf, "-0") == 0) strcpy(buf, "0");
    printf("%s\n", buf);
    return 0;
}

/* =========================== fish: abbr =========================== */

static strvec_t g_abbr_names, g_abbr_vals;
static bool g_abbr_init = false;

static void abbr_ensure(void) { if (!g_abbr_init) { sv_init(&g_abbr_names); sv_init(&g_abbr_vals); g_abbr_init = true; } }

const char *abbr_get(const char *name) {
    abbr_ensure();
    for (size_t i = 0; i < g_abbr_names.count; i++) if (strcmp(g_abbr_names.items[i], name) == 0) return g_abbr_vals.items[i];
    return NULL;
}

void abbr_list(strvec_t *names) {
    abbr_ensure();
    for (size_t i = 0; i < g_abbr_names.count; i++) sv_push_dup(names, g_abbr_names.items[i]);
}

static void abbr_erase(const char *name) {
    for (size_t i = 0; i < g_abbr_names.count; i++) {
        if (strcmp(g_abbr_names.items[i], name) != 0) continue;
        free(g_abbr_names.items[i]); free(g_abbr_vals.items[i]);
        for (size_t k = i + 1; k < g_abbr_names.count; k++) { g_abbr_names.items[k-1] = g_abbr_names.items[k]; g_abbr_vals.items[k-1] = g_abbr_vals.items[k]; }
        g_abbr_names.count--; g_abbr_vals.count--;
        return;
    }
}

static int b_abbr(int argc, char **argv) {
    abbr_ensure();
    int i = 1;
    bool add = false, erase = false, list = false, query = false;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-a") == 0 || strcmp(a, "--add") == 0) add = true;
        else if (strcmp(a, "-e") == 0 || strcmp(a, "--erase") == 0) erase = true;
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0) list = true;
        else if (strcmp(a, "-q") == 0 || strcmp(a, "--query") == 0) query = true;
        else if (strcmp(a, "-s") == 0 || strcmp(a, "--show") == 0) { i = argc; break; }
        else if (strcmp(a, "-g") == 0 || strcmp(a, "-U") == 0 || strcmp(a, "--global") == 0) {}
        else break;
    }
    if (erase) { for (; i < argc; i++) abbr_erase(argv[i]); return 0; }
    if (query) { for (; i < argc; i++) if (!abbr_get(argv[i])) return 1; return 0; }
    if (list) { for (size_t k = 0; k < g_abbr_names.count; k++) printf("%s\n", g_abbr_names.items[k]); return 0; }
    if (i >= argc) {
        bool c = out_color();
        for (size_t k = 0; k < g_abbr_names.count; k++) {
            if (c) printf("abbr -a -- \x1b[1;34m%s\x1b[0m '%s'\n", g_abbr_names.items[k], g_abbr_vals.items[k]);
            else printf("abbr -a -- %s '%s'\n", g_abbr_names.items[k], g_abbr_vals.items[k]);
        }
        return 0;
    }
    (void)add;
    if (strcmp(argv[i], "--") == 0) i++;
    if (i >= argc) return 1;
    const char *name = argv[i++];
    const char *eq = strchr(name, '=');
    char *nm; dstr_t val; ds_init(&val);
    if (eq && i >= argc) { nm = xstrndup(name, (size_t)(eq - name)); ds_append(&val, eq + 1); }
    else {
        nm = xstrdup(name);
        for (; i < argc; i++) { if (val.len) ds_append_c(&val, ' '); ds_append(&val, argv[i]); }
    }
    if (!val.len) { sh_err("abbr: %s: expansion required\n", nm); free(nm); ds_free(&val); return 2; }
    abbr_erase(nm);
    sv_push(&g_abbr_names, nm);
    sv_push(&g_abbr_vals, xstrdup(val.data));
    ds_free(&val);
    return 0;
}

/* =========================== help =========================== */

static int b_help(int argc, char **argv) {
    (void)argc; (void)argv;
    bool c = out_color();
    const char *H = c ? "\x1b[1;36m" : "", *K = c ? "\x1b[34m" : "", *R = c ? "\x1b[0m" : "", *D = c ? "\x1b[90m" : "";
    printf("%sspire%s - a shell that blends fish, zsh and bash\n\n", H, R);
    printf("%scontrol flow%s %s(fish or bash syntax, mix freely)%s\n", H, R, D, R);
    static const char *rows[][2] = {
        {"if / else if / else / end", "if / then / elif / else / fi"},
        {"while|until COND; ...; end", "while|until COND; do ...; done"},
        {"for x in ...; ...; end", "for x in ...; do ...; done   (for x; = \"$@\")"},
        {"switch x; case a b; ...; end", "case x in a|b) ... ;; esac"},
        {"function f -a x y; ...; end", "f() { ...; }"},
        {"begin; ...; end", "{ ...; }   ( subshell )"},
        {"cmd; and cmd; or cmd; not cmd", "cmd && cmd || cmd; ! cmd"},
        {"time PIPELINE", "[[ ( a == b* ) && x =~ re ]]   (( i++ ))"},
        {NULL, NULL}
    };
    for (int r = 0; rows[r][0]; r++) printf("  %s%-32s%s %s\n", K, rows[r][0], R, rows[r][1]);
    printf("\n");
    printf("%sexpansion%s\n", H, R);
    printf("  ${v:-d} ${v:=d} ${v:+a} ${v:?e} ${#v} ${v#p} ${v##p} ${v%%p} ${v%%%%p}\n");
    printf("  ${v/p/r} ${v//p/r} ${v:o:l} ${v^^} ${v,,} ${!ref} ${arr[@]:1:2} ${v@Q}\n");
    printf("  {a,b} {1..10..2}  $((math))  $(cmd) `cmd` (cmd)  <(cmd) >(cmd)  $'\\t'\n");
    printf("  $list $list[1] $list[-1] $list[2..3]  **/*.c  ~  $RANDOM $SECONDS $status\n\n");
    printf("%sredirection%s  > >> < <> 2> 2>&1 >&2 &> &>> <<EOF <<-EOF <<< |& N>&-\n\n", H, R);
    printf("%sbuiltins%s\n", H, R);
    strvec_t names; sv_init(&names);
    builtins_list_names(&names);
    qsort(names.items, names.count, sizeof(char *), cmp_strp);
    size_t col = 2;
    printf("  ");
    for (size_t i = 0; i < names.count; i++) {
        size_t l = strlen(names.items[i]) + 1;
        if (col + l > 78) { printf("\n  "); col = 2; }
        printf("%s%s%s ", K, names.items[i], R);
        col += l;
    }
    printf("\n\n");
    sv_free(&names);
    printf("%skeys%s  Tab complete (again: cycle, Shift-Tab back)  Ctrl-R search  → accept suggestion\n", H, R);
    printf("      Alt-→/Alt-F accept a word  Alt-. last argument  Ctrl-X Ctrl-E edit in $EDITOR\n\n");
    printf("Configure colors and the prompt in ~/.config/spire/spire.conf, add startup\n");
    printf("commands to ~/.config/spire/config.spire, and try `colorscheme`.\n");
    return 0;
}

/* =========================== table =========================== */

static BuiltinEntry g_extra[] = {
    {"set", b_set}, {"declare", b_declare}, {"typeset", b_declare}, {"readonly", b_declare},
    {"local", b_local}, {"export", b_export},
    {"echo", b_echo}, {"printf", b_printf}, {"read", b_read},
    {"test", test_eval}, {"[", test_eval},
    {"type", b_type}, {"which", b_which}, {"command", b_command}, {"builtin", b_builtin},
    {"eval", b_eval}, {"exec", b_exec}, {":", b_colon}, {"source", b_source}, {".", b_source},
    {"shift", b_shift}, {"getopts", b_getopts},
    {"jobs", b_jobs}, {"fg", b_fg}, {"bg", b_bg}, {"wait", b_wait}, {"kill", b_kill},
    {"disown", b_disown}, {"trap", b_trap}, {"umask", b_umask},
    {"dirs", b_dirs}, {"pushd", b_pushd}, {"popd", b_popd},
    {"history", b_history}, {"functions", b_functions},
    {"count", b_count}, {"contains", b_contains}, {"random", b_random}, {"status", b_status},
    {"string", b_string}, {"math", b_math}, {"abbr", b_abbr},
    {"help", b_help},
    {NULL, NULL}
};

BuiltinEntry *builtins_extra_table(void) { return g_extra; }
