#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"

/* ── Signal type for unwinding the call stack on return ───────────── */
/*
 * When a return statement executes deep inside nested calls, we need to
 * stop executing statements and bubble the value back up to the function
 * that was called. We do this with a global signal rather than setjmp/longjmp.
 */
static int   g_returning = 0;
static Value g_return_val;

/* ── Loop control signals ─────────────────────────────────────────── */
/*
 * `break` and `continue` unwind like `return` does: a flag is set and statement
 * execution stops bubbling up until the nearest enclosing loop consumes it.
 * g_loop_depth tracks how many loops are currently running in the *current*
 * function activation (it is saved/restored across calls), so a stray break or
 * continue outside any loop is a clean runtime error rather than corruption.
 */
static int g_breaking   = 0;
static int g_continuing = 0;
static int g_loop_depth = 0;

static void runtime_error(Interpreter *interp, int line, const char *msg) {
    fprintf(stderr, "[line %d] Runtime error: %s\n", line, msg);
    interp->had_error = 1;
}

/* Truthiness used by if/while/for conditions. */
static int is_truthy(Value v) {
    switch (v.type) {
        case VAL_BOOL:  return v.boolean;
        case VAL_INT:   return v.integer != 0;
        case VAL_FLOAT: return v.floating != 0.0;
        case VAL_NULL:  return 0;
        default:        return 1;   /* strings, lists, maps, structs */
    }
}

/* ── Declared-type enforcement (list<…> / map<…> annotations) ──────── */

static const char *type_base_name(MorayType t) {
    switch (t) {
        case TYPE_INT:    return "int";
        case TYPE_FLOAT:  return "float";
        case TYPE_STRING: return "string";
        case TYPE_BOOL:   return "bool";
        case TYPE_LIST:   return "list";
        case TYPE_MAP:    return "map";
        case TYPE_STRUCT: return "struct";
        case TYPE_ANY:    return "any";
        case TYPE_VOID:   return "any";
    }
    return "?";
}

static int base_matches(MorayType base, Value v) {
    switch (base) {
        case TYPE_INT:    return v.type == VAL_INT;
        case TYPE_FLOAT:  return v.type == VAL_FLOAT;
        case TYPE_STRING: return v.type == VAL_STRING;
        case TYPE_BOOL:   return v.type == VAL_BOOL;
        case TYPE_LIST:   return v.type == VAL_LIST;
        case TYPE_MAP:    return v.type == VAL_MAP;
        case TYPE_STRUCT: return v.type == VAL_STRUCT;
        case TYPE_ANY:    return 1;   /* matches anything */
        case TYPE_VOID:   return 1;
    }
    return 0;
}

/*
 * Recursively check a runtime value against a declared annotation. Raises a
 * runtime error and returns 0 on the first mismatch. A list's elements are
 * checked against p0; a map's values against p1 (keys are always strings, so a
 * declared key type other than 'string' is rejected). Annotations nest.
 */
static int check_declared_type(Interpreter *interp, TypeAnn *t, Value v, int line) {
    if (!base_matches(t->base, v)) {
        char msg[96];
        snprintf(msg, sizeof msg, "type mismatch: expected '%s', got '%s'",
                 type_base_name(t->base), value_type_name(v.type));
        runtime_error(interp, line, msg);
        return 0;
    }
    if (t->base == TYPE_LIST && t->p0) {
        for (int i = 0; i < v.list->len; i++)
            if (!check_declared_type(interp, t->p0, v.list->data[i], line)) return 0;
    }
    if (t->base == TYPE_MAP) {
        for (int i = 0; i < v.map->len; i++) {
            if (t->p0 && !check_declared_type(interp, t->p0, v.map->pairs[i].key,   line)) return 0;
            if (t->p1 && !check_declared_type(interp, t->p1, v.map->pairs[i].value, line)) return 0;
        }
    }
    return 1;
}

/* ── Forward declarations ─────────────────────────────────────────── */
static Value eval_expr(Interpreter *interp, Expr *e, Env *env);
static void  exec_stmt(Interpreter *interp, Stmt *s, Env *env);
static void  interpreter_prepare(Interpreter *interp, Program *prog);
static Env  *load_module(Interpreter *interp, const char *path, int line);

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

    /*
     * Each evaluated argument is pinned on the GC protect stack as we go: a
     * later argument's evaluation may run code that triggers a collection, and
     * the arguments filled so far are not yet reachable from any scope. The
     * caller stores them into a fresh environment (or struct) without any
     * intervening collection, so we release them here before returning.
     */
    int pi = 0;
    int pinned = 0;
    for (int i = 0; i < args.len; i++) {
        Arg a = args.data[i];
        if (a.name == NULL) {                       /* positional */
            if (pi >= slot_count) { runtime_error(interp, line, "Too many arguments"); gc_pop(pinned); return 0; }
            out[pi] = gc_protect(eval_expr(interp, a.value, env));
            pinned++;
            filled[pi++] = 1;
        } else {                                    /* named */
            int j = -1;
            for (int k = 0; k < slot_count; k++)
                if (strcmp(slot_names[k], a.name) == 0) { j = k; break; }
            if (j < 0) {
                runtime_error(interp, line, "No parameter or field with that name");
                fprintf(stderr, "  '%s'\n", a.name);
                gc_pop(pinned); return 0;
            }
            if (filled[j]) {
                runtime_error(interp, line, "Argument given twice");
                fprintf(stderr, "  '%s'\n", a.name);
                gc_pop(pinned); return 0;
            }
            out[j] = gc_protect(eval_expr(interp, a.value, env));
            pinned++;
            filled[j] = 1;
        }
    }
    gc_pop(pinned);
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
    /* range(end) or range(start, end) → a list of ints [start, end) */
    if (strcmp(name, "range") == 0) {
        long start, end;
        if (argc == 1 && args[0].type == VAL_INT) {
            start = 0; end = args[0].integer;
        } else if (argc == 2 && args[0].type == VAL_INT && args[1].type == VAL_INT) {
            start = args[0].integer; end = args[1].integer;
        } else {
            runtime_error(interp, line, "range(end) or range(start, end) with integers");
            return val_null();
        }
        Value list = val_list_empty();
        for (long v = start; v < end; v++)
            list_push(list.list, val_int(v));
        return list;
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
    /* map methods: has, keys, values, items */
    if (strcmp(name, "has") == 0) {
        if (argc != 2 || args[0].type != VAL_MAP) {
            runtime_error(interp, line, "has(map, key)"); return val_null();
        }
        return val_bool(map_has(args[0].map, args[1]));
    }
    if (strcmp(name, "keys") == 0) {
        if (argc != 1 || args[0].type != VAL_MAP) { runtime_error(interp, line, "keys(map)"); return val_null(); }
        MorayMap *m = args[0].map;
        Value list = val_list_empty();
        for (int i = 0; i < m->len; i++)
            list_push(list.list, m->pairs[i].key);
        return list;
    }
    if (strcmp(name, "values") == 0) {
        if (argc != 1 || args[0].type != VAL_MAP) { runtime_error(interp, line, "values(map)"); return val_null(); }
        MorayMap *m = args[0].map;
        Value list = val_list_empty();
        for (int i = 0; i < m->len; i++)
            list_push(list.list, m->pairs[i].value);
        return list;
    }
    if (strcmp(name, "items") == 0) {
        if (argc != 1 || args[0].type != VAL_MAP) { runtime_error(interp, line, "items(map)"); return val_null(); }
        MorayMap *m = args[0].map;
        /* a list of [key, value] pairs — iterate with `for pair in items(m)` */
        Value list = val_list_empty();
        for (int i = 0; i < m->len; i++) {
            Value pair = val_list_empty();
            list_push(pair.list, m->pairs[i].key);
            list_push(pair.list, m->pairs[i].value);
            list_push(list.list, pair);
        }
        return list;
    }
    runtime_error(interp, line, "Undefined function");
    return val_null();
}

/* Apply a binary operator to two already-evaluated values. Shared by the
   EXPR_BINARY evaluator and compound assignment (`+=`, `++`, …). */
static Value eval_binary_op(Interpreter *interp, const char *op,
                            Value left, Value right, int line) {
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
            case VAL_FUNCTION: return val_bool(left.func == right.func);
            case VAL_MODULE:   return val_bool(left.menv == right.menv);
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
            case VAL_FUNCTION: return val_bool(left.func != right.func);
            case VAL_MODULE:   return val_bool(left.menv != right.menv);
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
            if (r == 0) { runtime_error(interp, line, "Division by zero"); return val_null(); }
            return val_float(l / r);
        }
        if (strcmp(op, "%") == 0) {
            if (!both_int) { runtime_error(interp, line, "'%' requires integers"); return val_null(); }
            if (right.integer == 0) { runtime_error(interp, line, "Modulo by zero"); return val_null(); }
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
        char *s  = gc_new_string_buffer(len + 1);
        strcpy(s, left.string);
        strcat(s, right.string);
        return (Value){ VAL_STRING, .string = s };
    }

    runtime_error(interp, line, "Invalid operands for operator");
    return val_null();
}

/* Construct a struct instance: bind the call's arguments to the type's fields.
   Arguments are evaluated in `arg_env` (the caller's scope). */
static Value construct_struct(Interpreter *interp, StructType *st,
                              vector(Arg) args, Env *arg_env, int line) {
    int n = st->def->struct_def.fields.len;
    if (n > 64) { runtime_error(interp, line, "Struct has too many fields"); return val_null(); }
    const char *names[64];
    for (int i = 0; i < n; i++) names[i] = st->def->struct_def.fields.data[i].name;
    Value vals[64];
    if (!bind_args(interp, args, arg_env, names, n, vals, line))
        return val_null();
    Value inst = val_struct(st->def->struct_def.name);
    for (int i = 0; i < n; i++) struct_set(inst.strukt, names[i], vals[i]);
    return inst;
}

/* Invoke a closure: bind arguments (evaluated in the caller's `arg_env`) into a
   fresh activation whose parent is the function's captured scope, then run the
   body. This is the one place lexical scoping + closures come together — the
   activation chains to `fn->closure`, not to the call site. */
static Value call_function(Interpreter *interp, MorayFunc *fn,
                           vector(Arg) args, Env *arg_env, int line) {
    Stmt *fn_stmt = fn->def;
    int n = fn_stmt->fn_def.params.len;
    if (n > 64) { runtime_error(interp, line, "Function has too many parameters"); return val_null(); }
    const char *names[64];
    for (int i = 0; i < n; i++) names[i] = fn_stmt->fn_def.params.data[i].name;
    Value vals[64];
    if (!bind_args(interp, args, arg_env, names, n, vals, line))
        return val_null();

    Env *fn_env = env_new(fn->closure);   /* lexical parent = captured scope */
    for (int i = 0; i < n; i++) env_define(fn_env, names[i], vals[i]);
    int saved_depth = g_loop_depth;        /* break/continue can't cross a call */
    g_loop_depth = 0;
    exec_stmt(interp, fn_stmt->fn_def.body, fn_env);
    g_loop_depth = saved_depth;
    int   returned = g_returning;
    Value ret      = returned ? g_return_val : val_null();
    g_returning = 0;
    env_free(fn_env);                      /* may collect; a returned closure
                                              keeps its captured scope alive   */
    if (returned) gc_pop(1);               /* release STMT_RETURN's protect    */
    return ret;
}

/* Look up a name in a module's own top-level frame (not its parents, so a
   module reference never resolves to the importer's globals). */
static int module_lookup(Env *menv, const char *name, Value *out) {
    for (int i = 0; i < menv->count; i++)
        if (strcmp(menv->names[i], name) == 0) { *out = menv->values[i]; return 1; }
    return 0;
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
            /* Pin the left operand across the right's evaluation: if it is a
               fresh heap temporary (e.g. a string from a nested concat) a
               collection during the right side would otherwise free it. */
            Value left  = gc_protect(eval_expr(interp, e->binary.left, env));
            Value right = eval_expr(interp, e->binary.right, env);
            gc_pop(1);
            return eval_binary_op(interp, e->binary.op, left, right, e->line);
        }

        case EXPR_CALLV: {
            /* Call whatever a callee expression evaluates to (e.g. f()(), xs[i]()).
               Keep the callee pinned across argument evaluation inside the call. */
            Value callee = gc_protect(eval_expr(interp, e->callv.callee, env));
            if (callee.type != VAL_FUNCTION) {
                gc_pop(1);
                runtime_error(interp, e->line, "Value is not callable");
                return val_null();
            }
            Value r = call_function(interp, callee.func, e->callv.args, env, e->line);
            gc_pop(1);
            return r;
        }

        case EXPR_CALL: {
            /* struct construction: the callee names a registered struct type */
            StructType *st = find_struct(interp, e->call.name);
            if (st)
                return construct_struct(interp, st, e->call.args, env, e->line);

            /* user-defined function: resolved lexically, so a closure captured in
               an outer scope (or a function value held in a variable) is callable */
            Value fn;
            if (env_get(env, e->call.name, &fn) && fn.type == VAL_FUNCTION)
                return call_function(interp, fn.func, e->call.args, env, e->line);

            /* built-in function: positional arguments only */
            int argc = e->call.args.len;
            if (argc > 64) { runtime_error(interp, e->line, "Too many arguments"); return val_null(); }
            Value args[64];
            int   pinned = 0;
            for (int i = 0; i < argc; i++) {
                if (e->call.args.data[i].name != NULL) {
                    runtime_error(interp, e->line, "Built-in functions do not take named arguments");
                    gc_pop(pinned);
                    return val_null();
                }
                args[i] = gc_protect(eval_expr(interp, e->call.args.data[i].value, env));
                pinned++;
            }
            Value r = call_builtin(interp, e->call.name, args, argc, e->line);
            gc_pop(pinned);
            return r;
        }

        case EXPR_LIST: {
            /* Pin the list while building it: evaluating elements may collect,
               and the half-built list (and what it already holds) must survive. */
            Value v = gc_protect(val_list_empty());
            for (int i = 0; i < e->list.len; i++)
                list_push(v.list, eval_expr(interp, e->list.data[i], env));
            gc_pop(1);
            return v;
        }

        case EXPR_MAP: {
            Value v = gc_protect(val_map_empty());
            for (int i = 0; i < e->map.len; i++) {
                Value key = gc_protect(eval_expr(interp, e->map.data[i].key, env));
                Value val = eval_expr(interp, e->map.data[i].value, env);
                map_set(v.map, key, val);
                gc_pop(1);       /* key (val is now reachable through v) */
            }
            gc_pop(1);           /* v */
            return v;
        }

        case EXPR_INDEX: {
            Value obj = gc_protect(eval_expr(interp, e->index.object, env));
            Value key = eval_expr(interp, e->index.index,  env);
            gc_pop(1);
            if (obj.type == VAL_LIST) {
                if (key.type != VAL_INT) { runtime_error(interp, e->line, "List index must be an integer"); return val_null(); }
                return list_get(obj.list, (int)key.integer);
            }
            if (obj.type == VAL_MAP) {
                Value out;
                if (!map_get(obj.map, key, &out)) return val_null();
                return out;
            }
            runtime_error(interp, e->line, "Cannot index into this type");
            return val_null();
        }

        case EXPR_FIELD: {
            Value obj = eval_expr(interp, e->field.object, env);
            if (obj.type == VAL_MODULE) {        /* module member access: math.pi */
                Value out;
                if (module_lookup(obj.menv, e->field.name, &out)) return out;
                runtime_error(interp, e->line, "Module has no such member");
                fprintf(stderr, "  '%s'\n", e->field.name);
                return val_null();
            }
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
            if (obj.type == VAL_MODULE) {        /* module member call: math.add(1, 2) */
                Value member;
                if (module_lookup(obj.menv, e->method.name, &member) &&
                    member.type == VAL_FUNCTION)
                    return call_function(interp, member.func, e->method.args, env, e->line);
                /* type constructors are registered globally; allow math.Circle(…) */
                StructType *mst = find_struct(interp, e->method.name);
                if (mst)
                    return construct_struct(interp, mst, e->method.args, env, e->line);
                runtime_error(interp, e->line, "Module has no such function");
                fprintf(stderr, "  '%s'\n", e->method.name);
                return val_null();
            }
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
            /* Keep the receiver alive across argument evaluation; it becomes a
               root once bound as `self`. */
            gc_protect(obj);
            if (!bind_args(interp, e->method.args, env, names, nslots, vals, e->line)) {
                gc_pop(1);
                return val_null();
            }

            Env *fn_env = env_new(interp->globals);
            if (has_self) env_define(fn_env, m->fn_def.params.data[0].name, obj);
            gc_pop(1);   /* obj now reachable via fn_env (or no longer needed) */
            for (int i = 0; i < nslots; i++) env_define(fn_env, names[i], vals[i]);
            int saved_depth = g_loop_depth;   /* break/continue can't cross a call */
            g_loop_depth = 0;
            exec_stmt(interp, m->fn_def.body, fn_env);
            g_loop_depth = saved_depth;
            int   returned = g_returning;
            Value ret      = returned ? g_return_val : val_null();
            g_returning = 0;
            env_free(fn_env);
            if (returned) gc_pop(1);
            return ret;
        }
    }
    return val_null();
}

/* ── Statement executor ───────────────────────────────────────────── */
static void exec_stmt(Interpreter *interp, Stmt *s, Env *env) {
    if (interp->had_error || g_returning || g_breaking || g_continuing) return;

    switch (s->kind) {
        case STMT_VAR_DECL: {
            Value v = eval_expr(interp, s->var_decl.init, env);
            /* A typed declaration (`name: type = …`) always carries an
               annotation and is enforced against the value; `any` matches all.
               Declares (shadows) in the current scope. */
            if (s->var_decl.ann)
                check_declared_type(interp, s->var_decl.ann, v, s->line);
            env_define(env, s->var_decl.name, v);
            break;
        }
        case STMT_ASSIGN: {
            Value v = eval_expr(interp, s->assign.value, env);
            if (s->assign.op[0]) {                  /* compound: x op= v  →  x = x op v */
                Value cur;
                if (!env_get(env, s->assign.name, &cur)) {
                    runtime_error(interp, s->line, "Compound assignment to undefined variable");
                    fprintf(stderr, "  '%s'\n", s->assign.name);
                    break;
                }
                v = eval_binary_op(interp, s->assign.op, cur, v, s->line);
                env_set(env, s->assign.name, v);
            } else {
                /* plain `name = value`: declare-or-assign — update the variable
                   if it exists anywhere up the scope chain, else declare it in
                   the current scope (inferred, dynamically typed). */
                if (!env_set(env, s->assign.name, v))
                    env_define(env, s->assign.name, v);
            }
            break;
        }
        case STMT_EXPR:
            eval_expr(interp, s->expr, env);
            break;

        case STMT_BLOCK: {
            Env *block_env = env_new(env);
            for (int i = 0; i < s->block.len &&
                            !g_returning && !g_breaking && !g_continuing; i++)
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
            g_loop_depth++;
            for (;;) {
                Value cond = eval_expr(interp, s->while_stmt.condition, env);
                if (!is_truthy(cond) || interp->had_error) break;
                exec_stmt(interp, s->while_stmt.body, env);
                if (g_returning) break;
                if (g_breaking)   { g_breaking = 0;   break; }
                if (g_continuing) { g_continuing = 0; }       /* fall through to re-test */
            }
            g_loop_depth--;
            break;
        }
        case STMT_FOR: {
            /* The loop scope holds the initializer's variable for the whole loop;
               the body is a block that gets its own child scope each pass. */
            Env *loop_env = env_new(env);
            if (s->for_stmt.init) exec_stmt(interp, s->for_stmt.init, loop_env);
            g_loop_depth++;
            while (!interp->had_error && !g_returning) {
                if (s->for_stmt.condition) {
                    Value cond = eval_expr(interp, s->for_stmt.condition, loop_env);
                    if (!is_truthy(cond)) break;
                }
                exec_stmt(interp, s->for_stmt.body, loop_env);
                if (g_returning) break;
                if (g_breaking) { g_breaking = 0; break; }
                if (g_continuing) g_continuing = 0;   /* still run the update step */
                if (s->for_stmt.update) exec_stmt(interp, s->for_stmt.update, loop_env);
            }
            g_loop_depth--;
            env_free(loop_env);
            break;
        }
        case STMT_FOR_IN: {
            Value iter = eval_expr(interp, s->for_in.iterable, env);
            if (iter.type != VAL_LIST) {
                runtime_error(interp, s->line, "for-in expects a list");
                break;
            }
            gc_protect(iter);                 /* the iterable may be a fresh temp */
            Env *loop_env = env_new(env);
            env_define(loop_env, s->for_in.var, val_null());
            g_loop_depth++;
            for (int i = 0; i < iter.list->len; i++) {
                env_set(loop_env, s->for_in.var, iter.list->data[i]);
                exec_stmt(interp, s->for_in.body, loop_env);
                if (interp->had_error || g_returning) break;
                if (g_breaking) { g_breaking = 0; break; }
                if (g_continuing) g_continuing = 0;
            }
            g_loop_depth--;
            env_free(loop_env);
            gc_pop(1);
            break;
        }
        case STMT_BREAK:
            if (g_loop_depth == 0) { runtime_error(interp, s->line, "'break' outside of a loop"); break; }
            g_breaking = 1;
            break;
        case STMT_CONTINUE:
            if (g_loop_depth == 0) { runtime_error(interp, s->line, "'continue' outside of a loop"); break; }
            g_continuing = 1;
            break;
        case STMT_RETURN:
            g_return_val = s->ret.value
                ? eval_expr(interp, s->ret.value, env)
                : val_null();
            /* Pin the return value: scope teardown as the stack unwinds back to
               the call site will trigger collections, and nothing else may
               reach this value yet. The call site (EXPR_CALL/EXPR_METHOD)
               releases it once it has been received. */
            gc_protect(g_return_val);
            g_returning = 1;
            break;

        case STMT_FN_DEF: {
            /* A function is a first-class closure value: the AST body plus the
               scope in force here, captured so the body sees its lexical
               surroundings. Defined in the *current* scope, so a nested `fn`
               becomes a local closure rather than a global. */
            Value fn = val_function(s, env);
            env_define(env, s->fn_def.name, fn);
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
            gc_protect(obj);
            Value v = eval_expr(interp, s->field_assign.value, env);
            gc_pop(1);
            if (s->field_assign.op[0]) {            /* compound: p.x op= v */
                Value cur;
                struct_get(obj.strukt, s->field_assign.name, &cur);  /* field exists (checked) */
                v = eval_binary_op(interp, s->field_assign.op, cur, v, s->line);
            }
            struct_set(obj.strukt, s->field_assign.name, v);
            break;
        }

        case STMT_INDEX_ASSIGN: {
            /* Protect object and key across the value (and operator) evaluation:
               a map key must outlive the eval that may trigger a collection. */
            Value obj = gc_protect(eval_expr(interp, s->index_assign.object, env));
            Value key = gc_protect(eval_expr(interp, s->index_assign.index,  env));
            Value v   = gc_protect(eval_expr(interp, s->index_assign.value,  env));
            if (interp->had_error) { gc_pop(3); break; }

            if (obj.type == VAL_LIST) {
                if (key.type != VAL_INT) {
                    runtime_error(interp, s->line, "List index must be an integer");
                    gc_pop(3); break;
                }
                int idx = (int)key.integer;
                if (idx < 0 || idx >= obj.list->len) {
                    runtime_error(interp, s->line, "List index out of range");
                    gc_pop(3); break;
                }
                if (s->index_assign.op[0])              /* compound: list[i] op= v */
                    v = eval_binary_op(interp, s->index_assign.op,
                                       list_get(obj.list, idx), v, s->line);
                list_set(obj.list, idx, v);
            } else if (obj.type == VAL_MAP) {
                if (s->index_assign.op[0]) {            /* compound: m[k] op= v */
                    Value cur;
                    if (!map_get(obj.map, key, &cur)) {
                        runtime_error(interp, s->line, "Compound assignment to a missing map key");
                        gc_pop(3); break;
                    }
                    v = eval_binary_op(interp, s->index_assign.op, cur, v, s->line);
                }
                map_set(obj.map, key, v);
            } else {
                runtime_error(interp, s->line, "Cannot index-assign into this type");
            }
            gc_pop(3);
            break;
        }

        case STMT_IMPORT: {
            /* Load (or reuse) the module and bind its namespace to the alias.
               The module's scope is wrapped in a VAL_MODULE; member access goes
               through EXPR_FIELD / EXPR_METHOD. */
            Env *menv = load_module(interp, s->import.path, s->line);
            if (interp->had_error || !menv) break;
            Value mod = (Value){ VAL_MODULE, .menv = menv };
            if (!env_set(env, s->import.alias, mod))
                env_define(env, s->import.alias, mod);
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

/* ── Module loading ───────────────────────────────────────────────── */

/* Read an entire file into a fresh null-terminated buffer (NULL on failure). */
static char *read_source_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)size, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/*
 * Load a module by path and return its namespace environment, or NULL on error.
 * Modules are loaded once and cached by path: a repeat import reuses the same
 * namespace, and a still-loading entry (env == NULL) signals a circular import.
 * The module's structs/interfaces register into the shared global registry; its
 * top-level statements run into a fresh scope that becomes the namespace. The
 * parsed Program and source buffer are owned by the interpreter and freed at the
 * end (function values point into the AST).
 */
static Env *load_module(Interpreter *interp, const char *path, int line) {
    for (int i = 0; i < interp->module_count; i++) {
        if (strcmp(interp->module_paths[i], path) == 0) {
            if (interp->module_envs[i] == NULL) {
                runtime_error(interp, line, "Circular import");
                fprintf(stderr, "  '%s'\n", path);
                return NULL;
            }
            return interp->module_envs[i];
        }
    }
    if (interp->module_count >= MAX_MODULES) {
        runtime_error(interp, line, "Too many imported modules");
        return NULL;
    }

    char *src = read_source_file(path);
    if (!src) {
        runtime_error(interp, line, "Could not open module");
        fprintf(stderr, "  '%s'\n", path);
        return NULL;
    }

    int idx = interp->module_count++;
    interp->module_paths[idx]   = strdup(path);
    interp->module_envs[idx]    = NULL;     /* loading — guards against cycles */
    interp->module_sources[idx] = src;
    interp->module_progs[idx]   = NULL;

    Lexer  lx; lexer_init(&lx, src);
    Parser ps; parser_init(&ps, &lx);
    Program *prog = malloc(sizeof(Program));
    *prog = parser_parse(&ps);
    interp->module_progs[idx] = prog;
    if (ps.had_error) {
        runtime_error(interp, line, "Failed to parse module");
        fprintf(stderr, "  '%s'\n", path);
        return NULL;
    }

    interpreter_prepare(interp, prog);       /* register the module's types */
    if (interp->had_error) return NULL;

    Env *menv = env_new(interp->globals);
    for (int i = 0; i < prog->stmts.len; i++) {
        Stmt *s = prog->stmts.data[i];
        if (s->kind == STMT_STRUCT_DEF || s->kind == STMT_INTERFACE_DEF ||
            s->kind == STMT_IMPL       || s->kind == STMT_IMPLEMENT)
            continue;
        exec_stmt(interp, s, menv);
        if (interp->had_error) return NULL;
    }
    interp->module_envs[idx] = menv;
    return menv;
}

/* ── Entry points ─────────────────────────────────────────────────── */
void interpreter_init(Interpreter *interp) {
    interp->globals         = env_new(NULL);
    interp->had_error       = 0;
    interp->struct_count    = 0;
    interp->interface_count = 0;
    interp->module_count    = 0;
}

void interpreter_free(Interpreter *interp) {
    /* Drop every module namespace and the globals as GC roots, then collect:
       the final sweep reclaims all environments, closures, and other objects. */
    for (int i = 0; i < interp->module_count; i++)
        if (interp->module_envs[i]) env_free(interp->module_envs[i]);
    env_free(interp->globals);
    gc_collect();

    /* Module ASTs and sources outlive the GC objects (function values point into
       them), so free them only now that every closure is gone. */
    for (int i = 0; i < interp->module_count; i++) {
        if (interp->module_progs[i]) {
            program_free(interp->module_progs[i]);
            free(interp->module_progs[i]);
        }
        free(interp->module_sources[i]);
        free(interp->module_paths[i]);
    }
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
