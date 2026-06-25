#include <stdlib.h>
#include <string.h>
#include "ast.h"

TypeAnn *typeann_alloc(MorayType base) {
    TypeAnn *t = calloc(1, sizeof(TypeAnn));
    t->base = base;
    return t;
}

void typeann_free(TypeAnn *t) {
    if (!t) return;
    typeann_free(t->p0);
    typeann_free(t->p1);
    free(t);
}

Expr *expr_alloc(ExprKind kind, int line) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    return e;
}

Stmt *stmt_alloc(StmtKind kind, int line) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    return s;
}

void expr_free(Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_STR:    free(e->str.ptr); break;
        case EXPR_IDENT:  free(e->ident); break;
        case EXPR_BINARY: expr_free(e->binary.left); expr_free(e->binary.right); break;
        case EXPR_UNARY:  expr_free(e->unary.right); break;
        case EXPR_CALL:
            free(e->call.name);
            for (int i = 0; i < e->call.args.len; i++) {
                free(e->call.args.data[i].name);
                expr_free(e->call.args.data[i].value);
            }
            vector_free(&e->call.args);
            break;
        case EXPR_CALLV:
            expr_free(e->callv.callee);
            for (int i = 0; i < e->callv.args.len; i++) {
                free(e->callv.args.data[i].name);
                expr_free(e->callv.args.data[i].value);
            }
            vector_free(&e->callv.args);
            break;
        case EXPR_INDEX:
            expr_free(e->index.object);
            expr_free(e->index.index);
            break;
        case EXPR_FIELD:
            expr_free(e->field.object);
            free(e->field.name);
            break;
        case EXPR_METHOD:
            expr_free(e->method.object);
            free(e->method.name);
            for (int i = 0; i < e->method.args.len; i++) {
                free(e->method.args.data[i].name);
                expr_free(e->method.args.data[i].value);
            }
            vector_free(&e->method.args);
            break;
        case EXPR_LIST:
            for (int i = 0; i < e->list.len; i++)
                expr_free(e->list.data[i]);
            vector_free(&e->list);
            break;
        case EXPR_MAP:
            for (int i = 0; i < e->map.len; i++) {
                expr_free(e->map.data[i].key);
                expr_free(e->map.data[i].value);
            }
            vector_free(&e->map);
            break;
        default: break;
    }
    free(e);
}

void stmt_free(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_VAR_DECL:
            free(s->var_decl.name);
            typeann_free(s->var_decl.ann);
            expr_free(s->var_decl.init);
            break;
        case STMT_ASSIGN:
            free(s->assign.name);
            expr_free(s->assign.value);
            break;
        case STMT_IF:
            expr_free(s->if_stmt.condition);
            stmt_free(s->if_stmt.then_block);
            stmt_free(s->if_stmt.else_block);
            break;
        case STMT_WHILE:
            expr_free(s->while_stmt.condition);
            stmt_free(s->while_stmt.body);
            break;
        case STMT_FOR:
            stmt_free(s->for_stmt.init);
            expr_free(s->for_stmt.condition);
            stmt_free(s->for_stmt.update);
            stmt_free(s->for_stmt.body);
            break;
        case STMT_FOR_IN:
            free(s->for_in.var);
            expr_free(s->for_in.iterable);
            stmt_free(s->for_in.body);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;
        case STMT_RETURN: expr_free(s->ret.value); break;
        case STMT_FN_DEF:
            free(s->fn_def.name);
            for (int i = 0; i < s->fn_def.params.len; i++)
                free(s->fn_def.params.data[i].name);
            vector_free(&s->fn_def.params);
            stmt_free(s->fn_def.body);
            break;
        case STMT_EXPR: expr_free(s->expr); break;
        case STMT_BLOCK:
            for (int i = 0; i < s->block.len; i++)
                stmt_free(s->block.data[i]);
            vector_free(&s->block);
            break;
        case STMT_STRUCT_DEF:
            free(s->struct_def.name);
            for (int i = 0; i < s->struct_def.fields.len; i++)
                free(s->struct_def.fields.data[i].name);
            vector_free(&s->struct_def.fields);
            break;
        case STMT_FIELD_ASSIGN:
            expr_free(s->field_assign.object);
            free(s->field_assign.name);
            expr_free(s->field_assign.value);
            break;
        case STMT_INDEX_ASSIGN:
            expr_free(s->index_assign.object);
            expr_free(s->index_assign.index);
            expr_free(s->index_assign.value);
            break;
        case STMT_IMPL:
            free(s->impl.struct_name);
            for (int i = 0; i < s->impl.methods.len; i++)
                stmt_free(s->impl.methods.data[i]);
            vector_free(&s->impl.methods);
            break;
        case STMT_INTERFACE_DEF:
            free(s->interface_def.name);
            for (int i = 0; i < s->interface_def.sigs.len; i++)
                stmt_free(s->interface_def.sigs.data[i]);
            vector_free(&s->interface_def.sigs);
            break;
        case STMT_IMPLEMENT:
            free(s->implement.interface_name);
            free(s->implement.struct_name);
            for (int i = 0; i < s->implement.methods.len; i++)
                stmt_free(s->implement.methods.data[i]);
            vector_free(&s->implement.methods);
            break;
        case STMT_IMPORT:
            free(s->import.path);
            free(s->import.alias);
            break;
    }
    free(s);
}

void program_free(Program *p) {
    for (int i = 0; i < p->stmts.len; i++)
        stmt_free(p->stmts.data[i]);
    vector_free(&p->stmts);
}
