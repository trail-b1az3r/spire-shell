#ifndef SPIRE_BUILTINS_H
#define SPIRE_BUILTINS_H

#include "common.h"

typedef int (*BuiltinFn)(int argc, char **argv);

typedef struct { const char *name; BuiltinFn fn; } BuiltinEntry;
BuiltinEntry *builtins_extra_table(void);  /* builtins_extra.c */

/* fish abbreviations (expanded by the line editor on space/enter) */
const char *abbr_get(const char *name);
void abbr_list(strvec_t *names);

/* `complete` (complete.c) */
int complete_builtin(int argc, char **argv);

void builtins_init(void);
BuiltinFn builtin_lookup(const char *name);
bool builtin_exists(const char *name);
void builtins_list_names(strvec_t *out);

/* the `test`/`[` evaluator, exposed so exec.c can reuse it for the
 * non-glob, non-regex operators inside `[[ ... ]]` atoms. */
int test_eval(int argc, char **argv);

#endif
