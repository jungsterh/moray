#ifndef MORAY_LEXER_H
#define MORAY_LEXER_H

typedef enum {
    /* Literals */
    TOK_NUMBER, TOK_STRING, TOK_TRUE, TOK_FALSE, TOK_NULL,

    /* Identifiers & keywords */
    TOK_IDENT,
    TOK_INT_TYPE, TOK_FLOAT_TYPE, TOK_STRING_TYPE, TOK_BOOL_TYPE,
    TOK_LIST_TYPE, TOK_MAP_TYPE,
    TOK_FN, TOK_RETURN,
    TOK_IF, TOK_ELSE,
    TOK_WHILE,
    TOK_STRUCT, TOK_INTERFACE,
    TOK_IMPL, TOK_IMPLEMENT,

    /* Operators */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_AND,       /* && */
    TOK_OR,        /* || */
    TOK_NOT,       /* !  */
    TOK_EQ,        /* = */
    TOK_EQEQ,     /* == */
    TOK_BANGEQ,    /* != */
    TOK_LT, TOK_LTEQ,
    TOK_GT, TOK_GTEQ,

    /* Delimiters */
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,  /* [ ] */
    TOK_COMMA,
    TOK_COLON,                   /* : */
    TOK_DOT,                     /* . */

    /* Control */
    TOK_NEWLINE,
    TOK_EOF,
    TOK_ERROR,
} TokenType;

typedef struct {
    TokenType type;
    const char *start;  /* pointer into source */
    int length;
    int line;
    double number;      /* filled for TOK_NUMBER */
} Token;

typedef struct {
    const char *source;
    const char *current;
    int line;
} Lexer;

void  lexer_init(Lexer *l, const char *source);
Token lexer_next(Lexer *l);
const char *token_type_name(TokenType t);

#endif
