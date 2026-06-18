#ifndef MORAY_INTERPRETER_H
#define MORAY_INTERPRETER_H

#include "ast.h"
#include "value.h"
#include "env.h"

#define MAX_STRUCT_TYPES   64
#define MAX_METHODS        32   /* per struct type */
#define MAX_IMPL_IFACES     8   /* interfaces a single struct may implement */
#define MAX_INTERFACES     64

/* A registered struct type: its definition plus the method table assembled from
   its (single) impl block and any implement blocks. */
typedef struct {
    Stmt       *def;                          /* STMT_STRUCT_DEF              */
    const char *method_names[MAX_METHODS];    /* point into the AST           */
    Stmt       *methods[MAX_METHODS];         /* STMT_FN_DEF                   */
    int         method_count;
    int         has_impl;                     /* at most one impl block        */
    const char *interfaces[MAX_IMPL_IFACES];  /* interface names implemented   */
    int         interface_count;
} StructType;

typedef struct {
    Env *globals;
    int  had_error;

    StructType structs[MAX_STRUCT_TYPES];
    int        struct_count;
    Stmt      *interfaces[MAX_INTERFACES];    /* STMT_INTERFACE_DEF            */
    int        interface_count;
} Interpreter;

void  interpreter_init(Interpreter *interp);
void  interpreter_free(Interpreter *interp);
void  interpreter_run(Interpreter *interp, Program *prog);

#endif
