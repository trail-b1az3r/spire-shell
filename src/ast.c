#include "ast.h"

Node *node_new(NodeType type) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->type = type;
    sv_init(&n->argv);
    sv_init(&n->env_assigns);
    sv_init(&n->for_words);
    n->children = NULL;
    n->nchildren = 0;
    n->children_cap = 0;
    return n;
}

void node_add_child(Node *n, Node *child) {
    if (n->nchildren + 1 > n->children_cap) {
        n->children_cap = n->children_cap ? n->children_cap * 2 : 4;
        n->children = xrealloc(n->children, sizeof(Node *) * n->children_cap);
    }
    n->children[n->nchildren++] = child;
}

Redirect *redirect_new(RedirType type, const char *target) {
    Redirect *r = xmalloc(sizeof(Redirect));
    r->type = type;
    r->fd = (type == R_IN || type == R_RDWR || type == R_HEREDOC || type == R_HERESTRING) ? 0 : 1;
    r->target = xstrdup(target);
    r->heredoc_body = NULL;
    r->heredoc_no_expand = false;
    r->next = NULL;
    return r;
}

void node_free(Node *n) {
    if (!n) return;
    sv_free(&n->argv);
    sv_free(&n->env_assigns);
    sv_free(&n->for_words);
    Redirect *r = n->redirects;
    while (r) {
        Redirect *next = r->next;
        free(r->target);
        free(r->heredoc_body);
        free(r);
        r = next;
    }
    for (size_t i = 0; i < n->nchildren; i++) node_free(n->children[i]);
    free(n->children);
    free(n->for_var);
    free(n->func_name);
    free(n->case_subject);
    free(n);
}
