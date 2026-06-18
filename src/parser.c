#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

/* ── Helpers ──────────────────────────────────────────────────────── */

static void error(Parser *p, const char *msg) {
    if (p->had_error) return;
    fprintf(stderr, "[line %d] Parse error: %s (got '%.*s')\n",
            p->current.line, msg,
            p->current.length, p->current.start);
    p->had_error = 1;
}

/* move to next token, skipping blank lines */
static void advance(Parser *p) {
    p->previous = p->current;
    if (p->has_pending) {
        p->current     = p->pending;
        p->has_pending = 0;
        return;
    }
    do {
        p->current = lexer_next(p->lexer);
    } while (p->current.type == TOK_NEWLINE);
}

static int check(Parser *p, TokenType t) {
    return p->current.type == t;
}

static int match(Parser *p, TokenType t) {
    if (!check(p, t)) return 0;
    advance(p);
    return 1;
}

static void expect(Parser *p, TokenType t, const char *msg) {
    if (!match(p, t)) error(p, msg);
}

/* copy a token's text into a heap-allocated null-terminated string */
static char *token_to_str(Token t) {
    char *s = malloc(t.length + 1);
    memcpy(s, t.start, t.length);
    s[t.length] = '\0';
    return s;
}

static MorayType parse_type(Parser *p) {
    if (match(p, TOK_INT_TYPE))    return TYPE_INT;
    if (match(p, TOK_FLOAT_TYPE))  return TYPE_FLOAT;
    if (match(p, TOK_STRING_TYPE)) return TYPE_STRING;
    if (match(p, TOK_BOOL_TYPE))   return TYPE_BOOL;
    if (match(p, TOK_LIST_TYPE))   return TYPE_LIST;
    if (match(p, TOK_MAP_TYPE))    return TYPE_MAP;
    error(p, "Expected type (int, float, string, bool, list, map)");
    return TYPE_INT;
}

static int is_type_token(Parser *p) {
    return check(p, TOK_INT_TYPE)    || check(p, TOK_FLOAT_TYPE) ||
           check(p, TOK_STRING_TYPE) || check(p, TOK_BOOL_TYPE)  ||
           check(p, TOK_LIST_TYPE)   || check(p, TOK_MAP_TYPE);
}

/* A parameter is normally `type name`, but may be an untyped name (e.g. the
   `self` receiver in a method, where the type is implied by the impl target). */
static Param parse_param(Parser *p) {
    Param param;
    param.type = is_type_token(p) ? parse_type(p) : TYPE_VOID;
    expect(p, TOK_IDENT, "Expected parameter name");
    param.name = token_to_str(p->previous);
    return param;
}

/* ── Expression parsing ───────────────────────────────────────────── */
/*
 * Precedence ladder (lowest → highest).
 * Each level calls the next one first, so higher-precedence
 * operators bind tighter and sit deeper in the tree.
 *
 *   or
 *   and
 *   equality   (== !=)
 *   comparison (< <= > >=)
 *   term       (+ -)
 *   factor     (* / %)
 *   unary      (- not)
 *   primary    (literals, identifiers, grouped expressions)
 */

static Expr *parse_expr(Parser *p);
static void  parse_args(Parser *p, vector(Arg) *out);  /* fills until and past ')' */

static Expr *parse_primary(Parser *p) {
    int line = p->current.line;

    if (match(p, TOK_NUMBER)) {
        Expr *e = expr_alloc(EXPR_NUM, line);
        e->num = p->previous.number;
        return e;
    }
    if (match(p, TOK_STRING)) {
        Expr *e = expr_alloc(EXPR_STR, line);
        /* strip surrounding quotes */
        e->str.ptr = p->previous.start + 1;
        e->str.len = p->previous.length - 2;
        return e;
    }
    if (match(p, TOK_TRUE)) {
        Expr *e = expr_alloc(EXPR_BOOL, line);
        e->bool_val = 1;
        return e;
    }
    if (match(p, TOK_FALSE)) {
        Expr *e = expr_alloc(EXPR_BOOL, line);
        e->bool_val = 0;
        return e;
    }
    if (match(p, TOK_NULL)) {
        return expr_alloc(EXPR_NULL, line);
    }
    if (match(p, TOK_IDENT)) {
        char *name = token_to_str(p->previous);

        /* function call or struct construction: name(...) */
        if (match(p, TOK_LPAREN)) {
            Expr *e = expr_alloc(EXPR_CALL, line);
            e->call.name = name;
            e->call.args = (vector(Arg))vector_new();
            parse_args(p, &e->call.args);
            return e;
        }

        /* plain identifier */
        Expr *e = expr_alloc(EXPR_IDENT, line);
        e->ident = name;
        return e;
    }
    if (match(p, TOK_LPAREN)) {
        Expr *e = parse_expr(p);
        expect(p, TOK_RPAREN, "Expected ')' after expression");
        return e;
    }

    /* list literal: [expr, expr, ...] */
    if (match(p, TOK_LBRACKET)) {
        Expr *e  = expr_alloc(EXPR_LIST, line);
        e->list  = (vector(ExprPtr))vector_new();
        while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)) {
            Expr *item = parse_expr(p);
            vector_push(&e->list, item);
            if (!match(p, TOK_COMMA)) break;
        }
        expect(p, TOK_RBRACKET, "Expected ']' after list elements");
        return e;
    }

    /* map literal: {"key": expr, ...} */
    if (match(p, TOK_LBRACE)) {
        Expr *e = expr_alloc(EXPR_MAP, line);
        e->map  = (vector(MapEntry))vector_new();
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            MapEntry entry;
            entry.key = parse_expr(p);
            expect(p, TOK_COLON, "Expected ':' after map key");
            entry.value = parse_expr(p);
            vector_push(&e->map, entry);
            if (!match(p, TOK_COMMA)) break;
        }
        expect(p, TOK_RBRACE, "Expected '}' after map entries");
        return e;
    }

    error(p, "Expected an expression");
    return NULL;
}

/*
 * Parse a call's argument list, assuming '(' has already been consumed.
 * Arguments are positional, optionally followed by named ones (`name = expr`),
 * Python-style: a positional argument may not follow a named one. Consumes the
 * closing ')'.
 */
static void parse_args(Parser *p, vector(Arg) *out) {
    int seen_named = 0;
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        Arg arg = { NULL, NULL };

        /* a named argument looks like `IDENT = expr` (single '='). Peek one
           token past the identifier to tell it apart from a positional
           expression that merely starts with an identifier. */
        if (check(p, TOK_IDENT)) {
            Token ident = p->current;
            advance(p);                 /* current = lookahead */
            if (match(p, TOK_EQ)) {
                arg.name  = token_to_str(ident);
                arg.value = parse_expr(p);
                seen_named = 1;
            } else {
                /* positional: push the lookahead back, restore the identifier */
                p->pending     = p->current;
                p->has_pending = 1;
                p->current     = ident;
                if (seen_named) error(p, "Positional argument after named argument");
                arg.value = parse_expr(p);
            }
        } else {
            if (seen_named) error(p, "Positional argument after named argument");
            arg.value = parse_expr(p);
        }

        vector_push(out, arg);
        if (!match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RPAREN, "Expected ')' after arguments");
}

/* wrap an expression with postfix operations: indexing expr[key], field access
   expr.name, and method calls expr.name(args) */
static Expr *parse_postfix(Parser *p, Expr *object) {
    for (;;) {
        if (match(p, TOK_LBRACKET)) {
            int line    = p->previous.line;
            Expr *e     = expr_alloc(EXPR_INDEX, line);
            e->index.object = object;
            e->index.index  = parse_expr(p);
            expect(p, TOK_RBRACKET, "Expected ']' after index");
            object = e;
        } else if (match(p, TOK_DOT)) {
            int line = p->previous.line;
            expect(p, TOK_IDENT, "Expected field or method name after '.'");
            char *name = token_to_str(p->previous);
            if (match(p, TOK_LPAREN)) {           /* method call */
                Expr *e = expr_alloc(EXPR_METHOD, line);
                e->method.object = object;
                e->method.name   = name;
                e->method.args   = (vector(Arg))vector_new();
                parse_args(p, &e->method.args);
                object = e;
            } else {                              /* field access */
                Expr *e = expr_alloc(EXPR_FIELD, line);
                e->field.object = object;
                e->field.name   = name;
                object = e;
            }
        } else {
            break;
        }
    }
    return object;
}

static Expr *parse_unary(Parser *p) {
    int line = p->current.line;
    if (match(p, TOK_MINUS)) {
        Expr *e = expr_alloc(EXPR_UNARY, line);
        strcpy(e->unary.op, "-");
        e->unary.right = parse_unary(p);
        return e;
    }
    if (match(p, TOK_NOT)) {
        Expr *e = expr_alloc(EXPR_UNARY, line);
        strcpy(e->unary.op, "not");
        e->unary.right = parse_unary(p);
        return e;
    }
    return parse_postfix(p, parse_primary(p));
}

static Expr *parse_factor(Parser *p) {
    Expr *left = parse_unary(p);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
        int line = p->current.line;
        char op[3];
        strncpy(op, p->current.start, p->current.length);
        op[p->current.length] = '\0';
        advance(p);
        Expr *e      = expr_alloc(EXPR_BINARY, line);
        strcpy(e->binary.op, op);
        e->binary.left  = left;
        e->binary.right = parse_unary(p);
        left = e;
    }
    return left;
}

static Expr *parse_term(Parser *p) {
    Expr *left = parse_factor(p);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        int line = p->current.line;
        char op[3];
        strncpy(op, p->current.start, p->current.length);
        op[p->current.length] = '\0';
        advance(p);
        Expr *e      = expr_alloc(EXPR_BINARY, line);
        strcpy(e->binary.op, op);
        e->binary.left  = left;
        e->binary.right = parse_factor(p);
        left = e;
    }
    return left;
}

static Expr *parse_comparison(Parser *p) {
    Expr *left = parse_term(p);
    while (check(p, TOK_LT)   || check(p, TOK_LTEQ) ||
           check(p, TOK_GT)   || check(p, TOK_GTEQ)) {
        int line = p->current.line;
        char op[3];
        strncpy(op, p->current.start, p->current.length);
        op[p->current.length] = '\0';
        advance(p);
        Expr *e      = expr_alloc(EXPR_BINARY, line);
        strcpy(e->binary.op, op);
        e->binary.left  = left;
        e->binary.right = parse_term(p);
        left = e;
    }
    return left;
}

static Expr *parse_equality(Parser *p) {
    Expr *left = parse_comparison(p);
    while (check(p, TOK_EQEQ) || check(p, TOK_BANGEQ)) {
        int line = p->current.line;
        char op[3];
        strncpy(op, p->current.start, p->current.length);
        op[p->current.length] = '\0';
        advance(p);
        Expr *e      = expr_alloc(EXPR_BINARY, line);
        strcpy(e->binary.op, op);
        e->binary.left  = left;
        e->binary.right = parse_comparison(p);
        left = e;
    }
    return left;
}

static Expr *parse_and(Parser *p) {
    Expr *left = parse_equality(p);
    while (match(p, TOK_AND)) {
        int line = p->previous.line;
        Expr *e  = expr_alloc(EXPR_BINARY, line);
        strcpy(e->binary.op, "&&");
        e->binary.left  = left;
        e->binary.right = parse_equality(p);
        left = e;
    }
    return left;
}

static Expr *parse_expr(Parser *p) {
    Expr *left = parse_and(p);
    while (match(p, TOK_OR)) {
        int line = p->previous.line;
        Expr *e  = expr_alloc(EXPR_BINARY, line);
        strcpy(e->binary.op, "||");
        e->binary.left  = left;
        e->binary.right = parse_and(p);
        left = e;
    }
    return left;
}

/* ── Statement parsing ────────────────────────────────────────────── */

static Stmt *parse_stmt(Parser *p);

static Stmt *parse_block(Parser *p) {
    int line = p->previous.line;
    Stmt *s  = stmt_alloc(STMT_BLOCK, line);
    s->block = (vector(StmtPtr))vector_new();
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Stmt *inner = parse_stmt(p);
        if (inner) vector_push(&s->block, inner);
    }
    expect(p, TOK_RBRACE, "Expected '}' to close block");
    return s;
}

static Stmt *parse_fn_def(Parser *p) {
    int line = p->current.line;
    expect(p, TOK_IDENT, "Expected function name after 'fn'");
    char *name = token_to_str(p->previous);

    expect(p, TOK_LPAREN, "Expected '(' after function name");

    Stmt *s     = stmt_alloc(STMT_FN_DEF, line);
    s->fn_def.name   = name;
    s->fn_def.params = (vector(Param))vector_new();

    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        vector_push(&s->fn_def.params, parse_param(p));
        if (!match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RPAREN, "Expected ')' after parameters");
    expect(p, TOK_LBRACE, "Expected '{' before function body");
    s->fn_def.body = parse_block(p);
    return s;
}

/* A method signature inside an interface: `fn name(params)` with no body. */
static Stmt *parse_fn_sig(Parser *p) {
    int line = p->current.line;
    expect(p, TOK_IDENT, "Expected method name");
    char *name = token_to_str(p->previous);
    expect(p, TOK_LPAREN, "Expected '(' after method name");

    Stmt *s = stmt_alloc(STMT_FN_DEF, line);
    s->fn_def.name   = name;
    s->fn_def.params = (vector(Param))vector_new();
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        vector_push(&s->fn_def.params, parse_param(p));
        if (!match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RPAREN, "Expected ')' after parameters");
    s->fn_def.body = NULL;   /* signature only — no body */
    return s;
}

static Stmt *parse_struct_def(Parser *p) {
    int line = p->current.line;
    expect(p, TOK_IDENT, "Expected struct name after 'struct'");
    char *name = token_to_str(p->previous);
    expect(p, TOK_LBRACE, "Expected '{' after struct name");

    Stmt *s = stmt_alloc(STMT_STRUCT_DEF, line);
    s->struct_def.name   = name;
    s->struct_def.fields = (vector(Param))vector_new();
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Param field;
        field.type = parse_type(p);
        expect(p, TOK_IDENT, "Expected field name");
        field.name = token_to_str(p->previous);
        vector_push(&s->struct_def.fields, field);
    }
    expect(p, TOK_RBRACE, "Expected '}' to close struct");
    return s;
}

static Stmt *parse_impl(Parser *p) {
    int line = p->current.line;
    expect(p, TOK_IDENT, "Expected struct name after 'impl'");
    char *name = token_to_str(p->previous);
    expect(p, TOK_LBRACE, "Expected '{' after impl target");

    Stmt *s = stmt_alloc(STMT_IMPL, line);
    s->impl.struct_name = name;
    s->impl.methods     = (vector(StmtPtr))vector_new();
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        expect(p, TOK_FN, "Only method definitions are allowed in an impl block");
        vector_push(&s->impl.methods, parse_fn_def(p));
    }
    expect(p, TOK_RBRACE, "Expected '}' to close impl block");
    return s;
}

static Stmt *parse_interface_def(Parser *p) {
    int line = p->current.line;
    expect(p, TOK_IDENT, "Expected interface name after 'interface'");
    char *name = token_to_str(p->previous);
    expect(p, TOK_LBRACE, "Expected '{' after interface name");

    Stmt *s = stmt_alloc(STMT_INTERFACE_DEF, line);
    s->interface_def.name = name;
    s->interface_def.sigs = (vector(StmtPtr))vector_new();
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        expect(p, TOK_FN, "Only method signatures are allowed in an interface");
        vector_push(&s->interface_def.sigs, parse_fn_sig(p));
    }
    expect(p, TOK_RBRACE, "Expected '}' to close interface");
    return s;
}

/* interface conformance, written subject-first: `Point implement Drawable {}`.
   The struct name has already been consumed (it is the leading identifier of the
   statement); `implement` is the current token. */
static Stmt *parse_implement(Parser *p, Token struct_name, int line) {
    expect(p, TOK_IMPLEMENT, "Expected 'implement'");
    expect(p, TOK_IDENT, "Expected interface name after 'implement'");
    char *iface = token_to_str(p->previous);
    expect(p, TOK_LBRACE, "Expected '{' after interface name");

    Stmt *s = stmt_alloc(STMT_IMPLEMENT, line);
    s->implement.struct_name    = token_to_str(struct_name);
    s->implement.interface_name = iface;
    s->implement.methods        = (vector(StmtPtr))vector_new();
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        expect(p, TOK_FN, "Only method definitions are allowed in an implement block");
        vector_push(&s->implement.methods, parse_fn_def(p));
    }
    expect(p, TOK_RBRACE, "Expected '}' to close implement block");
    return s;
}

static Stmt *parse_stmt(Parser *p) {
    int line = p->current.line;

    /* variable declaration: int x = expr */
    if (check(p, TOK_INT_TYPE)    || check(p, TOK_FLOAT_TYPE) ||
        check(p, TOK_STRING_TYPE) || check(p, TOK_BOOL_TYPE)  ||
        check(p, TOK_LIST_TYPE)   || check(p, TOK_MAP_TYPE)) {
        MorayType type = parse_type(p);
        expect(p, TOK_IDENT, "Expected variable name after type");
        char *name = token_to_str(p->previous);
        expect(p, TOK_EQ, "Expected '=' after variable name");
        Expr *init = parse_expr(p);
        Stmt *s    = stmt_alloc(STMT_VAR_DECL, line);
        s->var_decl.type = type;
        s->var_decl.name = name;
        s->var_decl.init = init;
        return s;
    }

    if (match(p, TOK_FN))         return parse_fn_def(p);
    if (match(p, TOK_STRUCT))     return parse_struct_def(p);
    if (match(p, TOK_INTERFACE))  return parse_interface_def(p);
    if (match(p, TOK_IMPL))       return parse_impl(p);

    if (match(p, TOK_RETURN)) {
        Stmt *s    = stmt_alloc(STMT_RETURN, line);
        s->ret.value = check(p, TOK_RBRACE) ? NULL : parse_expr(p);
        return s;
    }

    if (match(p, TOK_IF)) {
        Expr *cond = parse_expr(p);
        expect(p, TOK_LBRACE, "Expected '{' after if condition");
        Stmt *then_block = parse_block(p);
        Stmt *else_block = NULL;
        if (match(p, TOK_ELSE)) {
            expect(p, TOK_LBRACE, "Expected '{' after else");
            else_block = parse_block(p);
        }
        Stmt *s = stmt_alloc(STMT_IF, line);
        s->if_stmt.condition  = cond;
        s->if_stmt.then_block = then_block;
        s->if_stmt.else_block = else_block;
        return s;
    }

    if (match(p, TOK_WHILE)) {
        Expr *cond = parse_expr(p);
        expect(p, TOK_LBRACE, "Expected '{' after while condition");
        Stmt *s = stmt_alloc(STMT_WHILE, line);
        s->while_stmt.condition = cond;
        s->while_stmt.body      = parse_block(p);
        return s;
    }

    /* A statement that begins with an identifier is one of:
         TypeName var = expr   (struct-typed declaration: IDENT IDENT ...)
         x = expr              (assignment)
         p.x = expr            (field assignment)
         <expression>          (e.g. a call like print(...))
       We peek one token past the leading identifier to spot the declaration
       form; everything else is handled by parsing a full expression and then
       checking for a trailing '='. */
    if (check(p, TOK_IDENT)) {
        Token first = p->current;
        advance(p);                     /* current = lookahead */
        if (check(p, TOK_IMPLEMENT)) {
            /* interface conformance: `Point implement Drawable { ... }` */
            return parse_implement(p, first, line);
        }
        if (check(p, TOK_IDENT)) {
            /* user struct-typed declaration: `Point p = expr` */
            char *var_name = token_to_str(p->current);
            advance(p);
            expect(p, TOK_EQ, "Expected '=' after variable name");
            Stmt *s = stmt_alloc(STMT_VAR_DECL, line);
            s->var_decl.type = TYPE_STRUCT;
            s->var_decl.name = var_name;
            s->var_decl.init = parse_expr(p);
            return s;
        }
        /* not a declaration: restore the leading identifier for parse_expr */
        p->pending     = p->current;
        p->has_pending = 1;
        p->current     = first;
    }

    /* expression statement, or an assignment whose target is the expression */
    Expr *target = parse_expr(p);
    if (match(p, TOK_EQ)) {
        Expr *value = parse_expr(p);
        if (target && target->kind == EXPR_IDENT) {
            Stmt *s = stmt_alloc(STMT_ASSIGN, line);
            s->assign.name  = target->ident;   /* steal the heap string */
            s->assign.value = value;
            free(target);                      /* free the shell, not its name */
            return s;
        }
        if (target && target->kind == EXPR_FIELD) {
            Stmt *s = stmt_alloc(STMT_FIELD_ASSIGN, line);
            s->field_assign.object = target->field.object;  /* steal */
            s->field_assign.name   = target->field.name;    /* steal */
            s->field_assign.value  = value;
            free(target);
            return s;
        }
        error(p, "Invalid assignment target");
        expr_free(target);
        expr_free(value);
        return NULL;
    }

    Stmt *s  = stmt_alloc(STMT_EXPR, line);
    s->expr  = target;
    return s;
}

/* ── Entry point ──────────────────────────────────────────────────── */

void parser_init(Parser *p, Lexer *l) {
    p->lexer       = l;
    p->had_error   = 0;
    p->has_pending = 0;
    advance(p); /* prime the first token */
}

Program parser_parse(Parser *p) {
    Program prog;
    prog.stmts = (vector(StmtPtr))vector_new();
    while (!check(p, TOK_EOF) && !p->had_error) {
        Stmt *s = parse_stmt(p);
        if (s) vector_push(&prog.stmts, s);
    }
    return prog;
}
