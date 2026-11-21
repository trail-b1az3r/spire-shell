#ifndef SPIRE_CONFIG_H
#define SPIRE_CONFIG_H

#include "common.h"

void config_init(void);                 /* set built-in defaults */
void config_load(const char *path);     /* load & merge a config file, if it exists */
void config_load_default(void);         /* load ~/.config/spire/spire.conf */

const char *config_get(const char *key, const char *fallback);
void config_set(const char *key, const char *value);

const char *config_prompt(void);
const char *config_color(const char *class_name); /* e.g. "command", "string" ... */
int config_history_size(void);
const char *config_history_path(void);
const char *config_module_dir(void);
strvec_t *config_modules(void); /* enabled module names, do not free */

const char *config_dir(void);

/* called for `colorscheme = NAME` lines, in file order (set by main) */
extern void (*config_scheme_hook)(const char *name);   /* ~/.config/spire */

#endif
