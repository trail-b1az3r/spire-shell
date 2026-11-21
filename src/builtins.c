#include "builtins.h"
#include "exec.h"
#include "vars.h"
#include "arrays.h"
#include "arith.h"
#include "aliases.h"
#include "funcs.h"
#include "jobs.h"
#include "config.h"
#include "history.h"
#include "colors.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>

static int b_cd(int argc, char **argv) {
    const char *target;
    char resolved[4096];
    if (argc < 2) {
        target = var_get("HOME");
        if (!target) { sh_err("cd: HOME not set\n"); return 1; }
    } else if (strcmp(argv[1], "-") == 0) {
        target = var_get("OLDPWD");
        if (!target) { sh_err("cd: OLDPWD not set\n"); return 1; }
        printf("%s\n", target);
    } else {
        target = argv[1];
        /* CDPATH lookup for relative names */
        const char *cdpath = var_get("CDPATH");
        static char found[4096];
        if (cdpath && target[0] != '/' && target[0] != '.' && access(target, F_OK) != 0) {
            char *copy = xstrdup(cdpath);
            for (char *save = NULL, *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
                snprintf(found, sizeof(found), "%s/%s", d, target);
                struct stat st;
                if (stat(found, &st) == 0 && S_ISDIR(st.st_mode)) { target = found; printf("%s\n", found); break; }
            }
            free(copy);
        }
    }
    char *oldpwd = getcwd(resolved, sizeof(resolved));
    if (chdir(target) != 0) { sh_err("cd: %s: %s\n", target, strerror(errno)); return 1; }
    if (oldpwd) var_set("OLDPWD", oldpwd, true);
    char newpwd[4096];
    if (getcwd(newpwd, sizeof(newpwd))) var_set("PWD", newpwd, true);
    return 0;
}

static int b_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) { printf("%s\n", buf); return 0; }
    sh_err("pwd: %s\n", strerror(errno));
    return 1;
}

static int b_exit(int argc, char **argv) {
    g_should_exit = true;
    g_exit_code = argc > 1 ? atoi(argv[1]) : g_exit_code;
    return g_exit_code;
}


static int b_unset(int argc, char **argv) {
    bool funcs = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-f") == 0) funcs = true;
        else if (strcmp(argv[i], "-v") == 0) funcs = false;
        else break;
    }
    for (; i < argc; i++) {
        if (funcs) { func_undefine(argv[i]); continue; }
        char *br = strchr(argv[i], '[');
        if (br && array_exists(argv[i]) == false) {
            /* unset arr[N]: blank that element (no sparse arrays) */
            char *name = xstrndup(argv[i], (size_t)(br - argv[i]));
            if (array_exists(name)) array_set_index(name, arith_eval(br + 1), "");
            free(name);
            continue;
        }
        var_unset(argv[i]);
        array_unset(argv[i]);
    }
    return 0;
}

static int b_alias(int argc, char **argv) {
    if (argc == 1) {
        strvec_t names, values; sv_init(&names); sv_init(&values);
        alias_list(&names, &values);
        for (size_t i = 0; i < names.count; i++) printf("alias %s='%s'\n", names.items[i], values.items[i]);
        sv_free(&names); sv_free(&values);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            const char *v = alias_get(argv[i]);
            if (v) printf("alias %s='%s'\n", argv[i], v);
            else sh_err("alias: %s: not found\n", argv[i]);
            continue;
        }
        *eq = '\0';
        char *val = eq + 1;
        size_t vl = strlen(val);
        if (vl >= 2 && ((val[0] == '\'' && val[vl-1] == '\'') || (val[0] == '"' && val[vl-1] == '"'))) {
            val[vl-1] = '\0'; val++;
        }
        alias_set(argv[i], val);
    }
    return 0;
}

static int b_unalias(int argc, char **argv) {
    for (int i = 1; i < argc; i++) alias_unset(argv[i]);
    return 0;
}











static int b_true(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int b_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }

static int b_let(int argc, char **argv) {
    if (argc < 2) return 1;
    long last = 0;
    for (int i = 1; i < argc; i++) last = arith_eval(argv[i]);
    return last != 0 ? 0 : 1;
}

/* fish's set_color: prints the raw escape code for a name (or "normal" to
 * reset), with no trailing newline, so it can be embedded via command
 * substitution in prompts/echo: `echo (set_color red)hi(set_color normal)`.
 * -b NAME sets the background instead. */
static int b_set_color(int argc, char **argv) {
    if (argc < 2) return 0;
    bool bg = false;
    int i = 1;
    if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) { bg = true; i++; }
    const char *name = argv[i];
    if (strcmp(name, "normal") == 0 || strcmp(name, "reset") == 0) { fputs(ANSI_RESET, stdout); return 0; }
    char *code = ansi_code_for(name);
    if (bg && code[0]) {
        /* crude fg->bg shift: 3x/9x -> 4x/10x */
        dstr_t d; ds_init(&d);
        for (char *p = code; *p; p++) {
            if (*p == '3' && p[1] != ';' ) { ds_append_c(&d, '4'); continue; }
            if (*p == '9' && isdigit((unsigned char)p[1])) { ds_append(&d, "10"); continue; }
            ds_append_c(&d, *p);
        }
        fputs(d.data, stdout);
        ds_free(&d);
    } else {
        fputs(code, stdout);
    }
    free(code);
    return 0;
}

static int b_break(int argc, char **argv) { (void)argc; (void)argv; exec_signal_break(); return 0; }
static int b_continue(int argc, char **argv) { (void)argc; (void)argv; exec_signal_continue(); return 0; }
static int b_return(int argc, char **argv) { exec_signal_return(argc > 1 ? atoi(argv[1]) : 0); return g_exit_code; }


static int b_module(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: module list | module load NAME\n"); return 1; }
    if (strcmp(argv[1], "list") == 0) {
        strvec_t *mods = config_modules();
        for (size_t i = 0; i < mods->count; i++) printf("%s\n", mods->items[i]);
        return 0;
    }
    if (strcmp(argv[1], "load") == 0 && argc > 2) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/%s.spire", config_module_dir(), argv[2]);
        int st = exec_source_file(path);
        if (st < 0) { sh_err("module: %s: not found\n", argv[2]); return 1; }
        return st;
    }
    fprintf(stderr, "usage: module list | module load NAME\n");
    return 1;
}

static int b_colorscheme(int argc, char **argv) {
    if (argc < 2) {
        printf("current color scheme (see ~/.config/spire/spire.conf to customize):\n");
        const char *classes[] = {"command","command_invalid","builtin","function","string","variable","operator",
                                  "redirect","comment","keyword","number","path","array","flag","escape",
                                  "suggestion","description","selection", NULL};
        bool tty = isatty(STDOUT_FILENO);
        for (int i = 0; classes[i]; i++) {
            char *code = ansi_code_for(config_color(classes[i]));
            if (tty) printf("  %s%-16s\x1b[0m %s\n", code, classes[i], config_color(classes[i]));
            else printf("  %-16s %s\n", classes[i], config_color(classes[i]));
            free(code);
        }
        return 0;
    }
    if (strcmp(argv[1], "list") == 0) {
        printf("fish\nspire\ndark\nlight\nmono\nsolarized\nnord\ngruvbox\ndracula\n");
        return 0;
    }
    if (strcmp(argv[1], "fish") == 0) {
        config_set("color.command", "default");
        config_set("color.command_invalid", "brightred");
        config_set("color.builtin", "default");
        config_set("color.function", "default");
        config_set("color.keyword", "default");
        config_set("color.string", "yellow");
        config_set("color.variable", "brightcyan");
        config_set("color.operator", "green");
        config_set("color.redirect", "bold-cyan");
        config_set("color.comment", "red");
        config_set("color.number", "cyan");
        config_set("color.path", "cyan");
        config_set("color.array", "cyan");
        config_set("color.flag", "cyan");
        config_set("color.escape", "brightcyan");
        config_set("color.suggestion", "brightblack");
        config_set("color.text", "cyan");
        config_set("color.description", "italic-yellow");
        config_set("color.selection", "reverse");
        config_set("color.completion", "default");
        config_set("color.completion_prefix", "bold-underline-default");
        config_set("color.error", "brightred");
        return 0;
    }
    if (strcmp(argv[1], "spire") == 0) {
        /* spire 1.1's own palette */
        config_set("color.command", "green"); config_set("color.command_invalid", "red");
        config_set("color.builtin", "blue"); config_set("color.function", "brightblue");
        config_set("color.string", "yellow"); config_set("color.variable", "cyan");
        config_set("color.operator", "magenta"); config_set("color.redirect", "brightmagenta");
        config_set("color.comment", "brightblack"); config_set("color.keyword", "blue");
        config_set("color.number", "magenta"); config_set("color.path", "white");
        config_set("color.array", "brightmagenta"); config_set("color.flag", "brightcyan");
        config_set("color.escape", "brightcyan"); config_set("color.text", "default");
        config_set("color.description", "brightblack"); config_set("color.completion_prefix", "default");
        return 0;
    }
    if (strcmp(argv[1], "dark") == 0) {
        config_set("color.command", "green"); config_set("color.string", "yellow");
        config_set("color.variable", "cyan"); config_set("color.operator", "magenta");
        config_set("color.keyword", "blue"); config_set("color.comment", "brightblack");
        config_set("color.path", "white"); config_set("color.array", "brightmagenta");
        config_set("color.flag", "brightcyan"); config_set("color.number", "magenta");
    } else if (strcmp(argv[1], "light") == 0) {
        config_set("color.command", "blue"); config_set("color.string", "green");
        config_set("color.variable", "magenta"); config_set("color.operator", "red");
        config_set("color.keyword", "cyan"); config_set("color.comment", "black");
        config_set("color.path", "black"); config_set("color.array", "magenta");
        config_set("color.flag", "blue"); config_set("color.number", "red");
    } else if (strcmp(argv[1], "mono") == 0) {
        config_set("color.command", "white"); config_set("color.string", "white");
        config_set("color.variable", "white"); config_set("color.operator", "white");
        config_set("color.keyword", "white"); config_set("color.comment", "brightblack");
        config_set("color.path", "white"); config_set("color.array", "white");
        config_set("color.flag", "brightblack"); config_set("color.number", "white");
    } else if (strcmp(argv[1], "solarized") == 0) {
        config_set("color.command", "green"); config_set("color.string", "83");
        config_set("color.variable", "37"); config_set("color.operator", "yellow");
        config_set("color.keyword", "blue"); config_set("color.comment", "brightblack");
        config_set("color.path", "37"); config_set("color.array", "magenta");
        config_set("color.number", "166"); config_set("color.flag", "37");
    } else if (strcmp(argv[1], "nord") == 0) {
        config_set("color.command", "109"); config_set("color.string", "150");
        config_set("color.variable", "111"); config_set("color.operator", "145");
        config_set("color.keyword", "111"); config_set("color.comment", "brightblack");
        config_set("color.path", "145"); config_set("color.array", "175");
        config_set("color.number", "175"); config_set("color.flag", "110");
    } else if (strcmp(argv[1], "gruvbox") == 0) {
        config_set("color.command", "142"); config_set("color.string", "214");
        config_set("color.variable", "109"); config_set("color.operator", "167");
        config_set("color.keyword", "175"); config_set("color.comment", "245");
        config_set("color.path", "223"); config_set("color.array", "208");
        config_set("color.number", "208"); config_set("color.flag", "108");
    } else if (strcmp(argv[1], "dracula") == 0) {
        config_set("color.command", "84"); config_set("color.string", "228");
        config_set("color.variable", "141"); config_set("color.operator", "212");
        config_set("color.keyword", "141"); config_set("color.comment", "61");
        config_set("color.path", "253"); config_set("color.array", "212");
        config_set("color.number", "215"); config_set("color.flag", "117");
    } else {
        sh_err("colorscheme: unknown scheme '%s' "
                         "(try fish, spire, dark, light, mono, solarized, nord, gruvbox, dracula)\n", argv[1]);
        return 1;
    }
    /* classes added later follow the preset's closest relatives */
    config_set("color.redirect", config_color("operator"));
    config_set("color.function", config_color("keyword"));
    config_set("color.builtin", config_color("keyword"));
    config_set("color.escape", config_color("variable"));
    config_set("color.description", config_color("comment"));
    config_set("color.suggestion", config_color("comment"));
    return 0;
}


/* ---------------- table ---------------- */

static BuiltinEntry g_table[] = {
    {"cd", b_cd}, {"pwd", b_pwd}, {"exit", b_exit}, {"unset", b_unset},
    {"alias", b_alias}, {"unalias", b_unalias},
    {"true", b_true}, {"false", b_false}, {"break", b_break}, {"continue", b_continue},
    {"return", b_return}, {"let", b_let}, {"set_color", b_set_color},
    {"module", b_module}, {"colorscheme", b_colorscheme}, {"complete", complete_builtin},
    {NULL, NULL}
};

void builtins_init(void) { /* tables are static; nothing to do */ }

BuiltinFn builtin_lookup(const char *name) {
    BuiltinEntry *extra = builtins_extra_table();
    for (int i = 0; extra[i].name; i++) if (strcmp(extra[i].name, name) == 0) return extra[i].fn;
    for (int i = 0; g_table[i].name; i++) if (strcmp(g_table[i].name, name) == 0) return g_table[i].fn;
    return NULL;
}

bool builtin_exists(const char *name) { return builtin_lookup(name) != NULL; }

void builtins_list_names(strvec_t *out) {
    BuiltinEntry *extra = builtins_extra_table();
    for (int i = 0; extra[i].name; i++) sv_push_dup(out, extra[i].name);
    for (int i = 0; g_table[i].name; i++) sv_push_dup(out, g_table[i].name);
}
