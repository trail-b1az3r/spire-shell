/* fishfuncs.c - functions spire defines at startup so `ls`, `ll` and `la`
 * behave like fish's: colored, with type indicators on a terminal.
 *
 * Same behavior as fish's share/functions/{ls,ll,la}.fish:
 *   - pick the color flag this system's ls understands, once: colorls -G
 *     on OpenBSD, else the first of --color=auto, -G, --color that works
 *   - add -F (type indicators: / * @ ...) only when stdout is a terminal
 *   - Terminal.app: set CLICOLOR so newer BSD ls colors at all
 *   - CLICOLOR_FORCE with piped output: don't pass a color flag
 *   - ll = ls -lh, la = ls -lAh
 * Written for spire (not copied from fish). A user function, alias or
 * `functions -e ls` replaces them. */
#include "fishfuncs.h"
#include "exec.h"
#include "funcs.h"
#include "aliases.h"
#include "config.h"
#include "vars.h"
#include "colors.h"

static const char *g_fish_funcs =
"function ls --description 'List contents of directory'\n"
"    if not set -q __spire_ls_command\n"
"        set -g __spire_ls_command ls\n"
"        set -g __spire_ls_color_opt\n"
"        set -g __spire_ls_indicators_opt\n"
"        if command -q colorls; and command colorls -GF >/dev/null 2>/dev/null\n"
"            set -g __spire_ls_command colorls\n"
"            set -g __spire_ls_color_opt -G\n"
"            set -g __spire_ls_indicators_opt -F\n"
"        else\n"
"            for opt in --color=auto -G --color\n"
"                if command ls $opt / >/dev/null 2>/dev/null\n"
"                    set -g __spire_ls_color_opt $opt\n"
"                    break\n"
"                end\n"
"            end\n"
"            if command ls -F / >/dev/null 2>/dev/null\n"
"                set -g __spire_ls_indicators_opt -F\n"
"            end\n"
"        end\n"
"    end\n"
"    set -l color $__spire_ls_color_opt\n"
"    set -l indicators\n"
"    isatty stdout; and set indicators $__spire_ls_indicators_opt\n"
"    test \"$TERM_PROGRAM\" = Apple_Terminal; and set -lx CLICOLOR 1\n"
"    set -q CLICOLOR_FORCE; and not isatty stdout; and set color\n"
"    command $__spire_ls_command $color $indicators \"$@\"\n"
"end\n"
"function ll --description 'List contents of directory using long format'\n"
"    ls -lh \"$@\"\n"
"end\n"
"function __fish_complete_directories --description 'directories, for completions'\n"
"    set -l tok (commandline -ct)\n"
"    for d in $tok*/\n"
"        test -d \"$d\"; and printf '%s\\tdirectory\\n' $d\n"
"    end\n"
"end\n"
"function __fish_complete_path\n"
"    set -l tok (commandline -ct)\n"
"    for f in $tok*\n"
"        printf '%s\\n' $f\n"
"    end\n"
"end\n"
"function __fish_seen_subcommand_from --description 'is one of these words already on the line?'\n"
"    set -l words (commandline -opc)\n"
"    set -e words[1]\n"
"    for w in $words\n"
"        contains -- $w $argv; and return 0\n"
"    end\n"
"    return 1\n"
"end\n"
"function __fish_use_subcommand --description 'no subcommand typed yet?'\n"
"    set -l words (commandline -opc)\n"
"    set -e words[1]\n"
"    for w in $words\n"
"        string match -q -- '-*' $w; or return 1\n"
"    end\n"
"    return 0\n"
"end\n"
"function __fish_is_first_arg\n"
"    set -l words (commandline -opc)\n"
"    test (count $words) -le 1\n"
"end\n"
"function __fish_is_first_token\n"
"    set -l words (commandline -opc)\n"
"    test (count $words) -eq 0\n"
"end\n"
"function __fish_no_arguments\n"
"    set -l words (commandline -opc)\n"
"    set -e words[1]\n"
"    for w in $words\n"
"        string match -q -- '-*' $w; or return 1\n"
"    end\n"
"    return 0\n"
"end\n"
"function __fish_contains_opt\n"
"    set -l words (commandline -opc)\n"
"    for o in $argv\n"
"        string match -q -- '-s' $o; and continue\n"
"        contains -- --$o $words; and return 0\n"
"        contains -- -$o $words; and return 0\n"
"    end\n"
"    return 1\n"
"end\n"
"function __fish_complete_users\n"
"    command getent passwd 2>/dev/null | string split -f1 :\n"
"end\n"
"function __fish_complete_pids\n"
"    command ps -o pid=,comm= -u (id -u) 2>/dev/null | string trim | string replace -r ' +' '\\t'\n"
"end\n"
"function __fish_print_hostnames\n"
"    command getent hosts 2>/dev/null | string split -f2 ' '\n"
"end\n"
"function la --description 'List contents of directory, including hidden files, using long format'\n"
"    ls -lAh \"$@\"\n"
"end\n";

/* With no $LS_COLORS, GNU ls only colors directories, executables and
 * links. Load the system's full palette (archives, images, media, ...)
 * from `dircolors -b`, as most distributions' shell profiles do. */
static void load_dircolors(void) {
    const char *cur = getenv("LS_COLORS");
    if (cur && *cur) return;
    if (strcmp(config_get("ls_colors.dircolors", "yes"), "no") == 0) return;
    char *out = NULL;
    exec_capture_stdout("command -q dircolors; and command dircolors -b 2>/dev/null", &out);
    if (!out) return;
    const char *start = strstr(out, "LS_COLORS='");
    if (start) {
        start += 11;
        const char *end = strchr(start, '\'');
        if (end && end > start) {
            char *val = xstrndup(start, (size_t)(end - start));
            var_set("LS_COLORS", val, true);
            free(val);
        }
    }
    free(out);
}

void fishfuncs_install(void) {
    if (strcmp(config_get("fish_ls", "yes"), "no") == 0) return;
    load_dircolors();
    exec_source_string(g_fish_funcs);
}

/* ======================= running fish configs ======================= */

#include "arrays.h"
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

static char g_fish_dir[1100];

/* $XDG_CONFIG_HOME/fish or ~/.config/fish */
const char *fish_config_dir(void) {
    if (g_fish_dir[0]) return g_fish_dir;
    const char *xdg = var_get("XDG_CONFIG_HOME");
    const char *home = var_get("HOME");
    if (xdg && *xdg) snprintf(g_fish_dir, sizeof(g_fish_dir), "%s/fish", xdg);
    else snprintf(g_fish_dir, sizeof(g_fish_dir), "%s/.config/fish", home ? home : "");
    return g_fish_dir;
}

static bool fish_enabled(void) { return strcmp(config_get("fish_config", "yes"), "no") != 0; }

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* decodes fish_variables escaping: \xHH bytes, \\ ; \x1e separates list items */
static char *fish_unescape(const char *s) {
    dstr_t d; ds_init(&d);
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
            char hex[3] = { p[2], p[3], 0 };
            ds_append_c(&d, (char)strtol(hex, NULL, 16));
            p += 3;
        } else if (*p == '\\' && p[1]) { ds_append_c(&d, p[1]); p++; }
        else ds_append_c(&d, *p);
    }
    char *r = xstrdup(d.data);
    ds_free(&d);
    return r;
}

/* universal variables: SETUVAR [--export] NAME:VALUE */
static void load_fish_variables(void) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/fish_variables", fish_config_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[16384];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (strncmp(line, "SETUVAR ", 8) != 0) continue;
        char *p = line + 8;
        bool exported = false;
        if (strncmp(p, "--export ", 9) == 0) { exported = true; p += 9; }
        char *colon = strchr(p, ':');
        if (!colon) continue;
        *colon = '\0';
        const char *name = p;
        if (var_get(name) && var_is_exported(name)) continue; /* the environment wins, as in fish */
        char *val = fish_unescape(colon + 1);
        if (strchr(val, '\x1e')) {
            strvec_t items; sv_init(&items);
            char *s0 = val;
            for (char *q = val; ; q++) {
                if (*q == '\x1e' || *q == '\0') {
                    bool end = *q == '\0';
                    *q = '\0';
                    sv_push_dup(&items, s0);
                    if (end) break;
                    s0 = q + 1;
                }
            }
            array_set(name, &items);
            sv_free(&items);
        } else {
            var_set(name, val, exported);
        }
        free(val);
    }
    fclose(f);
}

static void source_dir_fish_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    strvec_t files; sv_init(&files);
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 5 && strcmp(e->d_name + n - 5, ".fish") == 0) sv_push_dup(&files, e->d_name);
    }
    closedir(d);
    qsort(files.items, files.count, sizeof(char *), cmp_names);
    char path[2400];
    for (size_t i = 0; i < files.count; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files.items[i]);
        exec_source_file(path);
    }
    sv_free(&files);
}

/* $fish_user_paths (a universal list in many setups) is prepended to $PATH */
static void apply_fish_user_paths(void) {
    if (!array_exists("fish_user_paths") && !var_get("fish_user_paths")) return;
    strvec_t dirs; sv_init(&dirs);
    if (array_exists("fish_user_paths")) array_get_all("fish_user_paths", &dirs);
    else sv_push_dup(&dirs, var_get("fish_user_paths"));
    const char *cur = var_get("PATH");
    dstr_t d; ds_init(&d);
    for (size_t i = 0; i < dirs.count; i++) {
        if (!dirs.items[i][0]) continue;
        dstr_t probe; ds_init(&probe);
        ds_append_c(&probe, ':'); ds_append(&probe, cur ? cur : ""); ds_append_c(&probe, ':');
        dstr_t needle; ds_init(&needle);
        ds_append_c(&needle, ':'); ds_append(&needle, dirs.items[i]); ds_append_c(&needle, ':');
        bool present = strstr(probe.data, needle.data) != NULL;
        ds_free(&probe); ds_free(&needle);
        if (present) continue;
        if (d.len) ds_append_c(&d, ':');
        ds_append(&d, dirs.items[i]);
    }
    if (d.len) {
        if (cur && *cur) { ds_append_c(&d, ':'); ds_append(&d, cur); }
        var_set("PATH", d.data, true);
    }
    ds_free(&d);
    sv_free(&dirs);
}

/* fish_color_X / fish_pager_color_X -> spire color classes */
static const char *const g_fish_color_map[][2] = {
    {"fish_color_command", "command"}, {"fish_color_command", "builtin"}, {"fish_color_command", "function"},
    {"fish_color_command", "keyword"}, {"fish_color_keyword", "keyword"},
    {"fish_color_param", "text"}, {"fish_color_param", "number"}, {"fish_color_param", "array"},
    {"fish_color_param", "path"}, {"fish_color_param", "flag"}, {"fish_color_option", "flag"},
    {"fish_color_quote", "string"}, {"fish_color_redirection", "redirect"}, {"fish_color_end", "operator"},
    {"fish_color_operator", "variable"}, {"fish_color_escape", "escape"}, {"fish_color_error", "command_invalid"},
    {"fish_color_error", "error"}, {"fish_color_comment", "comment"}, {"fish_color_autosuggestion", "suggestion"},
    {"fish_pager_color_description", "description"}, {"fish_pager_color_prefix", "completion_prefix"},
    {"fish_pager_color_completion", "completion"}, {"fish_pager_color_selected_background", "selection"},
    {NULL, NULL}
};

static char *fish_var_text(const char *name) {
    if (array_exists(name)) {
        strvec_t v; sv_init(&v);
        array_get_all(name, &v);
        dstr_t d; ds_init(&d);
        for (size_t i = 0; i < v.count; i++) { if (i) ds_append_c(&d, ' '); ds_append(&d, v.items[i]); }
        char *r = xstrdup(d.data);
        ds_free(&d); sv_free(&v);
        return r;
    }
    const char *s = var_get(name);
    return s ? xstrdup(s) : NULL;
}

void fish_apply_colors(void) {
    if (!fish_enabled()) return;
    for (int i = 0; g_fish_color_map[i][0]; i++) {
        char *val = fish_var_text(g_fish_color_map[i][0]);
        if (!val) continue;
        char key[64];
        snprintf(key, sizeof(key), "color.%s", g_fish_color_map[i][1]);
        if (strcmp(g_fish_color_map[i][1], "selection") == 0 && !strstr(val, "-r") && !strstr(val, "--reverse")) {
            config_set(key, "reverse"); /* a background color: spire highlights by reversing */
        } else {
            char *spec = fish_color_to_spec(val);
            config_set(key, spec);
            free(spec);
        }
        free(val);
    }
}

/* ---- autoloading: functions/NAME.fish and completions/NAME.fish ---- */

static bool fish_file_for(const char *sub, const char *name, char *out, size_t cap) {
    if (!fish_enabled() || !*name || strchr(name, '/')) return false;
    snprintf(out, cap, "%s/%s/%s.fish", fish_config_dir(), sub, name);
    return access(out, R_OK) == 0;
}

bool fish_function_file_exists(const char *name) {
    char path[1400];
    return fish_file_for("functions", name, path, sizeof(path));
}

bool fish_autoload_function(const char *name) {
    static strvec_t tried;
    static bool init = false;
    if (!init) { sv_init(&tried); init = true; }
    if (func_get(name)) return true;
    for (size_t i = 0; i < tried.count; i++) if (strcmp(tried.items[i], name) == 0) return false;
    char path[1400];
    if (!fish_file_for("functions", name, path, sizeof(path))) return false;
    sv_push_dup(&tried, name);
    exec_source_file(path);
    return func_get(name) != NULL;
}

void fish_autoload_completion(const char *cmd) {
    static strvec_t done;
    static bool init = false;
    if (!init) { sv_init(&done); init = true; }
    for (size_t i = 0; i < done.count; i++) if (strcmp(done.items[i], cmd) == 0) return;
    sv_push_dup(&done, cmd);
    char path[1400];
    if (fish_file_for("completions", cmd, path, sizeof(path))) exec_source_file(path);
}

/* ---- prompts and greeting ---- */

/* output of fish_prompt / fish_right_prompt, or NULL if not defined */
char *fish_prompt_output(const char *fn) {
    if (!fish_enabled()) return NULL;
    if (strcmp(config_get("prompt.use_fish_prompt", "yes"), "no") == 0) return NULL;
    if (!fish_autoload_function(fn)) return NULL;
    char *out = NULL;
    exec_capture_stdout(fn, &out);
    return out;
}

void fish_greeting(void) {
    if (!fish_enabled()) return;
    if (fish_autoload_function("fish_greeting")) { exec_source_string("fish_greeting"); return; }
    char *g = fish_var_text("fish_greeting");
    if (g && *g) printf("%s\n", g);
    free(g);
}

/* loads a fish setup the way fish does at startup: universal variables,
 * conf.d snippets (alphabetically), then config.fish */
void fish_config_load(void) {
    if (!fish_enabled()) return;
    struct stat st;
    if (stat(fish_config_dir(), &st) != 0 || !S_ISDIR(st.st_mode)) return;
    var_set("__fish_config_dir", fish_config_dir(), false);
    load_fish_variables();
    apply_fish_user_paths();
    char path[1300];
    snprintf(path, sizeof(path), "%s/conf.d", fish_config_dir());
    source_dir_fish_files(path);
    snprintf(path, sizeof(path), "%s/config.fish", fish_config_dir());
    exec_source_file(path);
    apply_fish_user_paths();
    fish_apply_colors();
}
