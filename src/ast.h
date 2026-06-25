#ifndef MORAY_AST_H
#define MORAY_AST_H

#include "vec.h"

/* ── Expression kinds ─────────────────────────────────────────────── */
typedef enum {
    EXPR_NUM,       /* 3.14          */
    EXPR_STR,       /* "hello"       */
    EXPR_BOOL,      /* true / false  */
    EXPR_NULL,      /* null          */
    EXPR_IDENT,     /* x             */
    EXPR_BINARY,    /* a + b         */
    EXPR_UNARY,     /* not x, -x     */
    EXPR_CALL,      /* add(1, 2)     */
    EXPR_CALLV,     /* (expr)(1, 2)  — call an arbitrary callee expression */
    EXPR_LIST,      /* [1, 2, 3]     */
    EXPR_MAP,       /* {"a": 1}      */
    EXPR_INDEX,     /* x[0], m["k"]  */
    EXPR_FIELD,     /* p.x           */
    EXPR_METHOD,    /* p.dist(1, 2)  */
} ExprKind;

/* ── Statement kinds ──────────────────────────────────────────────── */
typedef enum {
    STMT_VAR_DECL,  /* int x = 10    */
    STMT_ASSIGN,    /* x = 20        */
    STMT_IF,        /* if cond { }   */
    STMT_WHILE,     /* while cond {} */
    STMT_FOR,       /* for i;c;u {}  */
    STMT_FOR_IN,    /* for x in xs {}*/
    STMT_BREAK,     /* break         */
    STMT_CONTINUE,  /* continue      */
    STMT_RETURN,    /* return expr   */
    STMT_FN_DEF,    /* fn foo(a) {}  */
    STMT_EXPR,      /* bare expr     */
    STMT_BLOCK,     /* { stmts }     */
    STMT_STRUCT_DEF,    /* struct Point { int x }            */
    STMT_FIELD_ASSIGN,  /* p.x = 5                           */
    STMT_INDEX_ASSIGN,  /* list[0] = x, m[k] = v             */
    STMT_IMPL,          /* impl Point { fn ... }             */
    STMT_INTERFACE_DEF, /* interface Drawable { fn draw() }  */
    STMT_IMPLEMENT,     /* implement Drawable for Point {}   */
    STMT_IMPORT,        /* import "math.my" as math          */
} StmtKind;

/* ── Type annotation ──────────────────────────────────────────────── */
typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STRING,
    TYPE_BOOL,
    TYPE_VOID,      /* for functions that don't return a value; also untyped params like 'self' */
    TYPE_LIST,
    TYPE_MAP,
    TYPE_STRUCT,    /* a user struct type, named by identifier (not enforced at runtime) */
    TYPE_ANY,       /* matches any value — opts out of declaration-time checking */
} MorayType;

/*
 * A (possibly parameterized) type annotation on a variable declaration:
 * `int`, `list<int>`, `map<string, int>`, `map<string, list<int>>`, …
 *
 * The type arguments inside `<…>` live in p0/p1: a list parameterizes its
 * element type in p0; a map parameterizes its key in p0 and value in p1. Both
 * are NULL for an unparameterized type, in which case the declaration is
 * documentation-only as before. When parameters are present the interpreter
 * enforces them against the bound value at declaration time.
 */
typedef struct TypeAnn TypeAnn;

/* forward declare so Expr and Stmt can reference each other */
typedef struct Expr Expr;
typedef struct Stmt Stmt;

/* ── Vector element types ──────────────────────────────────────────── */
/*
 * Pointer aliases for the vector macros (element types must be a single
 * identifier, since the name is pasted into the generated struct name).
 * These, and the vector_define() calls, must come before any struct that
 * embeds them so the generated vec_* types are complete at point of use.
 */
typedef Expr *ExprPtr;
typedef Stmt *StmtPtr;

typedef struct {
    Expr *key;   /* always a string expression */
    Expr *value;
} MapEntry;

typedef struct {
    MorayType type;
    char *name;
} Param;

struct TypeAnn {
    MorayType base;
    TypeAnn  *p0;   /* list element, or map key  (NULL when unparameterized) */
    TypeAnn  *p1;   /* map value                 (NULL otherwise)            */
};

/* A call/method argument: positional (name == NULL) or named (name = "y"). */
typedef struct {
    char *name;   /* NULL if positional */
    Expr *value;
} Arg;

vector_define(ExprPtr)
vector_define(StmtPtr)
vector_define(Param)
vector_define(MapEntry)
vector_define(Arg)

/* ── Expressions ──────────────────────────────────────────────────── */
struct Expr {
    ExprKind kind;
    int line;

    union {
        double num;                     /* EXPR_NUM              */

        struct {                        /* EXPR_STR              */
            char *ptr;                  /* owned, escape-decoded; freed by expr_free */
            int len;
        } str;

        int bool_val;                   /* EXPR_BOOL: 1 or 0     */

        char *ident;                    /* EXPR_IDENT            */

        struct {                        /* EXPR_BINARY           */
            Expr *left;
            char op[3];                 /* "+", "!=", "<=" etc.  */
            Expr *right;
        } binary;

        struct {                        /* EXPR_UNARY            */
            char op[4];                 /* "-" or "not"          */
            Expr *right;
        } unary;

        struct {                        /* EXPR_CALL             */
            char *name;
            vector(Arg) args;           /* positional and/or named */
        } call;

        struct {                        /* EXPR_CALLV            */
            Expr *callee;               /* evaluates to a function value */
            vector(Arg) args;
        } callv;

        vector(ExprPtr) list;           /* EXPR_LIST             */

        vector(MapEntry) map;           /* EXPR_MAP              */

        struct {                        /* EXPR_INDEX            */
            Expr *object;               /* the list or map       */
            Expr *index;                /* the key or position   */
        } index;

        struct {                        /* EXPR_FIELD            */
            Expr *object;
            char *name;
        } field;

        struct {                        /* EXPR_METHOD           */
            Expr *object;
            char *name;
            vector(Arg) args;
        } method;
    };
};

/* ── Statements ───────────────────────────────────────────────────── */

struct Stmt {
    StmtKind kind;
    int line;

    union {
        struct {                        /* STMT_VAR_DECL         */
            MorayType type;
            TypeAnn  *ann;              /* full annotation; NULL for struct-typed decls */
            char *name;
            Expr *init;
        } var_decl;

        struct {                        /* STMT_ASSIGN           */
            char *name;
            char  op[3];                /* "" plain; "+","-","*","/","%" compound */
            Expr *value;
        } assign;

        struct {                        /* STMT_IF               */
            Expr *condition;
            Stmt *then_block;
            Stmt *else_block;           /* NULL if no else       */
        } if_stmt;

        struct {                        /* STMT_WHILE            */
            Expr *condition;
            Stmt *body;
        } while_stmt;

        struct {                        /* STMT_FOR              */
            Stmt *init;                 /* NULL if omitted; decl or assignment */
            Expr *condition;            /* NULL means always true              */
            Stmt *update;               /* NULL if omitted; runs after each pass */
            Stmt *body;
        } for_stmt;

        struct {                        /* STMT_FOR_IN           */
            char *var;                  /* loop variable, bound to each element */
            Expr *iterable;             /* must evaluate to a list              */
            Stmt *body;
        } for_in;

        struct {                        /* STMT_RETURN           */
            Expr *value;                /* NULL for bare return  */
        } ret;

        struct {                        /* STMT_FN_DEF           */
            char *name;
            vector(Param) params;
            Stmt *body;
        } fn_def;

        Expr *expr;                     /* STMT_EXPR             */

        vector(StmtPtr) block;          /* STMT_BLOCK            */

        struct {                        /* STMT_STRUCT_DEF       */
            char *name;
            vector(Param) fields;       /* type + name pairs     */
        } struct_def;

        struct {                        /* STMT_FIELD_ASSIGN     */
            Expr *object;
            char *name;
            char  op[3];                /* "" plain; "+","-","*","/","%" compound */
            Expr *value;
        } field_assign;

        struct {                        /* STMT_INDEX_ASSIGN     */
            Expr *object;               /* the list or map       */
            Expr *index;                /* the position or key   */
            char  op[3];                /* "" plain; "+","-","*","/","%" compound */
            Expr *value;
        } index_assign;

        struct {                        /* STMT_IMPL             */
            char *struct_name;
            vector(StmtPtr) methods;    /* each a STMT_FN_DEF     */
        } impl;

        struct {                        /* STMT_INTERFACE_DEF    */
            char *name;
            vector(StmtPtr) sigs;       /* STMT_FN_DEF, body NULL */
        } interface_def;

        struct {                        /* STMT_IMPLEMENT        */
            char *interface_name;
            char *struct_name;
            vector(StmtPtr) methods;    /* each a STMT_FN_DEF     */
        } implement;

        struct {                        /* STMT_IMPORT           */
            char *path;                 /* module file path      */
            char *alias;                /* namespace bound in scope */
        } import;
    };
};

/* ── Program ──────────────────────────────────────────────────────── */

typedef struct {
    vector(StmtPtr) stmts;
} Program;

/* Helpers */
TypeAnn *typeann_alloc(MorayType base);
void     typeann_free(TypeAnn *t);
Expr *expr_alloc(ExprKind kind, int line);
Stmt *stmt_alloc(StmtKind kind, int line);
void  expr_free(Expr *e);
void  stmt_free(Stmt *s);
void  program_free(Program *p);

#endif
