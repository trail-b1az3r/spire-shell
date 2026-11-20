#ifndef SPIRE_AST_H
#define SPIRE_AST_H

#include "common.h"

typedef enum {
    N_CMD,      /* simple command: argv (raw words) + env_assigns + redirects   */
    N_PIPELINE, /* children = commands piped left to right                      */
    N_AND,      /* children[0] && children[1]                                    */
    N_OR,       /* children[0] || children[1]                                    */
    N_SEQ,      /* children run in order, separated by ; or newline              */
    N_IF,       /* children[0]=cond children[1]=then children[2]=else(or NULL)   */
    N_WHILE,    /* children[0]=cond children[1]=body                             */
    N_UNTIL,    /* children[0]=cond children[1]=body (loops while cond is false)  */
    N_FOR,      /* for_var/for_words, children[0]=body                           */
    N_FUNCDEF,  /* func_name, children[0]=body                                   */
    N_BLOCK,    /* children = list of statements (function/if/while/for bodies)  */
    N_CASE,     /* case_subject; children alternate [patterns(N_CMD.argv), body(N_BLOCK)]... */
    N_SUBSHELL, /* children[0] = body, runs in a forked, state-isolated child     */
    N_CONDATOM, /* one `[[ ... ]]` test atom: argv holds its raw words            */
    N_FISH_AND, /* fish `and CMD`: children[0] runs only if the last status was 0  */
    N_FISH_OR,  /* fish `or CMD`: children[0] runs only if the last status was !0  */
    N_TIME      /* `time PIPELINE`: children[0], prints real/user/sys afterward    */
} NodeType;

typedef enum {
    R_IN,             /* fd<  file   (fd defaults to 0)                       */
    R_OUT,            /* fd>  file   (fd defaults to 1)                       */
    R_APPEND,         /* fd>> file                                            */
    R_RDWR,           /* fd<> file                                            */
    R_DUP,            /* fd>&N / fd<&N, or fd>&- to close                     */
    R_OUT_ERR,        /* &>  file : stdout+stderr                              */
    R_OUT_ERR_APPEND, /* &>> file                                             */
    R_HEREDOC,        /* << / <<-                                             */
    R_HERESTRING      /* <<< word                                             */
} RedirType;

typedef struct Redirect {
    RedirType type;
    int fd;               /* the descriptor being redirected */
    char *target;         /* raw word, expanded at exec time (filename or fd);
                              for R_HEREDOC, the (already dequoted) delimiter */
    char *heredoc_body;    /* R_HEREDOC only: the captured body text, raw */
    bool heredoc_no_expand;/* R_HEREDOC only: delimiter was quoted -> no $ expansion */
    struct Redirect *next;
} Redirect;

typedef struct Node {
    NodeType type;

    /* N_CMD */
    strvec_t argv;          /* raw (unexpanded) words, argv[0] is command name */
    strvec_t env_assigns;   /* "NAME=value" prefix assignments, e.g. FOO=bar cmd */
    Redirect *redirects;

    /* generic children list, meaning depends on type (see NodeType comments) */
    struct Node **children;
    size_t nchildren;
    size_t children_cap;

    bool background;        /* trailing & */
    bool negate;             /* leading ! (pipeline negation, or a [[ ]] atom's !) */
    bool pipe_stderr;        /* pipeline stage followed by |& : stderr joins the pipe */

    /* N_FOR */
    char *for_var;
    strvec_t for_words;

    /* N_FUNCDEF */
    char *func_name;

    /* N_CASE */
    char *case_subject;
} Node;

Node *node_new(NodeType type);
void node_add_child(Node *n, Node *child);
void node_free(Node *n);
Redirect *redirect_new(RedirType type, const char *target);

#endif
