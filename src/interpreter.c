#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interpreter.h"

/* ── Signal type for unwinding the call stack on return ───────────── */
/*
 * When a return statement executes deep inside nested calls, we need to
 * stop executing statements and bubble the value back up to the function
 * that was called. We do this with a global signal rather than setjmp/longjmp.
 */
static int   g_returning = 0;
static Value g_return_val;

static void runtime_error(Interpreter *interp, int line, const char *msg) {
    fprintf(stderr, "[line %d] Runtime error: %s\n", line, msg);
    interp->had_error = 1;
}

/* ── Forward declarations ─────────────────────────────────────────── */
static Value eval_expr(Interpreter *interp, Expr *e, Env *env);
static void  exec_stmt(Interpreter *interp, Stmt *s, Env *env);

/* ── Struct / interface registry helpers ──────────────────────────── */

static void load_error(Interpreter *interp, int line, const char *msg) {
    fprintf(stderr, "[line %d] Load error: %s\n", line, msg);
    interp->had_error = 1;
}

static StructType *find_struct(Interpreter *interp, const char *name) {
    for (int i = 0; i < interp->struct_count; i++)
        if (strcmp(interp->structs[i].def->struct_def.name, name) == 0)
            return &interp->structs[i];
    return NULL;
}

static Stmt *find_interface(Interpreter *interp, const char *name) {
    for (int i = 0; i < interp->interface_count; i++)
        if (strcmp(interp->interfaces[i]->interface_def.name, name) == 0)
            return interp->interfaces[i];
    return NULL;
}

static Stmt *struct_find_method(StructType *st, const char *name) {
    for (int i = 0; i < st->method_count; i++)
        if (strcmp(st->method_names[i], name) == 0)
            return st->methods[i];
    return NULL;
}

/* Add a method to a struct's table, reporting a load error on a duplicate name
   (a method may not be defined twice across the impl and implement blocks). */
static void type_add_method(Interpreter *interp, StructType *st, Stmt *fn, int line) {
    if (struct_find_method(st, fn->fn_def.name)) {
        fprintf(stderr, "[line %d] Load error: duplicate method '%s' on struct '%s'\n",
                line, fn->fn_def.name, st->def->struct_def.name);
        interp->had_error = 1;
        return;
    }
    if (st->method_count >= MAX_METHODS) { load_error(interp, line, "Too many methods on struct"); return; }
    st->method_names[st->method_count] = fn->fn_def.name;
    st->methods[st->method_count]      = fn;
    st->method_count++;
}

/*
 * Evaluate call arguments into slots named by `slot_names` (a function's
 * parameters or a struct's fields). Positional arguments fill slots in order;
 * named arguments fill by matching name. Unfilled slots default to null.
 * Returns 0 (and flags a runtime error) on too many positional args, an unknown
 * named slot, or a slot filled twice.
 */
static int bind_args(Interpreter *interp, vector(Arg) args, Env *env,
                     const char **slot_names, int slot_count,
                     Value *out, int line) {
    int filled[64] = {0};
    if (slot_count > 64) { runtime_error(interp, line, "Too many parameters or fields"); return 0; }
    for (int i = 0; i < slot_count; i++) out[i] = val_null();

    int pi = 0;
    for (int i = 0; i < args.len; i++) {
        Arg a = args.data[i];
        if (a.name == NULL) {                       /* positional */
            if (pi >= slot_count) { runtime_error(interp, line, "Too many arguments"); return 0; }
            out[pi] = eval_expr(interp, a.value, env);
            filled[pi++] = 1;
        } else {                                    /* named */
            int j = -1;
            for (int k = 0; k < slot_count; k++)
                if (strcmp(slot_names[k], a.name) == 0) { j = k; break; }
            if (j < 0) {
                runtime_error(interp, line, "No parameter or field with that name");
                fprintf(stderr, "  '%s'\n", a.name);
                return 0;
            }
            if (filled[j]) {
                runtime_error(interp, line, "Argument given twice");
                fprintf(stderr, "  '%s'\n", a.name);
                return 0;
            }
            out[j] = eval_expr(interp, a.value, env);
            filled[j] = 1;
        }
    }
    return 1;
}

/* ── Built-in functions ───────────────────────────────────────────── */
static Value call_builtin(Interpreter *interp, const char *name,
                          Value *args, int argc, int line) {
    if (strcmp(name, "print") == 0) {
        for (int i = 0; i < argc; i++) {
            if (i > 0) printf(" ");
            value_print(args[i]);
        }
        printf("\n");
        return val_null();
    }
    if (strcmp(name, "type") == 0) {
        if (argc != 1) { runtime_error(interp, line, "type() takes 1 argument"); return val_null(); }
        /* structs report their own type name, not the generic "struct" */
        const char *tn = args[0].type == VAL_STRUCT
                       ? args[0].strukt->type_name
                       : value_type_name(args[0].type);
        return val_string(tn, strlen(tn));
    }
    if (strcmp(name, "int") == 0) {
        if (argc != 1) { runtime_error(interp, line, "int() takes 1 argument"); return val_null(); }
        if (args[0].type == VAL_FLOAT)  return val_int((long)args[0].floating);
        if (args[0].type == VAL_INT)    return args[0];
        runtime_error(interp, line, "int() requires a number"); return val_null();
    }
    if (strcmp(name, "float") == 0) {
        if (argc != 1) { runtime_error(interp, line, "float() takes 1 argument"); return val_null(); }
        if (args[0].type == VAL_INT)    return val_float((double)args[0].integer);
        if (args[0].type == VAL_FLOAT)  return args[0];
        runtime_error(interp, line, "float() requires a number"); return val_null();
    }
    /* list methods: push, pop, len, get */
    if (strcmp(name, "push") == 0) {
        if (argc != 2 || args[0].type != VAL_LIST) { runtime_error(interp, line, "push(list, value)"); return val_null(); }
        list_push(args[0].list, args[1]);
        return val_null();
    }
    if (strcmp(name, "pop") == 0) {
        if (argc != 1 || args[0].type != VAL_LIST) { runtime_error(interp, line, "pop(list)"); return val_null(); }
        if (args[0].list->len == 0) { runtime_error(interp, line, "pop() on empty list"); return val_null(); }
        Value v = args[0].list->data[--args[0].list->len];
        return v;
    }
    if (strcmp(name, "len") == 0) {
        if (argc != 1) { runtime_error(interp, line, "len() takes 1 argument"); return val_null(); }
        if (args[0].type == VAL_LIST)   return val_int(args[0].list->len);
        if (args[0].type == VAL_MAP)    return val_int(args[0].map->len);
        if (args[0].type == VAL_STRING) return val_int((long)strlen(args[0].string));
        runtime_error(interp, line, "len() requires a list, map, or string"); return val_null();
    }
    /* map methods: has */
    if (strcmp(name, "has") == 0) {
        if (argc != 2 || args[0].type != VAL_MAP || args[1].type != VAL_STRING) {
            runtime_error(interp, line, "has(map, key)"); return val_null();
        }
        return val_bool(map_has(args[0].map, args[1].string));
    }
    runtime_error(interp, line, "Undefined function");
    return val_null();
}

/* ── Expression evaluator ─────────────────────────────────────────── */
static Value eval_expr(Interpreter *interp, Expr *e, Env *env) {
    if (interp->had_error) return val_null();

    switch (e->kind) {
        case EXPR_NUM:  {
            /* store as int if it has no fractional part */
            double v = e->num;
            return (v == (long)v) ? val_int((long)v) : val_float(v);
        }
        case EXPR_STR:   return val_string(e->str.ptr, e->str.len);
        case EXPR_BOOL:  return val_bool(e->bool_val);
        case EXPR_NULL:  return val_null();

        case EXPR_IDENT: {
            Value v;
            if (!env_get(env, e->ident, &v)) {
                runtime_error(interp, e->line, "Undefined variable");
                fprintf(stderr, "  '%s'\n", e->ident);
            }
            return v;
        }

        case EXPR_UNARY: {
            Value right = eval_expr(interp, e->unary.right, env);
            if (strcmp(e->unary.op, "-") == 0) {
                if (right.type == VAL_INT)   return val_int(-right.integer);
                if (right.type == VAL_FLOAT) return val_float(-right.floating);
                runtime_error(interp, e->line, "Unary '-' requires a number");
            } else { /* not */
                return val_bool(!right.boolean);
            }
            return val_null();
        }

        case EXPR_BINARY: {
            Value left  = eval_expr(interp, e->binary.left,  env);
            Value right = eval_expr(interp, e->binary.right, env);
            const char *op = e->binary.op;

            /* logical */
            if (strcmp(op, "&&") == 0) return val_bool(left.boolean && right.boolean);
            if (strcmp(op, "||") == 0) return val_bool(left.boolean || right.boolean);

            /* equality works across all types */
            if (strcmp(op, "==") == 0) {
                if (left.type != right.type) return val_bool(0);
                switch (left.type) {
                    case VAL_INT:    return val_bool(left.integer  == right.integer);
                    case VAL_FLOAT:  return val_bool(left.floating == right.floating);
                    case VAL_BOOL:   return val_bool(left.boolean  == right.boolean);
                    case VAL_STRING: return val_bool(strcmp(left.string, right.string) == 0);
                    case VAL_NULL:   return val_bool(1);
                    case VAL_LIST:   return val_bool(left.list == right.list);
                    case VAL_MAP:    return val_bool(left.map  == right.map);
                    case VAL_STRUCT: return val_bool(left.strukt == right.strukt);
                }
            }
            if (strcmp(op, "!=") == 0) {
                if (left.type != right.type) return val_bool(1);
                switch (left.type) {
                    case VAL_INT:    return val_bool(left.integer  != right.integer);
                    case VAL_FLOAT:  return val_bool(left.floating != right.floating);
                    case VAL_BOOL:   return val_bool(left.boolean  != right.boolean);
                    case VAL_STRING: return val_bool(strcmp(left.string, right.string) != 0);
                    case VAL_NULL:   return val_bool(0);
                    case VAL_LIST:   return val_bool(left.list != right.list);
                    case VAL_MAP:    return val_bool(left.map  != right.map);
                    case VAL_STRUCT: return val_bool(left.strukt != right.strukt);
                }
            }

            /* arithmetic — promote int to float if mixed */
            int  both_int   = left.type == VAL_INT   && right.type == VAL_INT;
            int  any_float  = left.type == VAL_FLOAT || right.type == VAL_FLOAT;
            double l = left.type  == VAL_INT ? (double)left.integer  : left.floating;
            double r = right.type == VAL_INT ? (double)right.integer : right.floating;

            if (any_float || both_int) {
                if (strcmp(op, "+") == 0) return both_int ? val_int((long)(l+r)) : val_float(l+r);
                if (strcmp(op, "-") == 0) return both_int ? val_int((long)(l-r)) : val_float(l-r);
                if (strcmp(op, "*") == 0) return both_int ? val_int((long)(l*r)) : val_float(l*r);
                if (strcmp(op, "/") == 0) {
                    if (r == 0) { runtime_error(interp, e->line, "Division by zero"); return val_null(); }
                    return val_float(l / r);
                }
                if (strcmp(op, "%") == 0) {
                    if (!both_int) { runtime_error(interp, e->line, "'%' requires integers"); return val_null(); }
                    if (right.integer == 0) { runtime_error(interp, e->line, "Modulo by zero"); return val_null(); }
                    return val_int(left.integer % right.integer);
                }
                if (strcmp(op, "<")  == 0) return val_bool(l <  r);
                if (strcmp(op, "<=") == 0) return val_bool(l <= r);
                if (strcmp(op, ">")  == 0) return val_bool(l >  r);
                if (strcmp(op, ">=") == 0) return val_bool(l >= r);
            }

            /* string concatenation */
            if (strcmp(op, "+") == 0 && left.type == VAL_STRING && right.type == VAL_STRING) {
                int  len = strlen(left.string) + strlen(right.string);
                char *s  = malloc(len + 1);
                strcpy(s, left.string);
                strcat(s, right.string);
                Value v = (Value){ VAL_STRING, .string = s };
                return v;
            }

            runtime_error(interp, e->line, "Invalid operands for operator");
            return val_null();
        }

        case EXPR_CALL: {
            /* struct construction: the callee names a registered struct type */
            StructType *st = find_struct(interp, e->call.name);
            if (st) {
                int n = st->def->struct_def.fields.len;
                if (n > 64) { runtime_error(interp, e->line, "Struct has too many fields"); return val_null(); }
                const char *names[64];
                for (int i = 0; i < n; i++) names[i] = st->def->struct_def.fields.data[i].name;
                Value vals[64];
                if (!bind_args(interp, e->call.args, env, names, n, vals, e->line))
                    return val_null();
                Value inst = val_struct(e->call.name);
                for (int i = 0; i < n; i++) struct_set(inst.strukt, names[i], vals[i]);
                return inst;
            }

            /* user-defined function: fn.string holds a pointer to the Stmt */
            Value fn;
            if (env_get(interp->globals, e->call.name, &fn)) {
                Stmt *fn_stmt;
                memcpy(&fn_stmt, fn.string, sizeof(Stmt *));

                int n = fn_stmt->fn_def.params.len;
                if (n > 64) { runtime_error(interp, e->line, "Function has too many parameters"); return val_null(); }
                const char *names[64];
                for (int i = 0; i < n; i++) names[i] = fn_stmt->fn_def.params.data[i].name;
                Value vals[64];
                if (!bind_args(interp, e->call.args, env, names, n, vals, e->line))
                    return val_null();

                Env *fn_env = env_new(interp->globals);
                for (int i = 0; i < n; i++) env_define(fn_env, names[i], vals[i]);
                exec_stmt(interp, fn_stmt->fn_def.body, fn_env);
                Value ret = g_returning ? g_return_val : val_null();
                g_returning = 0;
                env_free(fn_env);
                return ret;
            }

            /* built-in function: positional arguments only */
            int argc = e->call.args.len;
            if (argc > 64) { runtime_error(interp, e->line, "Too many arguments"); return val_null(); }
            Value args[64];
            for (int i = 0; i < argc; i++) {
                if (e->call.args.data[i].name != NULL) {
                    runtime_error(interp, e->line, "Built-in functions do not take named arguments");
                    return val_null();
                }
                args[i] = eval_expr(interp, e->call.args.data[i].value, env);
            }
            return call_builtin(interp, e->call.name, args, argc, e->line);
        }

        case EXPR_LIST: {
            Value v = val_list_empty();
            for (int i = 0; i < e->list.len; i++)
                list_push(v.list, eval_expr(interp, e->list.data[i], env));
            return v;
        }

        case EXPR_MAP: {
            Value v = val_map_empty();
            for (int i = 0; i < e->map.len; i++) {
                Value key = eval_expr(interp, e->map.data[i].key, env);
                Value val = eval_expr(interp, e->map.data[i].value, env);
                if (key.type != VAL_STRING) {
                    runtime_error(interp, e->line, "Map keys must be strings");
                    return val_null();
                }
                map_set(v.map, key.string, val);
            }
            return v;
        }

        case EXPR_INDEX: {
            Value obj = eval_expr(interp, e->index.object, env);
            Value key = eval_expr(interp, e->index.index,  env);
            if (obj.type == VAL_LIST) {
                if (key.type != VAL_INT) { runtime_error(interp, e->line, "List index must be an integer"); return val_null(); }
                return list_get(obj.list, (int)key.integer);
            }
            if (obj.type == VAL_MAP) {
                if (key.type != VAL_STRING) { runtime_error(interp, e->line, "Map key must be a string"); return val_null(); }
                Value out;
                if (!map_get(obj.map, key.string, &out)) return val_null();
                return out;
            }
            runtime_error(interp, e->line, "Cannot index into this type");
            return val_null();
        }

        case EXPR_FIELD: {
            Value obj = eval_expr(interp, e->field.object, env);
            if (obj.type != VAL_STRUCT) {
                runtime_error(interp, e->line, "Field access on a non-struct value");
                return val_null();
            }
            Value out;
            if (!struct_get(obj.strukt, e->field.name, &out)) {
                runtime_error(interp, e->line, "No such field");
                fprintf(stderr, "  '%s' on '%s'\n", e->field.name, obj.strukt->type_name);
                return val_null();
            }
            return out;
        }

        case EXPR_METHOD: {
            Value obj = eval_expr(interp, e->method.object, env);
            if (obj.type != VAL_STRUCT) {
                runtime_error(interp, e->line, "Method call on a non-struct value");
                return val_null();
            }
            StructType *st = find_struct(interp, obj.strukt->type_name);
            Stmt *m = st ? struct_find_method(st, e->method.name) : NULL;
            if (!m) {
                runtime_error(interp, e->line, "Undefined method");
                fprintf(stderr, "  '%s' on '%s'\n", e->method.name, obj.strukt->type_name);
                return val_null();
            }

            /* params[0] is the receiver (self); the rest are bound from args */
            int np       = m->fn_def.params.len;
            int has_self = np > 0;
            int nslots   = has_self ? np - 1 : 0;
            if (nslots > 64) { runtime_error(interp, e->line, "Method has too many parameters"); return val_null(); }
            const char *names[64];
            for (int i = 0; i < nslots; i++) names[i] = m->fn_def.params.data[i + 1].name;
            Value vals[64];
            if (!bind_args(interp, e->method.args, env, names, nslots, vals, e->line))
                return val_null();

            Env *fn_env = env_new(interp->globals);
            if (has_self) env_define(fn_env, m->fn_def.params.data[0].name, obj);
            for (int i = 0; i < nslots; i++) env_define(fn_env, names[i], vals[i]);
            exec_stmt(interp, m->fn_def.body, fn_env);
            Value ret = g_returning ? g_return_val : val_null();
            g_returning = 0;
            env_free(fn_env);
            return ret;
        }
    }
    return val_null();
}

/* ── Statement executor ───────────────────────────────────────────── */
static void exec_stmt(Interpreter *interp, Stmt *s, Env *env) {
    if (interp->had_error || g_returning) return;

    switch (s->kind) {
        case STMT_VAR_DECL: {
            Value v = eval_expr(interp, s->var_decl.init, env);
            env_define(env, s->var_decl.name, v);
            break;
        }
        case STMT_ASSIGN: {
            Value v = eval_expr(interp, s->assign.value, env);
            if (!env_set(env, s->assign.name, v)) {
                runtime_error(interp, s->line, "Assignment to undefined variable");
                fprintf(stderr, "  '%s'\n", s->assign.name);
            }
            break;
        }
        case STMT_EXPR:
            eval_expr(interp, s->expr, env);
            break;

        case STMT_BLOCK: {
            Env *block_env = env_new(env);
            for (int i = 0; i < s->block.len && !g_returning; i++)
                exec_stmt(interp, s->block.data[i], block_env);
            env_free(block_env);
            break;
        }
        case STMT_IF: {
            Value cond = eval_expr(interp, s->if_stmt.condition, env);
            int truthy = (cond.type == VAL_BOOL)  ? cond.boolean :
                         (cond.type == VAL_INT)    ? cond.integer != 0 :
                         (cond.type == VAL_FLOAT)  ? cond.floating != 0.0 :
                         (cond.type == VAL_NULL)   ? 0 : 1;
            if (truthy)
                exec_stmt(interp, s->if_stmt.then_block, env);
            else if (s->if_stmt.else_block)
                exec_stmt(interp, s->if_stmt.else_block, env);
            break;
        }
        case STMT_WHILE: {
            for (;;) {
                Value cond = eval_expr(interp, s->while_stmt.condition, env);
                int truthy = (cond.type == VAL_BOOL)  ? cond.boolean :
                             (cond.type == VAL_INT)    ? cond.integer != 0 :
                             (cond.type == VAL_FLOAT)  ? cond.floating != 0.0 :
                             (cond.type == VAL_NULL)   ? 0 : 1;
                if (!truthy || interp->had_error) break;
                exec_stmt(interp, s->while_stmt.body, env);
                if (g_returning) break;
            }
            break;
        }
        case STMT_RETURN:
            g_return_val = s->ret.value
                ? eval_expr(interp, s->ret.value, env)
                : val_null();
            g_returning = 1;
            break;

        case STMT_FN_DEF: {
            /*
             * Store a pointer to the Stmt itself as the function's value.
             * We pack the pointer into a string-sized buffer — a simple trick
             * to avoid adding a new VAL_FUNCTION type for now.
             */
            char buf[sizeof(Stmt *)];
            memcpy(buf, &s, sizeof(Stmt *));
            Value fn = (Value){ VAL_STRING, .string = malloc(sizeof(Stmt *)) };
            memcpy(fn.string, &s, sizeof(Stmt *));
            env_define(interp->globals, s->fn_def.name, fn);
            break;
        }

        case STMT_FIELD_ASSIGN: {
            Value obj = eval_expr(interp, s->field_assign.object, env);
            if (obj.type != VAL_STRUCT) {
                runtime_error(interp, s->line, "Field assignment on a non-struct value");
                break;
            }
            if (!struct_has(obj.strukt, s->field_assign.name)) {
                runtime_error(interp, s->line, "No such field");
                fprintf(stderr, "  '%s' on '%s'\n", s->field_assign.name, obj.strukt->type_name);
                break;
            }
            Value v = eval_expr(interp, s->field_assign.value, env);
            struct_set(obj.strukt, s->field_assign.name, v);
            break;
        }

        /* Declarations are resolved at load time (see interpreter_prepare);
           there is nothing to execute when they appear in the statement stream. */
        case STMT_STRUCT_DEF:
        case STMT_INTERFACE_DEF:
        case STMT_IMPL:
        case STMT_IMPLEMENT:
            break;
    }
}

/* ── Load-time pass: register types, attach methods, check conformance ─ */
static void interpreter_prepare(Interpreter *interp, Program *prog) {
    /* Pass 1: register every struct and interface definition, so impl/implement
       blocks and constructions may refer to types declared later in the file. */
    for (int i = 0; i < prog->stmts.len; i++) {
        Stmt *s = prog->stmts.data[i];
        if (s->kind == STMT_STRUCT_DEF) {
            if (find_struct(interp, s->struct_def.name)) { load_error(interp, s->line, "Duplicate struct definition"); return; }
            if (interp->struct_count >= MAX_STRUCT_TYPES) { load_error(interp, s->line, "Too many struct types"); return; }
            StructType *st = &interp->structs[interp->struct_count++];
            memset(st, 0, sizeof(*st));
            st->def = s;
        } else if (s->kind == STMT_INTERFACE_DEF) {
            if (find_interface(interp, s->interface_def.name)) { load_error(interp, s->line, "Duplicate interface definition"); return; }
            if (interp->interface_count >= MAX_INTERFACES) { load_error(interp, s->line, "Too many interfaces"); return; }
            interp->interfaces[interp->interface_count++] = s;
        }
    }

    /* Pass 2: attach impl and implement methods to their struct's method table,
       enforcing one impl block per struct, and verify interface conformance. */
    for (int i = 0; i < prog->stmts.len; i++) {
        Stmt *s = prog->stmts.data[i];

        if (s->kind == STMT_IMPL) {
            StructType *st = find_struct(interp, s->impl.struct_name);
            if (!st)            { load_error(interp, s->line, "impl block for an undefined struct"); return; }
            if (st->has_impl)   { load_error(interp, s->line, "Duplicate impl block (a struct's methods must live in one impl block)"); return; }
            st->has_impl = 1;
            for (int m = 0; m < s->impl.methods.len; m++) {
                type_add_method(interp, st, s->impl.methods.data[m], s->line);
                if (interp->had_error) return;
            }
        } else if (s->kind == STMT_IMPLEMENT) {
            Stmt       *iface = find_interface(interp, s->implement.interface_name);
            StructType *st    = find_struct(interp, s->implement.struct_name);
            if (!iface) { load_error(interp, s->line, "implement of an undefined interface"); return; }
            if (!st)    { load_error(interp, s->line, "implement for an undefined struct"); return; }
            for (int k = 0; k < st->interface_count; k++)
                if (strcmp(st->interfaces[k], s->implement.interface_name) == 0) {
                    load_error(interp, s->line, "Struct already implements this interface"); return;
                }
            if (st->interface_count >= MAX_IMPL_IFACES) { load_error(interp, s->line, "Struct implements too many interfaces"); return; }
            st->interfaces[st->interface_count++] = s->implement.interface_name;

            for (int m = 0; m < s->implement.methods.len; m++) {
                type_add_method(interp, st, s->implement.methods.data[m], s->line);
                if (interp->had_error) return;
            }

            /* conformance: every signature in the interface must be defined here */
            for (int sg = 0; sg < iface->interface_def.sigs.len; sg++) {
                const char *req = iface->interface_def.sigs.data[sg]->fn_def.name;
                int found = 0;
                for (int m = 0; m < s->implement.methods.len; m++)
                    if (strcmp(s->implement.methods.data[m]->fn_def.name, req) == 0) { found = 1; break; }
                if (!found) {
                    fprintf(stderr, "[line %d] Load error: struct '%s' does not implement method '%s' required by interface '%s'\n",
                            s->line, s->implement.struct_name, req, s->implement.interface_name);
                    interp->had_error = 1;
                    return;
                }
            }
        }
    }
}

/* ── Entry points ─────────────────────────────────────────────────── */
void interpreter_init(Interpreter *interp) {
    interp->globals         = env_new(NULL);
    interp->had_error       = 0;
    interp->struct_count    = 0;
    interp->interface_count = 0;
}

void interpreter_free(Interpreter *interp) {
    env_free(interp->globals);
}

void interpreter_run(Interpreter *interp, Program *prog) {
    /* Resolve all type declarations and validate interface conformance before
       running anything; a load error stops execution entirely. */
    interpreter_prepare(interp, prog);
    if (interp->had_error) return;

    for (int i = 0; i < prog->stmts.len; i++) {
        Stmt *s = prog->stmts.data[i];
        /* type declarations were already handled in the load-time pass */
        if (s->kind == STMT_STRUCT_DEF || s->kind == STMT_INTERFACE_DEF ||
            s->kind == STMT_IMPL       || s->kind == STMT_IMPLEMENT)
            continue;
        exec_stmt(interp, s, interp->globals);
        if (interp->had_error) break;
    }
}
