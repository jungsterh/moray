#include <stdlib.h>
#include <string.h>
#include "env.h"

/* Registry of every live scope, used as the GC root set. A scope joins on
   creation and leaves on teardown, so the collector can reach every variable
   currently in play. */
static Env *g_live_envs = NULL;

void env_gc_mark_roots(void) {
    for (Env *e = g_live_envs; e != NULL; e = e->gc_link)
        env_gc_mark(e);
}

void env_gc_mark(Env *e) {
    if (!e || e->gc.mark) return;       /* stop on cycles / already visited */
    e->gc.mark = 1;
    for (int i = 0; i < e->count; i++)
        gc_mark_value(e->values[i]);
    env_gc_mark(e->parent);             /* the lexical chain stays reachable */
}

void env_gc_free(Env *e) {
    for (int i = 0; i < e->count; i++)
        free(e->names[i]);
    free(e);
}

Env *env_new(Env *parent) {
    Env *e      = calloc(1, sizeof(Env));
    gc_register_env(e);          /* the collector now owns this env's lifetime */
    e->parent   = parent;
    e->gc_link  = g_live_envs;   /* register as a live root */
    g_live_envs = e;
    return e;
}

void env_free(Env *e) {
    /* A scope is "discarded" only as a *root*: we unlink it from the live-root
       registry so its variables stop rooting the object graph. The Env itself is
       a GC object — if a closure or module still captures it, it stays alive and
       is reclaimed later; otherwise the collection below frees it. We must NOT
       free it here. */
    if (g_live_envs == e) {
        g_live_envs = e->gc_link;
    } else {
        for (Env *p = g_live_envs; p != NULL; p = p->gc_link) {
            if (p->gc_link == e) { p->gc_link = e->gc_link; break; }
        }
    }
    e->gc_link = NULL;

    gc_maybe_collect();
}

int env_define(Env *e, const char *name, Value val) {
    if (e->count >= ENV_MAX_VARS) return 0;
    e->names[e->count]  = strdup(name);
    e->values[e->count] = val;
    e->count++;
    return 1;
}

int env_get(Env *e, const char *name, Value *out) {
    /* search current scope first, then walk up to parent */
    for (Env *scope = e; scope != NULL; scope = scope->parent) {
        for (int i = 0; i < scope->count; i++) {
            if (strcmp(scope->names[i], name) == 0) {
                *out = scope->values[i];
                return 1;
            }
        }
    }
    return 0;
}

int env_set(Env *e, const char *name, Value val) {
    for (Env *scope = e; scope != NULL; scope = scope->parent) {
        for (int i = 0; i < scope->count; i++) {
            if (strcmp(scope->names[i], name) == 0) {
                value_free(scope->values[i]);
                scope->values[i] = val;
                return 1;
            }
        }
    }
    return 0;
}
