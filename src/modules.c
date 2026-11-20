#include "modules.h"
#include "config.h"
#include "exec.h"
#include "common.h"
#include <stdio.h>

void modules_load_enabled(void) {
    strvec_t *mods = config_modules();
    for (size_t i = 0; i < mods->count; i++) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/%s.spire", config_module_dir(), mods->items[i]);
        if (exec_source_file(path) < 0) {
            sh_err("module '%s' not found (looked for %s)\n", mods->items[i], path);
        }
    }
}
