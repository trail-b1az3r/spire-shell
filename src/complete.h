#ifndef SPIRE_COMPLETE_H
#define SPIRE_COMPLETE_H

#include "common.h"

typedef enum {
    CK_COMMAND, CK_BUILTIN, CK_FUNCTION, CK_ALIAS, CK_ABBR, CK_KEYWORD,
    CK_FILE, CK_DIR, CK_EXEC, CK_LINK,
    CK_VAR, CK_OPTION, CK_ARG
} CandKind;

typedef struct {
    char *text;   /* the full replacement word, unescaped */
    char *desc;   /* optional description shown in the menu (may be NULL) */
    CandKind kind;
} Cand;

typedef struct {
    Cand *items;
    size_t count, cap;
} CandList;

void cands_init(CandList *l);
void cands_free(CandList *l);
void cands_add(CandList *l, const char *text, const char *desc, CandKind kind);

/* Result of completing the word under the cursor. */
typedef struct {
    CandList cands;
    size_t word_start;  /* byte offset in the line where the replaced text begins */
    char quote;         /* the word was opened with ' or " (0 if not) */
} Completion;

/* Finds completions for the word touching `cursor` in `line`. Candidates
 * are sorted, deduplicated, and prefix-matched (with a fuzzy fallback). */
void complete_line(const char *line, size_t cursor, Completion *out);
void completion_free(Completion *c);

/* Escapes a candidate for insertion into the command line (backslashes
 * before spaces and shell metacharacters, unless inside `quote`). */
char *complete_escape(const char *text, char quote);

/* "did you mean" for an unknown command: the closest known command name
 * (builtins, functions, aliases, $PATH) within a small edit distance, or
 * NULL. Caller frees. */
char *complete_suggest_command(const char *name);

#endif
