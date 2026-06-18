#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"

void lexer_init(Lexer *l, const char *source) {
    l->source  = source;
    l->current = source;
    l->line    = 1;
}

static Token make_token(Lexer *l, TokenType type, const char *start) {
    Token t;
    t.type   = type;
    t.start  = start;
    t.length = (int)(l->current - start);
    t.line   = l->line;
    t.number = 0;
    return t;
}

static Token error_token(Lexer *l, const char *msg) {
    Token t;
    t.type   = TOK_ERROR;
    t.start  = msg;
    t.length = (int)strlen(msg);
    t.line   = l->line;
    t.number = 0;
    return t;
}

static char peek(Lexer *l)       { return *l->current; }
static char peek_next(Lexer *l)  { return l->current[1]; }
static char advance(Lexer *l)    { return *l->current++; }
static int  at_end(Lexer *l)     { return *l->current == '\0'; }

static int match(Lexer *l, char expected) {
    if (at_end(l) || *l->current != expected) return 0;
    l->current++;
    return 1;
}

static void skip_whitespace(Lexer *l) {
    for (;;) {
        char c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(l);
        } else if (c == '#') {
            while (!at_end(l) && peek(l) != '\n') advance(l);
        } else {
            break;
        }
    }
}

static Token read_string(Lexer *l) {
    const char *start = l->current - 1; /* include opening quote */
    while (!at_end(l) && peek(l) != '"') {
        if (peek(l) == '\n') l->line++;
        advance(l);
    }
    if (at_end(l)) return error_token(l, "Unterminated string");
    advance(l); /* closing " */
    return make_token(l, TOK_STRING, start);
}

static Token read_number(Lexer *l, const char *start) {
    while (isdigit(peek(l))) advance(l);
    if (peek(l) == '.' && isdigit(peek_next(l))) {
        advance(l);
        while (isdigit(peek(l))) advance(l);
    }
    Token t  = make_token(l, TOK_NUMBER, start);
    t.number = strtod(start, NULL);
    return t;
}

static TokenType check_keyword(const char *start, int len,
                               const char *word, TokenType type) {
    if (len == (int)strlen(word) && memcmp(start, word, len) == 0)
        return type;
    return TOK_IDENT;
}

static TokenType ident_type(const char *start, int len) {
    switch (start[0]) {
        case 'e': return check_keyword(start, len, "else",   TOK_ELSE);
        case 'b': return check_keyword(start, len, "bool",   TOK_BOOL_TYPE);
        case 'f': {
            if (len > 1) {
                if (start[1] == 'n')     return check_keyword(start, len, "fn",    TOK_FN);
                if (start[1] == 'a')     return check_keyword(start, len, "false", TOK_FALSE);
                if (start[1] == 'l')     return check_keyword(start, len, "float", TOK_FLOAT_TYPE);
            }
            break;
        }
        case 'i': {
            if (len > 1 && start[1] == 'f')  return check_keyword(start, len, "if",  TOK_IF);
            if (len > 1 && start[1] == 'm') {  /* impl, implement */
                TokenType t = check_keyword(start, len, "impl", TOK_IMPL);
                if (t != TOK_IDENT) return t;
                return check_keyword(start, len, "implement", TOK_IMPLEMENT);
            }
            if (len > 1 && start[1] == 'n') {  /* int, interface */
                TokenType t = check_keyword(start, len, "int", TOK_INT_TYPE);
                if (t != TOK_IDENT) return t;
                return check_keyword(start, len, "interface", TOK_INTERFACE);
            }
            break;
        }
        case 'l': return check_keyword(start, len, "list",   TOK_LIST_TYPE);
        case 'm': return check_keyword(start, len, "map",    TOK_MAP_TYPE);
        case 's': {  /* string, struct */
            TokenType t = check_keyword(start, len, "string", TOK_STRING_TYPE);
            if (t != TOK_IDENT) return t;
            return check_keyword(start, len, "struct", TOK_STRUCT);
        }
        case 'n': return check_keyword(start, len, "null",   TOK_NULL);
        case 'r': return check_keyword(start, len, "return", TOK_RETURN);
        case 't': return check_keyword(start, len, "true",   TOK_TRUE);
        case 'w': return check_keyword(start, len, "while",  TOK_WHILE);
    }
    return TOK_IDENT;
}

static Token read_ident(Lexer *l, const char *start) {
    while (isalnum(peek(l)) || peek(l) == '_') advance(l);
    int len = (int)(l->current - start);
    return make_token(l, ident_type(start, len), start);
}

Token lexer_next(Lexer *l) {
    skip_whitespace(l);
    if (at_end(l)) return make_token(l, TOK_EOF, l->current);

    const char *start = l->current;
    char c = advance(l);

    if (isdigit(c))  return read_number(l, start);
    if (isalpha(c) || c == '_') return read_ident(l, start);

    switch (c) {
        case '\n': l->line++; return make_token(l, TOK_NEWLINE, start);
        case '(':  return make_token(l, TOK_LPAREN,  start);
        case ')':  return make_token(l, TOK_RPAREN,  start);
        case '{':  return make_token(l, TOK_LBRACE,   start);
        case '}':  return make_token(l, TOK_RBRACE,   start);
        case '[':  return make_token(l, TOK_LBRACKET, start);
        case ']':  return make_token(l, TOK_RBRACKET, start);
        case ',':  return make_token(l, TOK_COMMA,    start);
        case ':':  return make_token(l, TOK_COLON,    start);
        case '.':  return make_token(l, TOK_DOT,      start);
        case '+':  return make_token(l, TOK_PLUS,    start);
        case '-':  return make_token(l, TOK_MINUS,   start);
        case '*':  return make_token(l, TOK_STAR,    start);
        case '/':  return make_token(l, TOK_SLASH,   start);
        case '%':  return make_token(l, TOK_PERCENT, start);
        case '&':  return match(l,'&') ? make_token(l, TOK_AND, start)
                                       : error_token(l, "Expected '&&' (use '&&' for logical and)");
        case '|':  return match(l,'|') ? make_token(l, TOK_OR, start)
                                       : error_token(l, "Expected '||' (use '||' for logical or)");
        case '=':  return make_token(l, match(l,'=') ? TOK_EQEQ  : TOK_EQ,     start);
        case '!':  return make_token(l, match(l,'=') ? TOK_BANGEQ : TOK_NOT,    start);
        case '<':  return make_token(l, match(l,'=') ? TOK_LTEQ   : TOK_LT,     start);
        case '>':  return make_token(l, match(l,'=') ? TOK_GTEQ   : TOK_GT,     start);
        case '"':  return read_string(l);
    }

    return error_token(l, "Unexpected character");
}

const char *token_type_name(TokenType t) {
    switch (t) {
        case TOK_NUMBER:  return "NUMBER";
        case TOK_STRING:  return "STRING";
        case TOK_TRUE:    return "TRUE";
        case TOK_FALSE:   return "FALSE";
        case TOK_NULL:    return "NULL";
        case TOK_IDENT:   return "IDENT";
        case TOK_INT_TYPE:    return "INT_TYPE";
        case TOK_FLOAT_TYPE:  return "FLOAT_TYPE";
        case TOK_STRING_TYPE: return "STRING_TYPE";
        case TOK_BOOL_TYPE:   return "BOOL_TYPE";
        case TOK_LIST_TYPE:   return "LIST_TYPE";
        case TOK_MAP_TYPE:    return "MAP_TYPE";
        case TOK_FN:      return "FN";
        case TOK_RETURN:  return "RETURN";
        case TOK_IF:      return "IF";
        case TOK_ELSE:    return "ELSE";
        case TOK_WHILE:   return "WHILE";
        case TOK_STRUCT:    return "STRUCT";
        case TOK_INTERFACE: return "INTERFACE";
        case TOK_IMPL:      return "IMPL";
        case TOK_IMPLEMENT: return "IMPLEMENT";
        case TOK_AND:     return "AND";
        case TOK_OR:      return "OR";
        case TOK_NOT:     return "NOT";
        case TOK_PLUS:    return "PLUS";
        case TOK_MINUS:   return "MINUS";
        case TOK_STAR:    return "STAR";
        case TOK_SLASH:   return "SLASH";
        case TOK_PERCENT: return "PERCENT";
        case TOK_EQ:      return "EQ";
        case TOK_EQEQ:    return "EQEQ";
        case TOK_BANGEQ:  return "BANGEQ";
        case TOK_LT:      return "LT";
        case TOK_LTEQ:    return "LTEQ";
        case TOK_GT:      return "GT";
        case TOK_GTEQ:    return "GTEQ";
        case TOK_LPAREN:  return "LPAREN";
        case TOK_RPAREN:  return "RPAREN";
        case TOK_LBRACE:    return "LBRACE";
        case TOK_RBRACE:    return "RBRACE";
        case TOK_LBRACKET:  return "LBRACKET";
        case TOK_RBRACKET:  return "RBRACKET";
        case TOK_COMMA:     return "COMMA";
        case TOK_COLON:     return "COLON";
        case TOK_DOT:       return "DOT";
        case TOK_NEWLINE: return "NEWLINE";
        case TOK_EOF:     return "EOF";
        case TOK_ERROR:   return "ERROR";
    }
    return "UNKNOWN";
}
