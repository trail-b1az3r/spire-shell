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

#endif
