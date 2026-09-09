#include "config.h"
#include "vars.h"
#include <ctype.h>
#include <sys/stat.h>

#define CFG_BUCKETS 128

typedef struct CfgEntry {
    char *key;
    char *value;
    struct CfgEntry *next;
} CfgEntry;

static CfgEntry *buckets[CFG_BUCKETS];
static strvec_t g_modules;
static char g_cfgdir[1100];
static char g_histpath[1100];
static char g_moduledir[1100];

static unsigned hash_key(const char *s) {
    unsigned h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)(*s++);
    return h % CFG_BUCKETS;
}

void config_set(const char *key, const char *value) {
    unsigned h = hash_key(key);
    for (CfgEntry *e = buckets[h]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) { free(e->value); e->value = xstrdup(value); return; }
    }
    CfgEntry *e = xmalloc(sizeof(CfgEntry));
    e->key = xstrdup(key);
    e->value = xstrdup(value);
    e->next = buckets[h];
    buckets[h] = e;
}

const char *config_get(const char *key, const char *fallback) {
    unsigned h = hash_key(key);
    for (CfgEntry *e = buckets[h]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e->value;
    return fallback;
}

const char *config_dir(void) { return g_cfgdir; }
const char *config_history_path(void) { return g_histpath; }
const char *config_module_dir(void) { return g_moduledir; }

void config_init(void) {
    for (int i = 0; i < CFG_BUCKETS; i++) buckets[i] = NULL;
    sv_init(&g_modules);

    const char *home = var_get("HOME");
    if (!home) home = "/tmp";
    snprintf(g_cfgdir, sizeof(g_cfgdir), "%s/.config/spire", home);
    snprintf(g_histpath, sizeof(g_histpath), "%s/.local/share/spire/history", home);
    snprintf(g_moduledir, sizeof(g_moduledir), "%s/.config/spire/modules", home);

    /* defaults: fish-like palette, but fully overridable. ▲ stands in for
     * a spire/tower glyph — swap for 🗼 in the config if your terminal
     * has good emoji coverage. */
    config_set("prompt", "%F{cyan}%n%f@%F{cyan}%m%f %F{yellow}%~%f %F{brightblack}%g%f %F{green}▲❯%f ");
    config_set("color.command", "green");
    config_set("color.command_invalid", "red");
    config_set("color.builtin", "blue");
    config_set("color.string", "yellow");
    config_set("color.variable", "cyan");
    config_set("color.operator", "magenta");
    config_set("color.comment", "brightblack");
    config_set("color.keyword", "blue");
    config_set("color.number", "magenta");
    config_set("color.path", "white");
    config_set("color.array", "brightmagenta");
    config_set("color.flag", "brightcyan");
    config_set("color.suggestion", "brightblack");
    config_set("color.text", "default");
    config_set("history.size", "5000");
}

static void ensure_dirs(void) {
    char buf[1200];
    const char *home = var_get("HOME");
    if (home) {
        snprintf(buf, sizeof(buf), "%s/.config", home);
        mkdir(buf, 0755);
    }
    snprintf(buf, sizeof(buf), "%s", g_cfgdir);
    mkdir(buf, 0755);
    snprintf(buf, sizeof(buf), "%s/modules", g_cfgdir);
    mkdir(buf, 0755);
    if (home) {
        snprintf(buf, sizeof(buf), "%s/.local", home);
        mkdir(buf, 0755);
        snprintf(buf, sizeof(buf), "%s/.local/share", home);
        mkdir(buf, 0755);
        snprintf(buf, sizeof(buf), "%s/.local/share/spire", home);
        mkdir(buf, 0755);
    }
}

static void parse_modules_csv(const char *csv) {
    sv_clear(&g_modules);
    const char *p = csv;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' ) p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (end > start) sv_push(&g_modules, xstrndup(start, (size_t)(end - start)));
    }
}

void config_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        trim_ws(s);
        if (s[0] == '\0' || s[0] == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = s;
        char *val = eq + 1;
        trim_ws(key);
        trim_ws(val);
        size_t vl = strlen(val);
        if (vl >= 2 && val[0] == '"' && val[vl-1] == '"') { val[vl-1] = '\0'; val++; }
        if (strcmp(key, "modules") == 0) { parse_modules_csv(val); continue; }
        config_set(key, val);
    }
    fclose(f);
}

void config_load_default(void) {
    ensure_dirs();
    char path[1200];
    snprintf(path, sizeof(path), "%s/spire.conf", g_cfgdir);
    struct stat st;
    if (stat(path, &st) != 0) {
        /* first run: write out a starter config so it's discoverable */
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f,
                "# spire configuration - see `man spire` / README for the full key list\n"
                "prompt = \"%%F{cyan}%%n%%f@%%F{cyan}%%m%%f %%F{yellow}%%~%%f %%F{brightblack}%%g%%f %%F{green}▲❯%%f \"\n"
                "color.command = green\n"
                "color.command_invalid = red\n"
                "color.builtin = blue\n"
                "color.string = yellow\n"
                "color.variable = cyan\n"
                "color.operator = magenta\n"
                "color.comment = brightblack\n"
                "color.keyword = blue\n"
                "color.number = magenta\n"
                "color.path = white\n"
                "color.array = brightmagenta\n"
                "color.flag = brightcyan\n"
                "color.suggestion = brightblack\n"
                "history.size = 5000\n"
                "modules = \n");
            fclose(f);
        }
    }
    config_load(path);
}

const char *config_prompt(void) { return config_get("prompt", "spire> "); }
const char *config_color(const char *class_name) {
    char key[64];
    snprintf(key, sizeof(key), "color.%s", class_name);
    return config_get(key, "default");
}
int config_history_size(void) {
    const char *v = config_get("history.size", "5000");
    int n = atoi(v);
    return n > 0 ? n : 5000;
}
strvec_t *config_modules(void) { return &g_modules; }
