#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "value.h"

/* ── String ───────────────────────────────────────────────────────── */

Value val_string(const char *ptr, int len) {
    char *s = malloc(len + 1);
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

Value val_map_empty(void) {
    MorayMap *m = calloc(1, sizeof(MorayMap));
    return (Value){ VAL_MAP, .map = m };
}

void map_set(MorayMap *m, const char *key, Value v) {
    /* update existing key if found */
    for (int i = 0; i < m->len; i++) {
        if (strcmp(m->pairs[i].key, key) == 0) {
            value_free(m->pairs[i].value);
            m->pairs[i].value = v;
            return;
        }
    }
    /* insert new key */
    if (m->len >= m->cap) {
        m->cap   = m->cap == 0 ? 8 : m->cap * 2;
        m->pairs = realloc(m->pairs, m->cap * sizeof(MapPair));
    }
    m->pairs[m->len].key   = strdup(key);
    m->pairs[m->len].value = v;
    m->len++;
}

int map_get(MorayMap *m, const char *key, Value *out) {
    for (int i = 0; i < m->len; i++) {
        if (strcmp(m->pairs[i].key, key) == 0) {
            *out = m->pairs[i].value;
            return 1;
        }
    }
    return 0;
}

int map_has(MorayMap *m, const char *key) {
    for (int i = 0; i < m->len; i++)
        if (strcmp(m->pairs[i].key, key) == 0) return 1;
    return 0;
}

/* ── Struct ───────────────────────────────────────────────────────── */
/*
 * A struct instance is a named bag of fields. We reuse the map machinery for
 * the fields, so struct_set/get/has are thin wrappers. Shared by reference.
 */

Value val_struct(const char *type_name) {
    MorayStruct *s = calloc(1, sizeof(MorayStruct));
    s->type_name   = strdup(type_name);
    s->fields      = calloc(1, sizeof(MorayMap));
    return (Value){ VAL_STRUCT, .strukt = s };
}

void struct_set(MorayStruct *s, const char *field, Value v) { map_set(s->fields, field, v); }
int  struct_get(MorayStruct *s, const char *field, Value *out) { return map_get(s->fields, field, out); }
int  struct_has(MorayStruct *s, const char *field) { return map_has(s->fields, field); }

/* ── Shared free / print ──────────────────────────────────────────── */

void value_free(Value v) {
    /*
     * Deliberately a no-op. Every heap-backed value (string, list, map, struct)
     * is shared by handle: bindings shallow-copy the Value, so the same heap
     * object is reachable from multiple variables, function/method arguments,
     * and `self` at once. With no ownership tracking or garbage collector,
     * freeing here when a scope ends would dangle the other holders and
     * double-free at program exit. So the interpreter reclaims nothing during
     * execution and lets the OS free everything on exit — sound and simple for
     * a tree-walker. (Kept as a function so call sites and the contract stay
     * explicit if a real memory strategy is added later.)
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
                printf("\"%s\": ", v.map->pairs[i].key);
                value_print(v.map->pairs[i].value);
            }
            printf("}");
            break;
        case VAL_STRUCT:
            printf("%s(", v.strukt->type_name);
            for (int i = 0; i < v.strukt->fields->len; i++) {
                if (i > 0) printf(", ");
                printf("%s: ", v.strukt->fields->pairs[i].key);
                value_print(v.strukt->fields->pairs[i].value);
            }
            printf(")");
            break;
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
    }
    return "?";
}
