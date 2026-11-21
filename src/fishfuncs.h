#ifndef SPIRE_FISHFUNCS_H
#define SPIRE_FISHFUNCS_H
#include "common.h"
/* defines fish-style ls/ll/la functions (see fishfuncs.c) */
void fishfuncs_install(void);

/* running real fish configs (~/.config/fish) */
const char *fish_config_dir(void);
void fish_config_load(void);                 /* fish_variables, conf.d, config.fish */
void fish_apply_colors(void);                /* fish_color_* -> spire colors */
bool fish_function_file_exists(const char *name);
bool fish_autoload_function(const char *name); /* functions/NAME.fish */
void fish_autoload_completion(const char *cmd); /* completions/CMD.fish */
char *fish_prompt_output(const char *fn);    /* fish_prompt / fish_right_prompt output */
void fish_greeting(void);
#endif
