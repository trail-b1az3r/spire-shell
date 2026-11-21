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

static void sv_copy(strvec_t *dst, const strvec_t *src) {
    for (size_t i = 0; i < src->count; i++) sv_push_dup(dst, src->items[i]);
}

Node *node_clone(const Node *n) {
    if (!n) return NULL;
    Node *c = node_new(n->type);
    sv_copy(&c->argv, &n->argv);
    sv_copy(&c->env_assigns, &n->env_assigns);
    sv_copy(&c->for_words, &n->for_words);
    Redirect **tail = &c->redirects;
    for (const Redirect *r = n->redirects; r; r = r->next) {
        Redirect *rc = redirect_new(r->type, r->target);
        rc->fd = r->fd;
        rc->heredoc_body = r->heredoc_body ? xstrdup(r->heredoc_body) : NULL;
        rc->heredoc_no_expand = r->heredoc_no_expand;
        *tail = rc;
        tail = &rc->next;
    }
    for (size_t i = 0; i < n->nchildren; i++) node_add_child(c, node_clone(n->children[i]));
    c->background = n->background;
    c->negate = n->negate;
    c->pipe_stderr = n->pipe_stderr;
    c->for_var = n->for_var ? xstrdup(n->for_var) : NULL;
    c->func_name = n->func_name ? xstrdup(n->func_name) : NULL;
    c->case_subject = n->case_subject ? xstrdup(n->case_subject) : NULL;
    return c;
}
