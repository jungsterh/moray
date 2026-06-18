#ifndef MORAY_VALUE_H
#define MORAY_VALUE_H

#include <stddef.h>

/*
 * A runtime value in Moray.
 *
 * Tagged union: the 'type' field tells you which union member is valid.
 * Reading the wrong member is undefined behavior in C — always check type first.
 */

typedef enum {
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_BOOL,
    VAL_NULL,
    VAL_LIST,
    VAL_MAP,
    VAL_STRUCT,
} ValueType;

/*
 * Forward declarations. struct Value only ever holds *pointers* to the list,
 * map, and struct instance, so it can be defined before they are. MapPair, on
 * the other hand, embeds a Value by value, so it must come after struct Value
 * is complete.
 */
typedef struct Value       Value;
typedef struct MorayList   MorayList;
typedef struct MorayMap    MorayMap;
typedef struct MorayStruct MorayStruct;

struct Value {
    ValueType type;
    union {
        long          integer;
        double        floating;
        char         *string;    /* heap-allocated, null-terminated */
        int           boolean;   /* 1 or 0 */
        MorayList    *list;      /* heap-allocated, shared */
        MorayMap     *map;       /* heap-allocated, shared */
        MorayStruct  *strukt;    /* heap-allocated, shared ('struct' is a keyword) */
    };
};

/* Heap-allocated list shared by reference */
struct MorayList {
    Value *data;
    int    len;
    int    cap;
};

/* Heap-allocated map entry */
typedef struct {
    char  *key;    /* heap-allocated string */
    Value  value;
} MapPair;

struct MorayMap {
    MapPair *pairs;
    int      len;
    int      cap;
};

/*
 * A struct instance. The fields are stored in a map (name -> Value); the type
 * name identifies which struct definition it belongs to, used for method
 * dispatch and type() reporting. Shared by reference, like lists and maps.
 */
struct MorayStruct {
    char     *type_name;   /* heap-allocated */
    MorayMap *fields;      /* name -> Value */
};

/* Constructors — one per type so you never set the tag by hand */
static inline Value val_int(long v)         { return (Value){ VAL_INT,    .integer  = v }; }
static inline Value val_float(double v)     { return (Value){ VAL_FLOAT,  .floating = v }; }
static inline Value val_bool(int v)         { return (Value){ VAL_BOOL,   .boolean  = v }; }
static inline Value val_null(void)          { return (Value){ VAL_NULL,   .integer  = 0 }; }
Value val_string(const char *ptr, int len);
Value val_list_empty(void);
Value val_map_empty(void);
Value val_struct(const char *type_name);   /* empty instance, fields added via struct_set */

/* List operations */
void  list_push(MorayList *l, Value v);
Value list_get(MorayList *l, int index);   /* returns null if out of bounds */
void  list_set(MorayList *l, int index, Value v);

/* Map operations */
void  map_set(MorayMap *m, const char *key, Value v);
int   map_get(MorayMap *m, const char *key, Value *out);  /* 1 if found */
int   map_has(MorayMap *m, const char *key);

/* Struct operations (thin wrappers over the field map) */
void  struct_set(MorayStruct *s, const char *field, Value v);
int   struct_get(MorayStruct *s, const char *field, Value *out);  /* 1 if found */
int   struct_has(MorayStruct *s, const char *field);

void        value_free(Value v);
void        value_print(Value v);
const char *value_type_name(ValueType t);

#endif
