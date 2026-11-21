#ifndef SPIRE_COLORS_H
#define SPIRE_COLORS_H

#include "common.h"

/* Returns a heap string containing the ANSI escape sequence for a color
 * name ("green", "brightcyan", "default", or a raw 256-color number like
 * "208"). Caller frees. */
char *ansi_code_for(const char *name);

#define ANSI_RESET "\x1b[0m"

/* Appends `\x1b[..m` + text[0..len) + reset to *out. */
void color_wrap_n(dstr_t *out, const char *color_name, const char *text, size_t len);
void color_wrap(dstr_t *out, const char *color_name, const char *text);

/* LS_COLORS lookup for a file name. kind: 0 regular, 1 directory,
 * 2 executable, 3 symlink. Returns SGR parameters like "01;34" (static
 * storage) or NULL for "no color". Falls back to built-in defaults when
 * $LS_COLORS is unset. */
const char *lscolor_for(const char *name, int kind);

/* display width of `s` in terminal columns, skipping ANSI escape sequences
 * and counting wide (CJK/emoji) characters as 2 */
size_t display_width(const char *s);
size_t display_width_n(const char *s, size_t n);

/* ---- fish color specs ----
 * fish writes colors as words: names (red, brred, brblack...), hex
 * (5f87af / #5f87af / f00), normal / --reset, and options --bold/-o,
 * --underline/-u, --italics/-i, --dim/-d, --reverse/-r,
 * --background=COLOR / -b COLOR. */

/* the escape sequence for fish-style set_color arguments */
char *fish_color_ansi(int argc, char **argv);

/* a fish color value ("blue --bold", "5f87af") as a spire color spec
 * ("bold-blue", "#5f87af"); background colors are dropped. Caller frees. */
char *fish_color_to_spec(const char *value);

#endif
