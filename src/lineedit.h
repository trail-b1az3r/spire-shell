#ifndef SPIRE_LINEEDIT_H
#define SPIRE_LINEEDIT_H

#include "common.h"

void lineedit_init(void);
void lineedit_shutdown(void);

/* Reads one logical line from the terminal in raw mode, with live
 * highlighting, history browsing, and tab completion. Returns a newly
 * allocated string (no trailing newline), or NULL on EOF (Ctrl-D on an
 * empty line). `prompt_fmt` is the *unrendered* prompt template (re-rendered
 * fresh so %~ / %g stay current); pass NULL to use the continuation
 * prompt instead. */
char *lineedit_read(const char *prompt_fmt);

/* the line currently being edited (empty when not editing), for `commandline` */
const char *lineedit_buffer(void);
size_t lineedit_cursor(void);

/* use already-rendered prompt text (e.g. fish_prompt's output) for the
 * next primary prompt instead of the configured format; NULL = none */
void lineedit_set_prompt_override(const char *left, const char *right);

#endif
