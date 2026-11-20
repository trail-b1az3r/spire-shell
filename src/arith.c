#include "arith.h"
#include "common.h"
#include "vars.h"
#include <ctype.h>
#include <stdlib.h>

typedef enum {
    AT_NUM, AT_IDENT, AT_END,
    AT_PLUS, AT_MINUS, AT_STAR, AT_SLASH, AT_PERCENT,
    AT_LPAREN, AT_RPAREN,
    AT_NOT, AT_BNOT,
    AT_LT, AT_LE, AT_GT, AT_GE, AT_EQ, AT_NE,
    AT_AND, AT_OR, AT_BAND, AT_BOR, AT_BXOR, AT_SHL, AT_SHR,
    AT_QUESTION, AT_COLON,
    AT_ASSIGN, AT_PLUSEQ, AT_MINUSEQ, AT_STAREQ, AT_SLASHEQ, AT_PERCENTEQ,
    AT_INC, AT_DEC, AT_POW, AT_COMMA
} ArithTokType;

typedef struct {
    ArithTokType type;
    long num;
    char ident[128];
} ArithTok;

typedef struct {
    const char *s;
    size_t pos;
    ArithTok cur;
} ArithState;

static void lex_advance(ArithState *st) {
    const char *s = st->s;
    size_t i = st->pos;
    while (isspace((unsigned char)s[i])) i++;

    if (s[i] == '\0') { st->cur.type = AT_END; st->pos = i; return; }

    if (isdigit((unsigned char)s[i])) {
        char *end;
        long v = strtol(s + i, &end, 0);
        st->cur.type = AT_NUM;
        st->cur.num = v;
        st->pos = (size_t)(end - s);
        return;
    }
    if (isalpha((unsigned char)s[i]) || s[i] == '_' || s[i] == '$') {
        bool dollar = (s[i] == '$');
        if (dollar) i++;
        size_t start = i;
        while (isalnum((unsigned char)s[i]) || s[i] == '_') i++;
        size_t n = i - start;
        if (n >= sizeof(st->cur.ident)) n = sizeof(st->cur.ident) - 1;
        memcpy(st->cur.ident, s + start, n);
        st->cur.ident[n] = '\0';
        st->cur.type = AT_IDENT;
        st->pos = i;
        return;
    }

#define TWO(a, b, t) if (s[i] == (a) && s[i+1] == (b)) { st->cur.type = (t); st->pos = i + 2; return; }
    TWO('*', '*', AT_POW);
    TWO('+', '+', AT_INC);
    TWO('-', '-', AT_DEC);
    TWO('=', '=', AT_EQ);
    TWO('!', '=', AT_NE);
    TWO('<', '=', AT_LE);
    TWO('>', '=', AT_GE);
    TWO('&', '&', AT_AND);
    TWO('|', '|', AT_OR);
    TWO('<', '<', AT_SHL);
    TWO('>', '>', AT_SHR);
    TWO('+', '=', AT_PLUSEQ);
    TWO('-', '=', AT_MINUSEQ);
    TWO('*', '=', AT_STAREQ);
    TWO('/', '=', AT_SLASHEQ);
    TWO('%', '=', AT_PERCENTEQ);
#undef TWO

    ArithTokType t;
    switch (s[i]) {
        case '+': t = AT_PLUS; break;
        case '-': t = AT_MINUS; break;
        case '*': t = AT_STAR; break;
        case '/': t = AT_SLASH; break;
        case '%': t = AT_PERCENT; break;
        case '(': t = AT_LPAREN; break;
        case ')': t = AT_RPAREN; break;
        case '!': t = AT_NOT; break;
        case '~': t = AT_BNOT; break;
        case '<': t = AT_LT; break;
        case '>': t = AT_GT; break;
        case '&': t = AT_BAND; break;
        case '|': t = AT_BOR; break;
        case '^': t = AT_BXOR; break;
        case '?': t = AT_QUESTION; break;
        case ':': t = AT_COLON; break;
        case '=': t = AT_ASSIGN; break;
        case ',': t = AT_COMMA; break;
        default:  t = AT_END; break; /* unknown char: stop gracefully */
    }
    st->cur.type = t;
    st->pos = i + 1;
}

static long parse_assignment(ArithState *st);
static long parse_pow(ArithState *st);

static long var_as_long(const char *name) {
    const char *v = var_get(name);
    if (!v || !*v) return 0;
    char *end;
    long r = strtol(v, &end, 0);
    return (end == v) ? 0 : r;
}

/* >0 while parsing a side of && / || / ?: that must not take effect */
static int g_noeval = 0;

static void var_store_long(const char *name, long val) {
    if (g_noeval) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", val);
    var_set(name, buf, var_is_exported(name));
}

static long parse_primary(ArithState *st) {
    if (st->cur.type == AT_LPAREN) {
        lex_advance(st);
        long v = parse_assignment(st);
        if (st->cur.type == AT_RPAREN) lex_advance(st);
        return v;
    }
    if (st->cur.type == AT_NUM) {
        long v = st->cur.num;
        lex_advance(st);
        return v;
    }
    if (st->cur.type == AT_IDENT) {
        char name[128];
        snprintf(name, sizeof(name), "%s", st->cur.ident);
        lex_advance(st);
        if (st->cur.type == AT_INC) { long old = var_as_long(name); var_store_long(name, old + 1); lex_advance(st); return old; }
        if (st->cur.type == AT_DEC) { long old = var_as_long(name); var_store_long(name, old - 1); lex_advance(st); return old; }
        return var_as_long(name);
    }
    return 0;
}

static long parse_unary(ArithState *st) {
    if (st->cur.type == AT_MINUS) { lex_advance(st); return -parse_unary(st); }
    if (st->cur.type == AT_PLUS) { lex_advance(st); return parse_unary(st); }
    if (st->cur.type == AT_NOT) { lex_advance(st); return !parse_unary(st); }
    if (st->cur.type == AT_BNOT) { lex_advance(st); return ~parse_unary(st); }
    if (st->cur.type == AT_INC) {
        lex_advance(st);
        if (st->cur.type == AT_IDENT) {
            char name[128]; snprintf(name, sizeof(name), "%s", st->cur.ident);
            lex_advance(st);
            long v = var_as_long(name) + 1;
            var_store_long(name, v);
            return v;
        }
        return parse_unary(st);
    }
    if (st->cur.type == AT_DEC) {
        lex_advance(st);
        if (st->cur.type == AT_IDENT) {
            char name[128]; snprintf(name, sizeof(name), "%s", st->cur.ident);
            lex_advance(st);
            long v = var_as_long(name) - 1;
            var_store_long(name, v);
            return v;
        }
        return parse_unary(st);
    }
    return parse_pow(st);
}

static long ipow(long base, long exp) {
    if (exp < 0) return 0;
    long r = 1;
    while (exp > 0) { if (exp & 1) r *= base; base *= base; exp >>= 1; }
    return r;
}

static long parse_pow(ArithState *st) {
    long v = parse_primary(st);
    if (st->cur.type == AT_POW) { lex_advance(st); long e = parse_unary(st); v = ipow(v, e); }
    return v;
}

static long parse_mul(ArithState *st) {
    long v = parse_unary(st);
    for (;;) {
        if (st->cur.type == AT_STAR) { lex_advance(st); v *= parse_unary(st); }
        else if (st->cur.type == AT_SLASH) {
            lex_advance(st);
            long r = parse_unary(st);
            if (r == 0 && !g_noeval) { sh_err("division by zero\n"); v = 0; }
            else v = r ? v / r : 0;
        }
        else if (st->cur.type == AT_PERCENT) { lex_advance(st); long r = parse_unary(st); v = r ? v % r : 0; }
        else break;
    }
    return v;
}

static long parse_add(ArithState *st) {
    long v = parse_mul(st);
    for (;;) {
        if (st->cur.type == AT_PLUS) { lex_advance(st); v += parse_mul(st); }
        else if (st->cur.type == AT_MINUS) { lex_advance(st); v -= parse_mul(st); }
        else break;
    }
    return v;
}

static long parse_shift(ArithState *st) {
    long v = parse_add(st);
    for (;;) {
        if (st->cur.type == AT_SHL) { lex_advance(st); v <<= parse_add(st); }
        else if (st->cur.type == AT_SHR) { lex_advance(st); v >>= parse_add(st); }
        else break;
    }
    return v;
}

static long parse_rel(ArithState *st) {
    long v = parse_shift(st);
    for (;;) {
        if (st->cur.type == AT_LT) { lex_advance(st); v = v < parse_shift(st); }
        else if (st->cur.type == AT_LE) { lex_advance(st); v = v <= parse_shift(st); }
        else if (st->cur.type == AT_GT) { lex_advance(st); v = v > parse_shift(st); }
        else if (st->cur.type == AT_GE) { lex_advance(st); v = v >= parse_shift(st); }
        else break;
    }
    return v;
}

static long parse_eq(ArithState *st) {
    long v = parse_rel(st);
    for (;;) {
        if (st->cur.type == AT_EQ) { lex_advance(st); v = v == parse_rel(st); }
        else if (st->cur.type == AT_NE) { lex_advance(st); v = v != parse_rel(st); }
        else break;
    }
    return v;
}

static long parse_band(ArithState *st) { long v = parse_eq(st); while (st->cur.type == AT_BAND) { lex_advance(st); v = v & parse_eq(st); } return v; }
static long parse_bxor(ArithState *st) { long v = parse_band(st); while (st->cur.type == AT_BXOR) { lex_advance(st); v = v ^ parse_band(st); } return v; }
static long parse_bor(ArithState *st)  { long v = parse_bxor(st); while (st->cur.type == AT_BOR) { lex_advance(st); v = v | parse_bxor(st); } return v; }
static long parse_land(ArithState *st) {
    long v = parse_bor(st);
    while (st->cur.type == AT_AND) {
        lex_advance(st);
        if (!v) g_noeval++;
        long r = parse_bor(st);
        if (!v) g_noeval--;
        v = v && r;
    }
    return v;
}
static long parse_lor(ArithState *st) {
    long v = parse_land(st);
    while (st->cur.type == AT_OR) {
        lex_advance(st);
        if (v) g_noeval++;
        long r = parse_land(st);
        if (v) g_noeval--;
        v = v || r;
    }
    return v;
}

static long parse_ternary(ArithState *st) {
    long v = parse_lor(st);
    if (st->cur.type == AT_QUESTION) {
        lex_advance(st);
        if (!v) g_noeval++;
        long a = parse_assignment(st);
        if (!v) g_noeval--;
        long b = 0;
        if (st->cur.type == AT_COLON) {
            lex_advance(st);
            if (v) g_noeval++;
            b = parse_assignment(st);
            if (v) g_noeval--;
        }
        return v ? a : b;
    }
    return v;
}

static long parse_assignment(ArithState *st) {
    if (st->cur.type == AT_IDENT) {
        char name[128];
        snprintf(name, sizeof(name), "%s", st->cur.ident);
        ArithState save = *st;
        lex_advance(st);
        if (st->cur.type == AT_ASSIGN || st->cur.type == AT_PLUSEQ || st->cur.type == AT_MINUSEQ ||
            st->cur.type == AT_STAREQ || st->cur.type == AT_SLASHEQ || st->cur.type == AT_PERCENTEQ) {
            ArithTokType op = st->cur.type;
            lex_advance(st);
            long rhs = parse_assignment(st);
            long newv;
            switch (op) {
                case AT_PLUSEQ: newv = var_as_long(name) + rhs; break;
                case AT_MINUSEQ: newv = var_as_long(name) - rhs; break;
                case AT_STAREQ: newv = var_as_long(name) * rhs; break;
                case AT_SLASHEQ: newv = rhs ? var_as_long(name) / rhs : 0; break;
                case AT_PERCENTEQ: newv = rhs ? var_as_long(name) % rhs : 0; break;
                default: newv = rhs; break;
            }
            var_store_long(name, newv);
            return newv;
        }
        *st = save; /* not an assignment: rewind and parse normally */
    }
    return parse_ternary(st);
}

long arith_eval(const char *expr) {
    ArithState st;
    st.s = expr ? expr : "";
    st.pos = 0;
    g_noeval = 0;
    lex_advance(&st);
    long v = parse_assignment(&st);
    /* comma operator: evaluate left to right, result is the last */
    while (st.cur.type == AT_COMMA) { lex_advance(&st); v = parse_assignment(&st); }
    return v;
}
