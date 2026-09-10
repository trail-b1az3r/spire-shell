#ifndef SPIRE_VARS_H
#define SPIRE_VARS_H

#include "common.h"

void vars_init(void);                       /* import process environ */
const char *var_get(const char *name);       /* NULL if unset */
void var_set(const char *name, const char *value, bool exported);
void var_export(const char *name);           /* mark existing var exported */
void var_unset(const char *name);
bool var_is_exported(const char *name);
void vars_dump(strvec_t *names_out); /* every currently-set variable name */

/* special vars: $?, $$, $0, positional handled by exec/main directly via var_set */

/* ---- function-local variable scoping (the `local` builtin) ----
 * Dynamically scoped, matching real bash: entering a function call pushes
 * a frame; `local NAME` within it saves NAME's current value (or "unset")
 * into that frame the first time it's declared there; popping the frame on
 * return restores (or unsets) every name it saved. */
void var_push_scope(void);
void var_pop_scope(void);
bool var_declare_local(const char *name); /* false if not inside any scope */
bool var_in_function_scope(void);

#endif
