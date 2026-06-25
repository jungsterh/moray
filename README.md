# Moray

Moray is a small, optionally-typed scripting language with a tree-walking
interpreter written in C. Programs are stored in `.my` files and run directly —
there is no separate compile step. Type annotations are TypeScript-style and
optional; values are dynamically typed at runtime.

```my
fn factorial(n: int) {
    if n <= 1 {
        return 1
    }
    return n * factorial(n - 1)
}

print(factorial(6))   # 720
```

---

## Table of contents

- [Getting started](#getting-started)
- [Language guide](#language-guide)
  - [Comments](#comments)
  - [Types](#types)
  - [Variables](#variables)
  - [Operators](#operators)
  - [Strings](#strings)
  - [Control flow](#control-flow)
  - [Functions](#functions)
  - [Lists & maps](#lists--maps)
  - [Structs & interfaces](#structs--interfaces)
  - [Truthiness](#truthiness)
  - [Scope](#scope)
  - [Memory management](#memory-management)
- [Built-in functions](#built-in-functions)
- [Grammar reference](#grammar-reference)
- [Language limitations](#language-limitations)
- [Project layout](#project-layout)

---

## Getting started

### Build

You need a C11 compiler (`gcc` or `clang`) and `make`.

```sh
make            # produces ./moray
```

### Run a program

```sh
./moray examples/sample.my     # run a specific file
./moray                        # no argument → runs examples/sample.my
```

Or through the Makefile:

```sh
make run                       # builds, then runs examples/sample.my
make run FILE=path/to/prog.my  # builds, then runs the given file
```

A complete example program lives in [examples/sample.my](examples/sample.my).

---

## Language guide

### Comments

Comments start with `#` and run to the end of the line.

```my
# this is a comment
x = 1   # trailing comments work too
```

### Types

| Type     | Keyword  | Literals / construction        | Semantics |
|----------|----------|--------------------------------|-----------|
| Integer  | `int`    | `42`, `-7`, `0`                | value     |
| Float    | `float`  | `3.14`, `0.5`                  | value     |
| String   | `string` | `"hi"`, `'hi'`                 | value     |
| Boolean  | `bool`   | `true`, `false`                | value     |
| List     | `list`   | `[1, 2, 3]`                    | reference |
| Map      | `map`    | `{"a": 1}`, `{1: "a"}`         | reference |
| Struct   | *name*   | `Point(3, 4)` (see below)      | reference |
| Any      | `any`    | — (annotation only)            | —         |
| Null     | —        | `null`                         | value     |

Numeric literals without a fractional part are stored as `int`; literals with a
`.` are `float`. There is no `null` type keyword — `null` is only a value. `any`
is an annotation that matches any value (it opts out of type checking); it is not
a runtime type that `type()` ever reports.

**Value vs. reference.** Numbers, booleans, and strings are values. Lists, maps,
and structs are *references*: assigning or passing one shares the same
underlying object rather than copying it (see [Structs](#structs--interfaces)).

### Variables

Declarations are TypeScript-style. A variable is created either with an explicit
type, `name: type = expression`, or by **inference** — a bare `name = expression`
declares the variable if it doesn't exist yet:

```my
count: int   = 10        # explicit type
ratio: float = 1.5
name = "Moray"           # inferred (declare-or-assign)
ready = true
```

A bare `name = expression` is **declare-or-assign**: if `name` already exists
(anywhere up the scope chain) it is updated; otherwise it is declared in the
current scope. To shadow a name in an inner scope, give it an explicit type
(`name: type = …`).

```my
count = count + 1     # updates the existing variable
total = 0             # declares `total` (inferred)
```

> Trade-off: because a bare assignment can declare, a mis-typed name silently
> creates a new variable rather than erroring.

**Compound assignment** updates an existing variable in place with an arithmetic
operator, and `++` / `--` add or subtract one. These work on plain variables and
on struct fields ([`p.x`](#structs--interfaces)); the variable must already
exist (unlike plain `=`):

```my
n = 10
n += 5        # 15   (also -=, *=, /=, %=)
n *= 2        # 30
n--           # 29

s = "Mor"
s += "ay"     # "Moray"   (+= concatenates strings, like +)
```

`x += e` is exactly `x = x + e`, so the same type rules apply (e.g. `/=` yields a
float, `%=` requires integers). They are statements, not expressions — there is
no `y = x++`.

**Type annotations are checked at the declaration.** `c: string = 2` is a runtime
error; `c: any = 2` is fine (`any` matches anything). Collection annotations are
checked recursively (see [Lists & maps](#lists--maps)). Checking happens only at
the declaration site — a later bare reassignment (`c = 2`) is not re-checked, as
Moray does not track a type per variable.

### Operators

From lowest to highest precedence:

| Category    | Operators            | Notes                                   |
|-------------|----------------------|-----------------------------------------|
| Logical or  | `\|\|`               |                                         |
| Logical and | `&&`                 |                                         |
| Equality    | `==`  `!=`           | Works across all types                  |
| Comparison  | `<`  `<=`  `>`  `>=` | Numbers only                            |
| Term        | `+`  `-`             | `+` also concatenates strings           |
| Factor      | `*`  `/`  `%`        | `/` always yields a float; `%` ints only|
| Unary       | `-`  `!`             | `!` is logical negation                 |

Arithmetic rules:

- `int op int` stays `int` (except `/`, which is always `float`).
- If either operand is `float`, the result is promoted to `float`.
- `/` by zero and `%` by zero are runtime errors.
- `%` requires both operands to be integers.

```my
print(7 / 2)      # 3.5   (always float)
print(7 % 2)      # 1
print(2 + 3 * 4)  # 14    (precedence)
print(-5)         # -5
print(!true)      # false
print(x > 0 && x < 10)   # logical and
print(x < 0 || x > 10)   # logical or
```

Equality compares value **and** type: values of different types are never
equal (`1 == 1.0` is `false` because one is `int` and one is `float`).

### Strings

Strings use single `'…'` or double `"…"` quotes (interchangeable). Concatenate
with `+`:

```my
greeting = "Hello, " + 'Moray!'
print(greeting)            # Hello, Moray!
```

Backslash escape sequences are supported in both quote styles: `\n` (newline),
`\t` (tab), `\r` (carriage return), `\0` (NUL), `\\` (backslash), `\'`, and
`\"`. A backslash followed by any other character is left verbatim, backslash
included (`"\q"` is the two characters `\q`). Strings may still span multiple
raw lines.

```my
print("col1\tcol2\nrow value")   # tab, then a newline
print('it\'s here')              # it's here
```

### Control flow

**`if` / `else`** — the condition is *not* parenthesized; braces are required.

```my
if x > 10 {
    print("big")
} else {
    print("small")
}
```

Chain conditions with **`else if`**:

```my
if x > 10 {
    print("big")
} else if x > 5 {
    print("medium")
} else {
    print("small")
}
```

**`while`** — loops while a condition holds:

```my
i = 0
while i < 5 {
    print(i)
    i = i + 1
}
```

**`for`** comes in two styles. The C-style form has three clauses —
initializer, condition, update — separated by `;` (no parentheses); any clause
may be omitted:

```my
for i = 0; i < 5; i = i + 1 {
    print(i)
}
```

The Python-style form iterates a list with `in`. Combine it with the
[`range`](#built-in-functions) built-in to count, or iterate any list directly:

```my
for i in range(5) {        # 0, 1, 2, 3, 4
    print(i)
}
for i in range(2, 5) {     # 2, 3, 4
    print(i)
}

words = ["a", "b", "c"]
for w in words {
    print(w)               # a, b, c
}
```

**`break`** exits the nearest enclosing loop; **`continue`** skips to its next
iteration. Both work in either loop style and only affect the innermost loop;
using them outside a loop is a runtime error.

```my
for i in range(100) {
    if i % 2 != 0 {
        continue           # skip odd numbers
    }
    if i > 10 {
        break              # stop once past 10
    }
    print(i)               # 0, 2, 4, 6, 8, 10
}
```

### Functions

Define with `fn`, annotate parameters `name: type` (the annotation is optional),
and use `return`:

```my
fn add(a: int, b: int) {
    return a + b
}

print(add(2, 3))   # 5
```

- A bare `return` (no expression) returns `null`.
- Falling off the end of a function also returns `null`.
- Recursion is supported (see `factorial` above).

Arguments may be passed positionally or by name, with named arguments after the
positional ones (as in Python):

```my
fn make(a: int, b: int) { return a + b }

make(1, 2)            # positional
make(1, b = 2)        # last argument named
make(a = 1, b = 2)    # all named
```

**Functions are first-class closures.** A function is a value you can store in a
variable, pass as an argument, return from another function, and call later. A
nested `fn` captures the scope it was defined in — including local variables —
and keeps it alive for as long as the function is reachable:

```my
fn make_counter() {
    count = 0
    fn inc() {
        count += 1        # mutates the captured `count`
        return count
    }
    return inc
}

c = make_counter()
print(c())   # 1
print(c())   # 2          (state persists between calls)

fn apply(f, x) { return f(x) }
fn double(n)  { return n * 2 }
print(apply(double, 21))   # 42

# The result of a call is itself callable:
fn adder(n) {
    fn go(x) { return x + n }
    return go
}
print(adder(10)(5))        # 15
```

Each call to `make_counter` produces an independent capture, so two counters
don't share state. `type(c)` reports `function`.

### Modules

Split code across files and pull one in with `import "<path>" as <name>`. The
path is resolved relative to the current working directory. The imported file
runs once (subsequent imports of the same path reuse it), and its top-level
**functions and values** become available through the namespace:

```my
# mathmod.my
pi = 3.14159
fn square(n) { return n * n }
fn circle_area(r) { return pi * square(r) }
struct Point { x: int  y: int }
```

```my
# main.my
import "mathmod.my" as math

print(math.square(5))        # 25
print(math.pi)               # 3.14159
print(math.circle_area(2))   # 12.56636
p = math.Point(3, 4)         # construct a module's struct type
```

Notes and limits:

- Member access uses `.`: `math.pi` reads a value, `math.square(5)` calls a
  function. `type(math)` reports `module`.
- A module's functions resolve names against *their own* module scope, so a
  module is self-contained (it can call its own helpers without the importer
  needing them).
- **Type names are global.** A `struct`/`interface` defined in any module
  registers under its bare name everywhere, so `Point(3, 4)` and
  `math.Point(3, 4)` both work — but two modules can't each define a `Point`.
- Circular imports are detected and reported as an error.

### Lists & maps

Lists are ordered and indexed from `0`; maps associate **keys of any type** with
values. Both are reference types and, by default, can hold values of any type.

Map keys compare exactly like the `==` operator: scalars (int/float/string/bool)
by value, and reference types (list/map/struct) by **identity** — the same heap
object. So a struct instance or list works as a key, but a *different* list with
the same contents is a different key:

```my
k = [1, 2]
m = {k: "found"}
print(m[k])        # found
print(m[[1, 2]])   # null   (a fresh list — different object)
```

**Typed collections.** Optionally declare the element type of a list, or the
key and value types of a map, with `list<T>` and `map<K, V>`. Like all
annotations these are **enforced at the declaration**: the bound value is
checked (recursively) and a mismatch is a runtime error. Types may nest, and
`any` opts a parameter out of checking.

```my
primes: list<int> = [2, 3, 5, 7]
ages:   map<string, int> = {"ada": 36, "bob": 41}
grid:   map<int, list<any>> = {1: [2, 5, 6], 2: ["a", true]}

oops: list<int> = [1, "two", 3]   # runtime error: expected 'int', got 'string'
```

A plain `list` / `map` (no `<…>`) is untyped and may mix types.

```my
nums = [1, 2, 3]
push(nums, 4)
print(nums[0])        # 1
print(len(nums))      # 4
print(pop(nums))      # 4  (removes and returns the last element)

person = {"name": "Ada", "age": 36}
print(person["name"]) # Ada
print(has(person, "name"))   # true
print(len(person))    # 2
```

**Index assignment.** Assign through a subscript to update a list element in
place or to insert/update a map entry. Compound forms (`+=`, `++`, …) work too,
and subscripts nest:

```my
nums[0] = 10          # list element must already be in range
nums[1] += 5
person["age"] = 37    # update existing key
person["city"] = "NYC"  # new key
grid = [[1, 2], [3, 4]]
grid[0][1] = 99       # nested
```

A list index out of range is a runtime error (use `push` to grow a list); a
compound assignment to a missing map key is an error, but a plain `m[k] = v`
inserts it.

Iterate a map with `keys`, `values`, or `items` (each returns a list, so they
plug straight into `for … in`):

```my
for k in keys(person) {
    print(k, person[k])      # name Ada / age 36
}

for pair in items(person) {
    print(pair[0], "=", pair[1])
}
```

> Methods (`x.f()`) are reserved for [structs](#structs--interfaces); operations
> on built-in types like maps are plain functions, so it is `items(m)`, not
> `m.items()`.

See [`len`, `push`, `pop`, `has`, `keys`, `values`, `items`](#built-in-functions) below.

### Structs & interfaces

Moray models user types with **structs** (data) and **interfaces** (contracts),
with no inheritance. A struct's behavior is split into explicit blocks, and the
interpreter *enforces* where those blocks live — there is no way to scatter a
type's methods across the file.

```my
struct Point {
    x: int
    y: int
}

impl Point {              # a struct's own methods — at most ONE impl block
    fn dist(self) {
        return self.x + self.y
    }
    fn shift(self, dx: int, dy: int) {
        self.x = self.x + dx   # methods can mutate self
        self.y = self.y + dy
    }
}
```

**Construction** reuses call syntax: positional, with optional trailing named
arguments (Python ordering). **Fields** and **methods** are reached with `.`:

```my
a: Point = Point(3, 4)
b: Point = Point(10, y = 20)

print(a.x)            # 3        field access
a.x = 9               # field assignment
print(a.dist())      # 13       method call
print(type(a))       # Point    type() reports the struct's name
```

Structs are **reference types** — assigning shares the instance:

```my
c: Point = a
c.x = 100
print(a.x)           # 100  (a and c are the same instance)
print(a == c)        # true (identity comparison)
```

**Interfaces** declare required method signatures. A struct opts in with an
explicit, subject-first `Type implement Interface` block, and conformance is
checked **at load time** (before any code runs) — a missing method is an error,
not a surprise later:

```my
interface Describable {
    fn describe(self)          # signature only, no body
}

Point implement Describable {
    fn describe(self) {
        print(self.x)
        print(self.y)
    }
}

a.describe()         # methods from implement blocks are called like any other
```

Rules the interpreter enforces:

- **One `impl` block per struct.** A second `impl Point { … }` is a load error.
- **No duplicate methods.** A method name may be defined once across the `impl`
  and `implement` blocks.
- **Load-time conformance.** `T implement I` must define every method `I`
  requires; otherwise the program fails before running.
- `self` is the receiver — an ordinary first parameter, written without a type.

### Truthiness

`if` and `while` accept any value as a condition:

| Value type | Truthy when           |
|------------|-----------------------|
| `bool`     | it is `true`          |
| `int`      | it is non-zero        |
| `float`    | it is non-zero        |
| `null`     | never (always falsy)  |
| `string`   | always truthy         |

### Scope

- Each `{ ... }` block introduces a new child scope.
- Variables declared inside a block are not visible outside it.
- Function bodies run in a scope whose parent is the scope where the function was
  **defined** (lexical scoping), not the scope it is called from. A nested `fn`
  therefore closes over its surrounding locals — see [Functions](#functions).

### Memory management

Memory is **automatic** — Moray has a garbage collector, so you never free
anything by hand. Heap values (strings, lists, maps, and structs) are reclaimed
once they are no longer reachable.

```my
fn build() {
    temp = [1, 2, 3]        # allocated here
    return temp[0]
}                           # `temp` is unreachable after the call → collected

i = 0
while i < 1000000 {
    scratch = [i, i * i]        # a million short-lived lists…
    i = i + 1
}                               # …reclaimed as the loop runs; memory stays flat
```

How it works, briefly:

- Heap values are **reference types** shared by handle (see
  [Value vs. reference](#types)), so the collector tracks reachability rather
  than tying each object to one variable.
- It is a **mark-and-sweep** collector: a collection traces every value
  reachable from a live scope, then frees the rest. Reference cycles (a list or
  struct that refers back to itself) are reclaimed correctly.
- Collections run **when a scope is discarded** — i.e. when a block, function,
  or method call ends — so memory is reclaimed during execution, not just at
  program exit.

This is transparent to programs: there is no syntax for it and nothing to call.

---

## Built-in functions

| Function        | Description                                                        |
|-----------------|-------------------------------------------------------------------|
| `print(...)`    | Prints all arguments separated by spaces, followed by a newline.  |
| `type(x)`       | Returns the type name of `x` as a string (`"int"`, `"float"`, …). |
| `int(x)`        | Converts a number to `int` (truncates floats). Errors on non-numbers. |
| `float(x)`      | Converts a number to `float`. Errors on non-numbers.              |
| `range(end)` / `range(start, end)` | Returns a list of ints `[start, end)` (`start` defaults to 0). Handy with `for … in`. |
| `len(x)`        | Length of a list, map, or string.                                |
| `push(list, v)` / `pop(list)` | Append to / remove and return the last element of a list. |
| `has(map, key)` | Whether `map` contains `key`.                                    |
| `keys(map)` / `values(map)` | Returns the map's keys / values as a list — iterate with `for … in`. |
| `items(map)`    | Returns a list of `[key, value]` pairs (2-element lists).        |

```my
print(type(42))        # int
print(type("hi"))      # string
print(int(3.9))        # 3
print(float(5))        # 5
```

---

## Grammar reference

An informal EBNF of the accepted syntax:

```ebnf
program     = statement* ;

statement   = var_decl
            | assignment
            | fn_def
            | struct_def | interface_def | impl | implement
            | return_stmt
            | if_stmt
            | while_stmt
            | for_stmt
            | "break"
            | "continue"
            | expr_stmt ;

var_decl    = IDENT ":" type "=" expression ;          (* typed declaration   *)
assignment  = lvalue ( assign_op expression | "++" | "--" ) ;
                                                       (* a bare IDENT "=" expr
                                                          declares-or-assigns  *)
assign_op   = "=" | "+=" | "-=" | "*=" | "/=" | "%=" ;
lvalue      = IDENT | postfix "." IDENT ;               (* variable or field   *)
fn_def      = "fn" IDENT "(" params? ")" block ;
params      = param ( "," param )* ;
param       = IDENT ( ":" type )? ;                    (* untyped, e.g. self  *)

struct_def    = "struct" IDENT "{" ( IDENT ":" type )* "}" ;
interface_def = "interface" IDENT "{" fn_sig* "}" ;
fn_sig        = "fn" IDENT "(" params? ")" ;            (* signature, no body  *)
impl          = "impl" IDENT "{" fn_def* "}" ;
implement     = IDENT "implement" IDENT "{" fn_def* "}" ;  (* struct implement interface *)

return_stmt = "return" expression? ;
if_stmt     = "if" expression block ( "else" ( if_stmt | block ) )? ;
while_stmt  = "while" expression block ;
for_stmt    = "for" ( IDENT "in" expression               (* python-style *)
                    | statement? ";" expression? ";" statement? )  (* c-style *)
              block ;
expr_stmt   = expression ;
block       = "{" statement* "}" ;

type        = "int" | "float" | "string" | "bool" | "any" | IDENT  (* IDENT = struct *)
            | "list" ( "<" type ">" )?
            | "map"  ( "<" type "," type ">" )? ;

expression  = logic_or ;
logic_or    = logic_and ( "||" logic_and )* ;
logic_and   = equality ( "&&" equality )* ;
equality    = comparison ( ( "==" | "!=" ) comparison )* ;
comparison  = term ( ( "<" | "<=" | ">" | ">=" ) term )* ;
term        = factor ( ( "+" | "-" ) factor )* ;
factor      = unary ( ( "*" | "/" | "%" ) unary )* ;
unary       = ( "-" | "!" ) unary | postfix ;
postfix     = primary ( "[" expression "]"
                      | "." IDENT
                      | "." IDENT "(" arguments? ")" )* ;
primary     = NUMBER | STRING | "true" | "false" | "null"
            | IDENT
            | IDENT "(" arguments? ")"        (* call or struct construction *)
            | "[" ( expression ( "," expression )* )? "]"   (* list literal  *)
            | "{" ( expression ":" expression ( "," ... )* )? "}" (* map literal: any key *)
            | "(" expression ")" ;
arguments   = arg ( "," arg )* ;
arg         = expression | IDENT "=" expression ;      (* positional | named  *)

STRING      = '"' … '"' | "'" … "'" ;                  (* single or double quotes *)
```

**Reserved keywords:** `fn`, `return`, `if`, `else`, `while`, `for`, `in`,
`break`, `continue`, `struct`, `interface`, `impl`, `implement`, `import`, `as`,
`true`, `false`, `null`, `int`, `float`, `string`, `bool`, `list`, `map`, `any`.

Logical operators are symbolic: `&&` (and), `||` (or), `!` (not).

Whitespace and newlines are not significant — statements are delimited by the
grammar, not by line breaks. A newline simply separates tokens.

---

## Language limitations

Moray is intentionally minimal. Things it does **not** have:

- No inheritance — structs use composition; interfaces give polymorphism
  without a class hierarchy.
- A call evaluates at most 64 arguments; a struct/function at most 64
  fields/parameters (fixed buffers).
- Struct and interface **type names are global**, shared across all imported
  modules — `import`ing two modules that both define a `Point` is a duplicate
  type error. A module's *functions and values* are namespaced; its *types* are
  reached either through the namespace (`m.Point(…)`) or by their bare global
  name (`Point(…)`).
- Type annotations (`name: type`, parameters, struct fields) are enforced for
  **variable declarations** at the declaration site only — a later bare
  reassignment is not re-checked, and parameter/field annotations are
  documentation-only. A struct-name annotation checks only that the value is a
  struct, not which one. `any` opts out of all checking.

---

## Project layout

| Path                  | Purpose                                              |
|-----------------------|------------------------------------------------------|
| `src/lexer.*`         | Turns source text into a stream of tokens.           |
| `src/parser.*`        | Recursive-descent parser → AST.                      |
| `src/ast.*`           | AST node definitions and allocation helpers.         |
| `src/interpreter.*`   | Tree-walking evaluator and built-in functions.       |
| `src/value.*`         | The runtime `Value` tagged union and the mark-and-sweep garbage collector. |
| `src/env.*`           | Lexically-scoped variable environments; GC root set. |
| `src/vec.h`           | Tiny generic growable-vector macro.                  |
| `src/main.c`          | Entry point: reads a `.my` file and runs it.         |
| `examples/sample.my`  | Primitives, lists, and maps.                         |
| `examples/shapes.my`  | Structs, methods, and interfaces.                    |
| `examples/closures.my` | First-class functions and closures.                 |
| `examples/modules.my` + `examples/mathmod.my` | `import` and namespacing.     |
| `examples/loops.my`   | `for`/`while`, `break`/`continue`, and `else if`.    |
| `examples/operators.my` | Compound assignment (`+=`, …) and `++`/`--`.       |
| `examples/map.my`     | Declaring and iterating maps (`keys`/`values`/`items`). |
| `examples/gc_stress.my` | Exercises the garbage collector under heavy churn. |
```
