#include "parser.h"
#include "lexer.h"
#include <ctype.h>
#include <stdarg.h>

typedef struct {
    Lexer lx;
    Token cur;
    Token la;
    bool la_valid;
    bool incomplete;
    bool error;
    char *errmsg;
} Parser;

/* forward decls */
static Node *parse_and_or_bg(Parser *p);
static Node *parse_pipeline(Parser *p);
static Node *parse_command(Parser *p);
static Node *parse_simple_command(Parser *p);
static Node *parse_if(Parser *p);
static Node *parse_while(Parser *p);
static Node *parse_until(Parser *p);
static Node *parse_for(Parser *p);
static Node *parse_funcdef_kw(Parser *p);
static Node *parse_funcdef_parens(Parser *p, size_t namelen);
static Node *parse_case(Parser *p);
static Node *parse_switch(Parser *p);
static Node *parse_cond(Parser *p);
static bool parse_stmt_list(Parser *p, Node *block, const char **stops);

static void set_error(Parser *p, const char *fmt, ...) {
    if (p->error) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    p->error = true;
    p->errmsg = xstrdup(buf);
}

static void p_advance(Parser *p) {
    token_free(&p->cur);
    if (p->la_valid) { p->cur = p->la; p->la_valid = false; }
    else lexer_next(&p->lx, &p->cur);
}

static Token *p_peek2(Parser *p) {
    if (!p->la_valid) { lexer_next(&p->lx, &p->la); p->la_valid = true; }
    return &p->la;
}

static void skip_separators(Parser *p) {
    while (p->cur.type == TOK_SEMI || p->cur.type == TOK_NEWLINE) p_advance(p);
}

static void skip_newlines(Parser *p) {
    while (p->cur.type == TOK_NEWLINE) p_advance(p);
}

static bool word_is(Parser *p, const char *kw) {
    return p->cur.type == TOK_WORD && strcmp(p->cur.text, kw) == 0;
}

static bool at_stop(Parser *p, const char **stops) {
    if (!stops || p->cur.type != TOK_WORD) return false;
    for (int i = 0; stops[i]; i++) if (strcmp(p->cur.text, stops[i]) == 0) return true;
    return false;
}

static bool expect_word(Parser *p, const char *kw) {
    if (!word_is(p, kw)) {
        set_error(p, "syntax error: expected '%s'", kw);
        return false;
    }
    p_advance(p);
    return true;
}

static bool is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static bool looks_like_assignment(const char *s) {
    if (!is_ident_start(s[0])) return false;
    size_t i = 1;
    while (s[i] && is_ident_char(s[i])) i++;
    if (s[i] == '[') {
        int depth = 1; i++;
        while (s[i] && depth > 0) {
            if (s[i] == '[') depth++;
            else if (s[i] == ']') depth--;
            i++;
        }
    }
    if (s[i] == '+' && s[i+1] == '=') return true;
    return s[i] == '=';
}

/* "name()" -> true, *namelen = strlen("name") */
static bool ends_with_parens(const char *s, size_t *namelen) {
    size_t n = strlen(s);
    if (n < 3) return false;
    if (!(s[n - 2] == '(' && s[n - 1] == ')')) return false;
    if (!is_ident_start(s[0])) return false;
    for (size_t i = 1; i < n - 2; i++) if (!is_ident_char(s[i])) return false;
    *namelen = n - 2;
    return true;
}

/* ---------------- grammar ---------------- */

static Node *parse_block(Parser *p, const char **stops) {
    Node *b = node_new(N_BLOCK);
    if (!parse_stmt_list(p, b, stops)) { node_free(b); return NULL; }
    return b;
}

static bool parse_stmt_list(Parser *p, Node *block, const char **stops) {
    for (;;) {
        skip_separators(p);
        if (p->cur.type == TOK_EOF) {
            if (stops) { p->incomplete = true; return false; }
            return true;
        }
        if (at_stop(p, stops)) return true;
        Node *stmt = parse_and_or_bg(p);
        if (!stmt) return false;
        node_add_child(block, stmt);
        if (p->cur.type == TOK_SEMI || p->cur.type == TOK_NEWLINE || stmt->background) continue;
        if (p->cur.type == TOK_EOF || at_stop(p, stops)) continue;
        set_error(p, "syntax error: unexpected token in statement list");
        return false;
    }
}

static Node *parse_and_or_bg(Parser *p) {
    Node *left = parse_pipeline(p);
    if (!left) return NULL;
    while (p->cur.type == TOK_AND || p->cur.type == TOK_OR) {
        NodeType nt = (p->cur.type == TOK_AND) ? N_AND : N_OR;
        p_advance(p);
        skip_newlines(p);
        Node *right = parse_pipeline(p);
        if (!right) { node_free(left); return NULL; }
        Node *n = node_new(nt);
        node_add_child(n, left);
        node_add_child(n, right);
        left = n;
    }
    if (p->cur.type == TOK_BG) {
        left->background = true;
        p_advance(p);
    }
    return left;
}

static Node *parse_pipeline(Parser *p) {
    bool neg = false;
    while (word_is(p, "!")) { neg = !neg; p_advance(p); }

    Node *first = parse_command(p);
    if (!first) return NULL;
    if (p->cur.type != TOK_PIPE) { if (neg) first->negate = !first->negate; return first; }
    Node *pipe = node_new(N_PIPELINE);
    node_add_child(pipe, first);
    while (p->cur.type == TOK_PIPE) {
        p_advance(p);
        skip_newlines(p);
        Node *next = parse_command(p);
        if (!next) { node_free(pipe); return NULL; }
        node_add_child(pipe, next);
    }
    if (neg) pipe->negate = !pipe->negate;
    return pipe;
}

static Node *parse_command(Parser *p) {
    if (p->cur.type != TOK_WORD) {
        set_error(p, "syntax error: unexpected token");
        return NULL;
    }
    if (strcmp(p->cur.text, "if") == 0) return parse_if(p);
    if (strcmp(p->cur.text, "while") == 0) return parse_while(p);
    if (strcmp(p->cur.text, "until") == 0) return parse_until(p);
    if (strcmp(p->cur.text, "for") == 0) return parse_for(p);
    if (strcmp(p->cur.text, "function") == 0) return parse_funcdef_kw(p);
    if (strcmp(p->cur.text, "case") == 0) return parse_case(p);
    if (strcmp(p->cur.text, "switch") == 0) return parse_switch(p);
    if (strcmp(p->cur.text, "[[") == 0) return parse_cond(p);
    {
        size_t tl = strlen(p->cur.text);
        if (tl >= 5 && str_has_prefix(p->cur.text, "((") && p->cur.text[tl-1] == ')' && p->cur.text[tl-2] == ')') {
            Node *n = node_new(N_CMD);
            sv_push_dup(&n->argv, "((");
            char *inner = xstrndup(p->cur.text + 2, tl - 4);
            sv_push(&n->argv, inner);
            p_advance(p);
            return n;
        }
        if (tl >= 2 && p->cur.text[0] == '(' && p->cur.text[tl-1] == ')') {
            char *inner = xstrndup(p->cur.text + 1, tl - 2);
            p_advance(p);
            Node *subroot = NULL; char *err = NULL;
            ParseStatus st = parse_program(inner, &subroot, &err);
            free(inner);
            Node *n = node_new(N_SUBSHELL);
            if (st == PARSE_OK) { node_add_child(n, subroot); return n; }
            if (st == PARSE_EMPTY) { node_add_child(n, node_new(N_SEQ)); return n; }
            node_free(n);
            set_error(p, "syntax error in subshell: %s", err ? err : "invalid");
            free(err);
            return NULL;
        }
    }
    size_t nl;
    if (ends_with_parens(p->cur.text, &nl)) return parse_funcdef_parens(p, nl);
    return parse_simple_command(p);
}

static char *dequote_delim(const char *raw, bool *was_quoted) {
    size_t n = strlen(raw);
    if (n >= 2 && ((raw[0] == '\'' && raw[n-1] == '\'') || (raw[0] == '"' && raw[n-1] == '"'))) {
        *was_quoted = true;
        return xstrndup(raw + 1, n - 2);
    }
    *was_quoted = false;
    return xstrdup(raw);
}

/* Called right after consuming the heredoc delimiter word. Scans the raw
 * source directly (bypassing tokenization) for the heredoc body: from the
 * end of the current line, through subsequent lines, until one matches the
 * delimiter (after stripping leading tabs, if `strip`). Resyncs the lexer
 * to continue right after the delimiter line. Sets p->incomplete if EOF is
 * hit first (caller should request more input). */
static void consume_heredoc_body(Parser *p, Redirect *r, bool strip, size_t start_pos) {
    const char *src = p->lx.src;
    size_t pos = start_pos;
    while (src[pos] && src[pos] != '\n') pos++;
    if (src[pos] == '\n') pos++;

    dstr_t body; ds_init(&body);
    size_t delim_len = strlen(r->target);

    for (;;) {
        size_t line_start = pos;
        while (src[pos] && src[pos] != '\n') pos++;
        size_t line_len = pos - line_start;
        bool at_eof = (src[pos] == '\0');
        if (src[pos] == '\n') pos++;

        const char *line = src + line_start;
        const char *cmp = line;
        size_t cmp_len = line_len;
        if (strip) { while (cmp_len > 0 && *cmp == '\t') { cmp++; cmp_len--; } }

        if (cmp_len == delim_len && strncmp(cmp, r->target, delim_len) == 0) break;

        const char *body_line = line;
        size_t body_len = line_len;
        if (strip) { while (body_len > 0 && *body_line == '\t') { body_line++; body_len--; } }
        ds_append_n(&body, body_line, body_len);
        ds_append_c(&body, '\n');

        if (at_eof) { p->incomplete = true; break; }
    }

    r->heredoc_body = xstrdup(body.data);
    ds_free(&body);

    p->lx.pos = pos;
    token_free(&p->cur);
    if (p->la_valid) { token_free(&p->la); p->la_valid = false; }
    if (!p->incomplete) lexer_next(&p->lx, &p->cur);
}

static Node *parse_simple_command(Parser *p) {
    Node *n = node_new(N_CMD);
    Redirect **tail = &n->redirects;
    for (;;) {
        if (p->cur.type == TOK_WORD) {
            if (n->argv.count == 0 && looks_like_assignment(p->cur.text)) {
                sv_push_dup(&n->env_assigns, p->cur.text);
                p_advance(p);
                continue;
            }
            sv_push_dup(&n->argv, p->cur.text);
            p_advance(p);
            continue;
        }
        RedirType rt; bool isredir = true;
        switch (p->cur.type) {
            case TOK_LT: rt = R_IN; break;
            case TOK_GT: rt = R_OUT; break;
            case TOK_DGT: rt = R_APPEND; break;
            case TOK_GTERR: rt = R_ERR; break;
            case TOK_DGTERR: rt = R_ERR_APPEND; break;
            case TOK_ERR2OUT: rt = R_ERR_TO_OUT; break;
            case TOK_GTAMP: rt = R_OUT_ERR; break;
            case TOK_DLT: case TOK_DLT_DASH: rt = R_HEREDOC; break;
            default: isredir = false; rt = R_IN; break;
        }
        if (isredir) {
            bool strip = (p->cur.type == TOK_DLT_DASH);
            p_advance(p);
            if (rt == R_ERR_TO_OUT) {
                Redirect *r = redirect_new(rt, "");
                *tail = r; tail = &r->next;
                continue;
            }
            if (p->cur.type != TOK_WORD) {
                set_error(p, "syntax error: expected redirection target");
                node_free(n); return NULL;
            }
            if (rt == R_HEREDOC) {
                bool quoted;
                char *delim = dequote_delim(p->cur.text, &quoted);
                Redirect *r = redirect_new(rt, delim);
                r->heredoc_no_expand = quoted;
                free(delim);
                size_t pos_after_delim = p->lx.pos; /* while p->cur is still the delimiter word */
                p_advance(p);
                consume_heredoc_body(p, r, strip, pos_after_delim);
                *tail = r; tail = &r->next;
                if (p->incomplete) { node_free(n); return NULL; }
                continue;
            }
            Redirect *r = redirect_new(rt, p->cur.text);
            *tail = r; tail = &r->next;
            p_advance(p);
            continue;
        }
        break;
    }
    if (n->argv.count == 0 && n->env_assigns.count == 0 && n->redirects == NULL) {
        set_error(p, "syntax error near unexpected token");
        node_free(n);
        return NULL;
    }
    return n;
}

static Node *parse_if(Parser *p) {
    p_advance(p); /* if */
    Node *if_node = node_new(N_IF);
    Node *cond = parse_and_or_bg(p);
    if (!cond) { node_free(if_node); return NULL; }
    node_add_child(if_node, cond);

    bool bash_style = false;
    skip_separators(p);
    if (word_is(p, "then")) { p_advance(p); bash_style = true; }

    const char *stops_bash[] = { "elif", "else", "fi", NULL };
    const char *stops_fish[] = { "else", "end", NULL };
    Node *body = parse_block(p, bash_style ? stops_bash : stops_fish);
    if (!body) { node_free(if_node); return NULL; }
    node_add_child(if_node, body);

    Node *cur = if_node;
    for (;;) {
        if (p->cur.type == TOK_EOF) { p->incomplete = true; node_free(if_node); return NULL; }
        if (bash_style && word_is(p, "elif")) {
            p_advance(p);
            Node *c2 = parse_and_or_bg(p);
            if (!c2) { node_free(if_node); return NULL; }
            skip_separators(p);
            if (!expect_word(p, "then")) { node_free(c2); node_free(if_node); return NULL; }
            Node *b2 = parse_block(p, stops_bash);
            if (!b2) { node_free(c2); node_free(if_node); return NULL; }
            Node *nested = node_new(N_IF);
            node_add_child(nested, c2);
            node_add_child(nested, b2);
            node_add_child(cur, nested);
            cur = nested;
            continue;
        }
        if (!bash_style && word_is(p, "else")) {
            Token *la = p_peek2(p);
            if (la->type == TOK_WORD && strcmp(la->text, "if") == 0) {
                p_advance(p); /* else */
                p_advance(p); /* if */
                Node *c2 = parse_and_or_bg(p);
                if (!c2) { node_free(if_node); return NULL; }
                Node *b2 = parse_block(p, stops_fish);
                if (!b2) { node_free(c2); node_free(if_node); return NULL; }
                Node *nested = node_new(N_IF);
                node_add_child(nested, c2);
                node_add_child(nested, b2);
                node_add_child(cur, nested);
                cur = nested;
                continue;
            }
        }
        break;
    }
    if (word_is(p, "else")) {
        p_advance(p);
        const char *stop_fi[] = { "fi", NULL };
        const char *stop_end[] = { "end", NULL };
        Node *belse = parse_block(p, bash_style ? stop_fi : stop_end);
        if (!belse) { node_free(if_node); return NULL; }
        node_add_child(cur, belse);
    }
    if (!expect_word(p, bash_style ? "fi" : "end")) { node_free(if_node); return NULL; }
    return if_node;
}

static Node *parse_while(Parser *p) {
    p_advance(p); /* while */
    Node *cond = parse_and_or_bg(p);
    if (!cond) return NULL;
    skip_separators(p);
    bool bash_style = false;
    if (word_is(p, "do")) { p_advance(p); bash_style = true; }
    const char *stop_done[] = { "done", NULL };
    const char *stop_end[] = { "end", NULL };
    Node *body = parse_block(p, bash_style ? stop_done : stop_end);
    if (!body) { node_free(cond); return NULL; }
    if (!expect_word(p, bash_style ? "done" : "end")) { node_free(cond); node_free(body); return NULL; }
    Node *n = node_new(N_WHILE);
    node_add_child(n, cond);
    node_add_child(n, body);
    return n;
}

static Node *parse_until(Parser *p) {
    p_advance(p); /* until */
    Node *cond = parse_and_or_bg(p);
    if (!cond) return NULL;
    skip_separators(p);
    bool bash_style = false;
    if (word_is(p, "do")) { p_advance(p); bash_style = true; }
    const char *stop_done[] = { "done", NULL };
    const char *stop_end[] = { "end", NULL };
    Node *body = parse_block(p, bash_style ? stop_done : stop_end);
    if (!body) { node_free(cond); return NULL; }
    if (!expect_word(p, bash_style ? "done" : "end")) { node_free(cond); node_free(body); return NULL; }
    Node *n = node_new(N_UNTIL);
    node_add_child(n, cond);
    node_add_child(n, body);
    return n;
}

static Node *parse_for(Parser *p) {
    p_advance(p); /* for */
    if (p->cur.type != TOK_WORD) { set_error(p, "syntax error: expected loop variable name"); return NULL; }
    Node *n = node_new(N_FOR);
    n->for_var = xstrdup(p->cur.text);
    p_advance(p);
    if (!word_is(p, "in")) { set_error(p, "syntax error: expected 'in' after for variable"); node_free(n); return NULL; }
    p_advance(p);
    while (p->cur.type == TOK_WORD) { sv_push_dup(&n->for_words, p->cur.text); p_advance(p); }
    skip_separators(p);
    bool bash_style = false;
    if (word_is(p, "do")) { p_advance(p); bash_style = true; }
    const char *stop_done[] = { "done", NULL };
    const char *stop_end[] = { "end", NULL };
    Node *body = parse_block(p, bash_style ? stop_done : stop_end);
    if (!body) { node_free(n); return NULL; }
    if (!expect_word(p, bash_style ? "done" : "end")) { node_free(n); node_free(body); return NULL; }
    node_add_child(n, body);
    return n;
}

static Node *parse_funcdef_kw(Parser *p) {
    p_advance(p); /* function */
    if (p->cur.type != TOK_WORD) { set_error(p, "syntax error: expected function name"); return NULL; }
    char *name = xstrdup(p->cur.text);
    size_t nl;
    if (ends_with_parens(name, &nl)) name[nl] = '\0';
    p_advance(p);
    skip_separators(p);
    bool brace_style = false;
    if (word_is(p, "{")) { p_advance(p); brace_style = true; }
    const char *stop_brace[] = { "}", NULL };
    const char *stop_end[] = { "end", NULL };
    Node *body = parse_block(p, brace_style ? stop_brace : stop_end);
    if (!body) { free(name); return NULL; }
    if (!expect_word(p, brace_style ? "}" : "end")) { free(name); node_free(body); return NULL; }
    Node *n = node_new(N_FUNCDEF);
    n->func_name = name;
    node_add_child(n, body);
    return n;
}

static Node *parse_funcdef_parens(Parser *p, size_t namelen) {
    char *name = xstrndup(p->cur.text, namelen);
    p_advance(p);
    skip_separators(p);
    if (!expect_word(p, "{")) { free(name); return NULL; }
    const char *stop_brace[] = { "}", NULL };
    Node *body = parse_block(p, stop_brace);
    if (!body) { free(name); return NULL; }
    if (!expect_word(p, "}")) { free(name); node_free(body); return NULL; }
    Node *n = node_new(N_FUNCDEF);
    n->func_name = name;
    node_add_child(n, body);
    return n;
}

/* bash: case SUBJECT in PAT1|PAT2) ... ;; *) ... ;; esac */
static Node *parse_case(Parser *p) {
    p_advance(p); /* case */
    if (p->cur.type != TOK_WORD) { set_error(p, "syntax error: expected case subject"); return NULL; }
    Node *n = node_new(N_CASE);
    n->case_subject = xstrdup(p->cur.text);
    p_advance(p);
    skip_separators(p);
    if (!expect_word(p, "in")) { node_free(n); return NULL; }
    skip_separators(p);

    while (!word_is(p, "esac")) {
        if (p->cur.type == TOK_EOF) { p->incomplete = true; node_free(n); return NULL; }

        Node *pats = node_new(N_CMD);
        if (p->cur.type != TOK_WORD) { set_error(p, "syntax error: expected a case pattern"); node_free(pats); node_free(n); return NULL; }
        sv_push_dup(&pats->argv, p->cur.text);
        p_advance(p);
        while (p->cur.type == TOK_PIPE) {
            p_advance(p);
            if (p->cur.type != TOK_WORD) { set_error(p, "syntax error: expected a case pattern"); node_free(pats); node_free(n); return NULL; }
            sv_push_dup(&pats->argv, p->cur.text);
            p_advance(p);
        }
        if (p->cur.type != TOK_RPAREN) { set_error(p, "syntax error: expected ')' after case pattern"); node_free(pats); node_free(n); return NULL; }
        p_advance(p);
        node_add_child(n, pats);
        skip_newlines(p);

        Node *body = node_new(N_BLOCK);
        for (;;) {
            while (p->cur.type == TOK_NEWLINE) p_advance(p);
            if (p->cur.type == TOK_EOF) { p->incomplete = true; node_free(body); node_free(n); return NULL; }
            if (word_is(p, "esac")) break;
            if (p->cur.type == TOK_SEMI) {
                Token *la = p_peek2(p);
                if (la->type == TOK_SEMI) { p_advance(p); p_advance(p); break; }
                p_advance(p);
                continue;
            }
            Node *stmt = parse_and_or_bg(p);
            if (!stmt) { node_free(body); node_free(n); return NULL; }
            node_add_child(body, stmt);
        }
        node_add_child(n, body);
        skip_separators(p);
    }
    p_advance(p); /* esac */
    return n;
}

/* fish: switch SUBJECT; case PAT1 PAT2; ...; case PAT3; ...; end */
static Node *parse_switch(Parser *p) {
    p_advance(p); /* switch */
    if (p->cur.type != TOK_WORD) { set_error(p, "syntax error: expected switch subject"); return NULL; }
    Node *n = node_new(N_CASE);
    n->case_subject = xstrdup(p->cur.text);
    p_advance(p);
    skip_separators(p);

    while (!word_is(p, "end")) {
        if (p->cur.type == TOK_EOF) { p->incomplete = true; node_free(n); return NULL; }
        if (!expect_word(p, "case")) { node_free(n); return NULL; }
        Node *pats = node_new(N_CMD);
        while (p->cur.type == TOK_WORD) { sv_push_dup(&pats->argv, p->cur.text); p_advance(p); }
        if (pats->argv.count == 0) { set_error(p, "syntax error: expected a pattern after 'case'"); node_free(pats); node_free(n); return NULL; }
        node_add_child(n, pats);
        skip_separators(p);
        const char *stops[] = { "case", "end", NULL };
        Node *body = parse_block(p, stops);
        if (!body) { node_free(n); return NULL; }
        node_add_child(n, body);
        skip_separators(p);
    }
    p_advance(p); /* end */
    return n;
}

/* bash: [[ expr ]] -- an extended test expression. Atoms are runs of WORD
 * tokens (an optional leading "!" negates the atom); "&&"/"||" between
 * atoms lex as ordinary TOK_AND/TOK_OR, so the same short-circuiting
 * N_AND/N_OR nodes used for shell-level "&&"/"||" are reused here.
 * Parenthesized sub-grouping is not supported (see README limitations). */
static Node *parse_cond_atom(Parser *p) {
    bool neg = false;
    while (word_is(p, "!")) { neg = !neg; p_advance(p); }
    if (p->cur.type != TOK_WORD || strcmp(p->cur.text, "]]") == 0) {
        set_error(p, "syntax error: expected a test expression after '[['");
        return NULL;
    }
    Node *n = node_new(N_CONDATOM);
    while (p->cur.type == TOK_WORD && strcmp(p->cur.text, "]]") != 0) {
        sv_push_dup(&n->argv, p->cur.text);
        p_advance(p);
    }
    n->negate = neg;
    return n;
}

static Node *parse_cond(Parser *p) {
    p_advance(p); /* [[ */
    Node *left = parse_cond_atom(p);
    if (!left) return NULL;
    while (p->cur.type == TOK_AND || p->cur.type == TOK_OR) {
        NodeType nt = (p->cur.type == TOK_AND) ? N_AND : N_OR;
        p_advance(p);
        skip_newlines(p);
        Node *right = parse_cond_atom(p);
        if (!right) { node_free(left); return NULL; }
        Node *comb = node_new(nt);
        node_add_child(comb, left);
        node_add_child(comb, right);
        left = comb;
    }
    if (!expect_word(p, "]]")) { node_free(left); return NULL; }
    return left;
}

/* ---------------- entry point ---------------- */

ParseStatus parse_program(const char *src, Node **out, char **err) {
    Parser p;
    memset(&p, 0, sizeof(p));
    lexer_init(&p.lx, src);
    lexer_next(&p.lx, &p.cur);

    skip_separators(&p);
    if (p.cur.type == TOK_EOF) {
        token_free(&p.cur);
        *out = NULL;
        return PARSE_EMPTY;
    }

    Node *root = node_new(N_SEQ);
    bool ok = parse_stmt_list(&p, root, NULL);

    token_free(&p.cur);
    if (p.la_valid) token_free(&p.la);

    if (!ok) {
        node_free(root);
        *out = NULL;
        if (p.incomplete) { free(p.errmsg); return PARSE_INCOMPLETE; }
        *err = p.errmsg ? p.errmsg : xstrdup("syntax error");
        return PARSE_ERROR;
    }
    free(p.errmsg);
    *out = root;
    return PARSE_OK;
}
