#ifndef MORAY_ENV_H
#define MORAY_ENV_H

#include "value.h"

#define ENV_MAX_VARS 256

typedef struct Env Env;

struct Env {
    GCHeader gc;      /* must stay first — the collector owns Env lifetimes   */
    char  *names[ENV_MAX_VARS];
    Value  values[ENV_MAX_VARS];
    int    count;
    Env   *parent;    /* enclosing scope — NULL for global */
    Env   *gc_link;   /* next live scope in the GC root registry             */
};

Env  *env_new(Env *parent);   /* allocate a new scope */
void  env_free(Env *e);       /* free scope (not its parent) */

/* Returns 1 on success, 0 if the scope is full */
int   env_define(Env *e, const char *name, Value val);

/* Walks up the scope chain. Returns 1 and sets *out if found */
int   env_get(Env *e, const char *name, Value *out);

/* Walks up the chain and updates existing variable. Returns 1 if found */
int   env_set(Env *e, const char *name, Value val);

/* Mark every value held by every live scope. Called by the collector to root
   the variable graph. */
void  env_gc_mark_roots(void);

/* Mark this environment and everything it can reach (its values and its parent
   chain). Called by the collector when a closure/module keeps a scope alive. */
void  env_gc_mark(Env *e);

/* Free an environment's private memory (its name strings). Called by the
   collector's sweep when the env is no longer reachable; the values it held are
   tracked objects reclaimed on their own. */
void  env_gc_free(Env *e);

#endif
