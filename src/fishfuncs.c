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
