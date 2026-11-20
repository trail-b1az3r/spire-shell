#ifndef SPIRE_LEXER_H
#define SPIRE_LEXER_H

#include "common.h"

typedef enum {
    TOK_WORD,
    TOK_PIPE,       /* |    */
    TOK_AND,        /* &&   */
    TOK_OR,         /* ||   */
    TOK_SEMI,       /* ;    */
    TOK_BG,         /* &    */
    TOK_PIPE_AMP,   /* |&   */
    TOK_REDIR,      /* any redirection operator: text holds the operator
                       ("<" ">" ">>" "<>" ">&" "<&" "&>" "&>>" "<<" "<<-" "<<<"),
                       fd holds an explicit leading fd number or -1 */
    TOK_RPAREN,     /* bare ')' - case-arm terminator / subshell close   */
    TOK_NEWLINE,
    TOK_EOF
} TokType;

typedef struct {
    TokType type;
    char *text; /* owned; the word for TOK_WORD, the operator for TOK_REDIR */
    int fd;     /* TOK_REDIR only: explicit fd prefix (2>), or -1 */
} Token;

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
    size_t heredoc_resume; /* when >0: at the next newline, jump here (past pending heredoc bodies) */
} Lexer;

void lexer_init(Lexer *lx, const char *src);
void lexer_next(Lexer *lx, Token *out);
void token_free(Token *t);

/* Quick pre-scan used by the REPL to know whether more input is needed
 * before attempting a real parse (unterminated quotes / trailing backslash). */
bool input_needs_more(const char *s);

#endif
