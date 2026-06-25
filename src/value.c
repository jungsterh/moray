#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "value.h"
#include "env.h"   /* for env_gc_mark_roots — the variable root set */

/* ── Garbage collector ─────────────────────────────────────────────────
 *
 * A simple mark-and-sweep collector. Every heap object is threaded on the
 * intrusive list `g_objects` at allocation; a collection marks everything
 * reachable from the roots (live scopes + the protect stack) and frees the
 * rest. See value.h for the rationale and the trigger policy.
 */

static GCHeader *g_objects = NULL;   /* all live heap objects                 */
static long      g_obj_count = 0;    /* objects currently tracked             */
static long      g_threshold = 256;  /* collect once this many objects exist  */

/* Protect stack — pins in-flight temporaries that no scope references yet. */
static Value *g_protect      = NULL;
static int    g_protect_len  = 0;
static int    g_protect_cap  = 0;

/* Recover the embedded/prefixed header from any heap object. The header is the
   first member of list/map/struct, and a prefix in front of string data. */
static GCHeader *list_header(MorayList *l)   { return &l->gc; }
static GCHeader *map_header(MorayMap *m)     { return &m->gc; }
static GCHeader *struct_header(MorayStruct *s){ return &s->gc; }
static GCHeader *string_header(char *s)      { return (GCHeader *)s - 1; }

/* Thread a freshly allocated object onto the all-objects list. */
static void gc_register(GCHeader *h, GCKind kind) {
    h->kind   = (unsigned char)kind;
    h->mark   = 0;
    h->next   = g_objects;
    g_objects = h;
    g_obj_count++;
}

char *gc_new_string_buffer(size_t nbytes) {
    GCHeader *h = calloc(1, sizeof(GCHeader) + nbytes);
    gc_register(h, GC_STRING);
    return (char *)(h + 1);
}

/* Hand an environment to the collector. Env's first member is a GCHeader (see
   env.h), so &e->gc is the object's header. */
void gc_register_env(Env *e) {
    gc_register(&e->gc, GC_ENV);
}

Value gc_protect(Value v) {
    if (g_protect_len >= g_protect_cap) {
        g_protect_cap = g_protect_cap == 0 ? 64 : g_protect_cap * 2;
        g_protect     = realloc(g_protect, g_protect_cap * sizeof(Value));
    }
    g_protect[g_protect_len++] = v;
    return v;
}

void gc_pop(int n) {
    g_protect_len -= n;
    if (g_protect_len < 0) g_protect_len = 0;
}

/* ── Mark ─────────────────────────────────────────────────────────── */

static void gc_mark_map_contents(MorayMap *m) {
    for (int i = 0; i < m->len; i++) {
        gc_mark_value(m->pairs[i].key);    /* string keys are GC-managed */
        gc_mark_value(m->pairs[i].value);
    }
}

void gc_mark_value(Value v) {
    switch (v.type) {
        case VAL_STRING: {
            string_header(v.string)->mark = 1;   /* leaf */
            break;
        }
        case VAL_LIST: {
            GCHeader *h = list_header(v.list);
            if (h->mark) return;                  /* already visited (cycles) */
            h->mark = 1;
            for (int i = 0; i < v.list->len; i++)
                gc_mark_value(v.list->data[i]);
            break;
        }
        case VAL_MAP: {
            GCHeader *h = map_header(v.map);
            if (h->mark) return;
            h->mark = 1;
            gc_mark_map_contents(v.map);
            break;
        }
        case VAL_STRUCT: {
            GCHeader *h = struct_header(v.strukt);
            if (h->mark) return;
            h->mark = 1;
            /* The fields map is its own tracked object, reachable only through
               this struct — mark it and its contents so both survive. */
            map_header(v.strukt->fields)->mark = 1;
            gc_mark_map_contents(v.strukt->fields);
            break;
        }
        case VAL_FUNCTION: {
            GCHeader *h = &v.func->gc;
            if (h->mark) return;
            h->mark = 1;
            env_gc_mark(v.func->closure);   /* keep the captured scope alive */
            break;
        }
        case VAL_MODULE:
            env_gc_mark(v.menv);             /* the module's namespace scope */
            break;
        default:
            break;   /* int/float/bool/null hold no heap memory */
    }
}

/* ── Sweep ────────────────────────────────────────────────────────── */

static void gc_free_object(GCHeader *h) {
    switch ((GCKind)h->kind) {
        case GC_STRING:
            free(h);   /* header is the allocation base for strings */
            break;
        case GC_LIST: {
            MorayList *l = (MorayList *)h;
#ifdef GC_TORTURE
            memset(l->data, 0xDD, (size_t)l->len * sizeof(Value));
#endif
            free(l->data);
            free(l);
            break;
        }
        case GC_MAP: {
            MorayMap *m = (MorayMap *)h;
            /* Keys are GC-managed Values (string keys are tracked strings), so
               they are reclaimed by their own sweep — do not free them here. */
            free(m->pairs);
            free(m);
            break;
        }
        case GC_STRUCT: {
            MorayStruct *s = (MorayStruct *)h;
            free(s->type_name);
            /* s->fields is a separately tracked object; it is swept on its own
               pass once it is no longer reachable. */
            free(s);
            break;
        }
        case GC_FUNC:
            /* def is AST-owned; closure is its own tracked object. */
            free(h);
            break;
        case GC_ENV:
            /* Env's variable values are tracked objects swept on their own; its
               name strings are private and freed here. */
            env_gc_free((Env *)h);
            break;
    }
    g_obj_count--;
}

void gc_collect(void) {
    /* Mark from every root. */
    env_gc_mark_roots();
    for (int i = 0; i < g_protect_len; i++)
        gc_mark_value(g_protect[i]);

    /* Sweep: free unmarked objects, clear marks on survivors. */
    GCHeader **link = &g_objects;
    while (*link) {
        GCHeader *h = *link;
        if (h->mark) {
            h->mark = 0;
            link    = &h->next;
        } else {
            *link = h->next;       /* unlink before freeing */
            gc_free_object(h);
        }
    }

    /* Grow the threshold so collection cost stays proportional to live data. */
    g_threshold = g_obj_count * 2 < 256 ? 256 : g_obj_count * 2;
}

void gc_maybe_collect(void) {
#ifdef GC_TORTURE
    /* Torture mode: collect at every scope teardown so any missing protection
       surfaces immediately. Enable with -DGC_TORTURE for testing. */
    gc_collect();
#else
    if (g_obj_count >= g_threshold)
        gc_collect();
#endif
}

/* ── String ───────────────────────────────────────────────────────── */

Value val_string(const char *ptr, int len) {
    char *s = gc_new_string_buffer(len + 1);
    memcpy(s, ptr, len);
    s[len] = '\0';
    return (Value){ VAL_STRING, .string = s };
}

/* ── List ─────────────────────────────────────────────────────────── */
/*
 * stdlib equivalent: there is none in standard C.
 * In C++ this would be std::vector<Value>.
 * We're manually managing a growable heap array — same strategy as vec.h
 * but for Value elements stored on the heap so the list can be shared.
 */

Value val_list_empty(void) {
    MorayList *l = calloc(1, sizeof(MorayList));
    gc_register(&l->gc, GC_LIST);
    return (Value){ VAL_LIST, .list = l };
}

void list_push(MorayList *l, Value v) {
    if (l->len >= l->cap) {
        l->cap  = l->cap == 0 ? 8 : l->cap * 2;
        l->data = realloc(l->data, l->cap * sizeof(Value));
    }
    l->data[l->len++] = v;
}

Value list_get(MorayList *l, int index) {
    if (index < 0 || index >= l->len) return val_null();
    return l->data[index];
}

void list_set(MorayList *l, int index, Value v) {
    if (index < 0 || index >= l->len) return;
    value_free(l->data[index]);
    l->data[index] = v;
}

/* ── Map ──────────────────────────────────────────────────────────── */
/*
 * stdlib equivalent: there is none in standard C.
 * In C++ this would be std::unordered_map<std::string, Value>.
 * We're using a simple linear-search array of key/value pairs.
 * Fast enough for small maps; a real hash table would replace this
 * for production use.
 */

/* Allocate a GC-tracked empty map. Shared by val_map_empty and val_struct. */
static MorayMap *gc_new_map(void) {
    MorayMap *m = calloc(1, sizeof(MorayMap));
    gc_register(&m->gc, GC_MAP);
    return m;
}

Value val_map_empty(void) {
    return (Value){ VAL_MAP, .map = gc_new_map() };
}

/* Key equality mirrors the `==` operator: scalars compare by value, and
   reference types (list/map/struct) by identity — the same heap object. */
static int map_key_equal(Value a, Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case VAL_INT:    return a.integer  == b.integer;
        case VAL_FLOAT:  return a.floating == b.floating;
        case VAL_BOOL:   return a.boolean  == b.boolean;
        case VAL_STRING: return strcmp(a.string, b.string) == 0;
        case VAL_NULL:   return 1;
        case VAL_LIST:   return a.list   == b.list;
        case VAL_MAP:    return a.map    == b.map;
        case VAL_STRUCT: return a.strukt == b.strukt;
        case VAL_FUNCTION: return a.func == b.func;
        case VAL_MODULE:   return a.menv == b.menv;
    }
    return 0;
}

/* Grow the pair array if full, then return the slot index for a new pair. */
static int map_reserve_slot(MorayMap *m) {
    if (m->len >= m->cap) {
        m->cap   = m->cap == 0 ? 8 : m->cap * 2;
        m->pairs = realloc(m->pairs, m->cap * sizeof(MapPair));
    }
    return m->len++;
}

void map_set(MorayMap *m, Value key, Value v) {
    for (int i = 0; i < m->len; i++) {
        if (map_key_equal(m->pairs[i].key, key)) {
            m->pairs[i].value = v;   /* update existing */
            return;
        }
    }
    int i = map_reserve_slot(m);
    m->pairs[i].key   = key;
    m->pairs[i].value = v;
}

int map_get(MorayMap *m, Value key, Value *out) {
    for (int i = 0; i < m->len; i++)
        if (map_key_equal(m->pairs[i].key, key)) { *out = m->pairs[i].value; return 1; }
    return 0;
}

int map_has(MorayMap *m, Value key) {
    for (int i = 0; i < m->len; i++)
        if (map_key_equal(m->pairs[i].key, key)) return 1;
    return 0;
}

/* ── String-keyed helpers (used for struct fields) ────────────────────
 * Structs store their fields in a MorayMap keyed by the field name. These
 * compare against a C string without allocating on lookup; only inserting a
 * brand-new field allocates a GC string for the key. */

static int map_get_str(MorayMap *m, const char *key, Value *out) {
    for (int i = 0; i < m->len; i++)
        if (m->pairs[i].key.type == VAL_STRING &&
            strcmp(m->pairs[i].key.string, key) == 0) { *out = m->pairs[i].value; return 1; }
    return 0;
}

static int map_has_str(MorayMap *m, const char *key) {
    Value out;
    return map_get_str(m, key, &out);
}

static void map_set_str(MorayMap *m, const char *key, Value v) {
    for (int i = 0; i < m->len; i++)
        if (m->pairs[i].key.type == VAL_STRING &&
            strcmp(m->pairs[i].key.string, key) == 0) { m->pairs[i].value = v; return; }
    int i = map_reserve_slot(m);
    m->pairs[i].key   = val_string(key, (int)strlen(key));
    m->pairs[i].value = v;
}

/* ── Struct ───────────────────────────────────────────────────────── */
/*
 * A struct instance is a named bag of fields. We reuse the map machinery for
 * the fields, so struct_set/get/has are thin wrappers. Shared by reference.
 */

Value val_struct(const char *type_name) {
    MorayStruct *s = calloc(1, sizeof(MorayStruct));
    gc_register(&s->gc, GC_STRUCT);
    s->type_name   = strdup(type_name);
    s->fields      = gc_new_map();
    return (Value){ VAL_STRUCT, .strukt = s };
}

/* ── Function (closure) ───────────────────────────────────────────── */

Value val_function(Stmt *def, Env *closure) {
    MorayFunc *f = calloc(1, sizeof(MorayFunc));
    gc_register(&f->gc, GC_FUNC);
    f->def     = def;
    f->closure = closure;
    return (Value){ VAL_FUNCTION, .func = f };
}

void struct_set(MorayStruct *s, const char *field, Value v) { map_set_str(s->fields, field, v); }
int  struct_get(MorayStruct *s, const char *field, Value *out) { return map_get_str(s->fields, field, out); }
int  struct_has(MorayStruct *s, const char *field) { return map_has_str(s->fields, field); }

/* ── Shared free / print ──────────────────────────────────────────── */

void value_free(Value v) {
    /*
     * Still a no-op — and now intentionally so for a different reason. Every
     * heap-backed value is shared by handle, so a slot losing its reference
     * (a scope ending, a variable reassigned, a list/map element overwritten)
     * must NOT free the object: other holders may still reach it. Reclamation
     * is the garbage collector's job. Dropping the reference is enough; the next
     * collection (gc_maybe_collect, run when a scope is discarded) frees the
     * object if nothing else can reach it. Kept so the existing "release this
     * slot" call sites remain explicit.
     */
    (void)v;
}

void value_print(Value v) {
    switch (v.type) {
        case VAL_INT:    printf("%ld", v.integer);  break;
        case VAL_FLOAT:  printf("%g",  v.floating); break;
        case VAL_STRING: printf("%s",  v.string);   break;
        case VAL_BOOL:   printf("%s",  v.boolean ? "true" : "false"); break;
        case VAL_NULL:   printf("null"); break;
        case VAL_LIST:
            printf("[");
            for (int i = 0; i < v.list->len; i++) {
                if (i > 0) printf(", ");
                value_print(v.list->data[i]);
            }
            printf("]");
            break;
        case VAL_MAP:
            printf("{");
            for (int i = 0; i < v.map->len; i++) {
                if (i > 0) printf(", ");
                Value k = v.map->pairs[i].key;
                if (k.type == VAL_STRING) printf("\"%s\"", k.string);
                else                      value_print(k);
                printf(": ");
                value_print(v.map->pairs[i].value);
            }
            printf("}");
            break;
        case VAL_STRUCT:
            printf("%s(", v.strukt->type_name);
            for (int i = 0; i < v.strukt->fields->len; i++) {
                if (i > 0) printf(", ");
                printf("%s: ", v.strukt->fields->pairs[i].key.string);  /* field keys are strings */
                value_print(v.strukt->fields->pairs[i].value);
            }
            printf(")");
            break;
        case VAL_FUNCTION: printf("<function>"); break;
        case VAL_MODULE:   printf("<module>");   break;
    }
}

const char *value_type_name(ValueType t) {
    switch (t) {
        case VAL_INT:    return "int";
        case VAL_FLOAT:  return "float";
        case VAL_STRING: return "string";
        case VAL_BOOL:   return "bool";
        case VAL_NULL:   return "null";
        case VAL_LIST:   return "list";
        case VAL_MAP:    return "map";
        case VAL_STRUCT: return "struct";   /* type() reports the instance's type name instead */
        case VAL_FUNCTION: return "function";
        case VAL_MODULE:   return "module";
    }
    return "?";
}
