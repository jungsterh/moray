# Moray

Moray is a small, statically-typed scripting language with a tree-walking
interpreter written in C. Programs are stored in `.my` files and run directly —
there is no separate compile step.

```my
fn factorial(int n) {
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
int x = 1   # trailing comments work too
```

### Types

| Type     | Keyword  | Literals / construction        | Semantics |
|----------|----------|--------------------------------|-----------|
| Integer  | `int`    | `42`, `-7`, `0`                | value     |
| Float    | `float`  | `3.14`, `0.5`                  | value     |
| String   | `string` | `"hello"`                      | value     |
| Boolean  | `bool`   | `true`, `false`                | value     |
| List     | `list`   | `[1, 2, 3]`                    | reference |
| Map      | `map`    | `{"a": 1}`                     | reference |
| Struct   | *name*   | `Point(3, 4)` (see below)      | reference |
| Null     | —        | `null`                         | value     |

Numeric literals without a fractional part are stored as `int`; literals with a
`.` are `float`. There is no `null` type keyword — `null` is only a value.

**Value vs. reference.** Numbers, booleans, and strings are values. Lists, maps,
and structs are *references*: assigning or passing one shares the same
underlying object rather than copying it (see [Structs](#structs--interfaces)).

### Variables

A declaration is `type name = expression`. The type annotation and an
initializer are both **required**.

```my
int    count    = 10
float  ratio    = 1.5
string name     = "Moray"
bool   ready    = true
```

Reassign an existing variable **without** a type:

```my
count = count + 1     # ok
int count = 5         # this declares a *new* variable, shadowing in inner scopes
```

Assigning to a name that was never declared is a runtime error.

> Note: the declared type is used to parse the declaration, but values are
> dynamically typed at runtime — there is currently no static type checking that
> a declared `int` only ever holds integers.

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

Strings are double-quoted. Concatenate with `+`:

```my
string greeting = "Hello, " + "Moray!"
print(greeting)            # Hello, Moray!
```

There are currently no escape sequences (e.g. `\n`, `\"`) — a string runs from
one `"` to the next. Strings may span multiple lines.

### Control flow

**`if` / `else`** — the condition is *not* parenthesized; braces are required.

```my
if x > 10 {
    print("big")
} else {
    print("small")
}
```

There is no `else if` keyword. Nest an `if` inside an `else` block instead:

```my
if x > 10 {
    print("big")
} else {
    if x > 5 {
        print("medium")
    } else {
        print("small")
    }
}
```

**`while`** — the only loop construct:

```my
int i = 0
while i < 5 {
    print(i)
    i = i + 1
}
```

### Functions

Define with `fn`, annotate every parameter with a type, and use `return`:

```my
fn add(int a, int b) {
    return a + b
}

print(add(2, 3))   # 5
```

- A bare `return` (no expression) returns `null`.
- Falling off the end of a function also returns `null`.
- Recursion is supported (see `factorial` above).
- Functions are defined in the global scope and can call other globally-defined
  functions.

Arguments may be passed positionally or by name, with named arguments after the
positional ones (as in Python):

```my
fn make(int a, int b) { return a + b }

make(1, 2)            # positional
make(1, b = 2)        # last argument named
make(a = 1, b = 2)    # all named
```

### Lists & maps

Lists are ordered and indexed from `0`; maps associate string keys with values.
Both are reference types and can hold values of any type.

```my
list nums = [1, 2, 3]
push(nums, 4)
print(nums[0])        # 1
print(len(nums))      # 4
print(pop(nums))      # 4  (removes and returns the last element)

map person = {"name": "Ada", "age": 36}
print(person["name"]) # Ada
print(has(person, "name"))   # true
print(len(person))    # 2
```

See [`len`, `push`, `pop`, `has`](#built-in-functions) below.

### Structs & interfaces

Moray models user types with **structs** (data) and **interfaces** (contracts),
with no inheritance. A struct's behavior is split into explicit blocks, and the
interpreter *enforces* where those blocks live — there is no way to scatter a
type's methods across the file.

```my
struct Point {
    int x
    int y
}

impl Point {              # a struct's own methods — at most ONE impl block
    fn dist(self) {
        return self.x + self.y
    }
    fn shift(self, int dx, int dy) {
        self.x = self.x + dx   # methods can mutate self
        self.y = self.y + dy
    }
}
```

**Construction** reuses call syntax: positional, with optional trailing named
arguments (Python ordering). **Fields** and **methods** are reached with `.`:

```my
Point a = Point(3, 4)
Point b = Point(10, y = 20)

print(a.x)            # 3        field access
a.x = 9               # field assignment
print(a.dist())      # 13       method call
print(type(a))       # Point    type() reports the struct's name
```

Structs are **reference types** — assigning shares the instance:

```my
Point c = a
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
- Function bodies run in a scope whose parent is the **global** scope, so they
  see globals and their own parameters/locals, but **not** the locals of the
  caller (there are no closures).

---

## Built-in functions

| Function        | Description                                                        |
|-----------------|-------------------------------------------------------------------|
| `print(...)`    | Prints all arguments separated by spaces, followed by a newline.  |
| `type(x)`       | Returns the type name of `x` as a string (`"int"`, `"float"`, …). |
| `int(x)`        | Converts a number to `int` (truncates floats). Errors on non-numbers. |
| `float(x)`      | Converts a number to `float`. Errors on non-numbers.              |

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
            | expr_stmt ;

var_decl    = ( type | IDENT ) IDENT "=" expression ;   (* IDENT type = struct *)
assignment  = lvalue "=" expression ;
lvalue      = IDENT | postfix "." IDENT ;               (* variable or field   *)
fn_def      = "fn" IDENT "(" params? ")" block ;
params      = param ( "," param )* ;
param       = type? IDENT ;                             (* untyped param = self *)

struct_def    = "struct" IDENT "{" ( type IDENT )* "}" ;
interface_def = "interface" IDENT "{" fn_sig* "}" ;
fn_sig        = "fn" IDENT "(" params? ")" ;            (* signature, no body  *)
impl          = "impl" IDENT "{" fn_def* "}" ;
implement     = IDENT "implement" IDENT "{" fn_def* "}" ;  (* struct implement interface *)

return_stmt = "return" expression? ;
if_stmt     = "if" expression block ( "else" block )? ;
while_stmt  = "while" expression block ;
expr_stmt   = expression ;
block       = "{" statement* "}" ;

type        = "int" | "float" | "string" | "bool" | "list" | "map" ;

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
            | "{" ( STRING ":" expression ( "," ... )* )? "}" (* map literal *)
            | "(" expression ")" ;
arguments   = arg ( "," arg )* ;
arg         = expression | IDENT "=" expression ;      (* positional | named  *)
```

**Reserved keywords:** `fn`, `return`, `if`, `else`, `while`, `struct`,
`interface`, `impl`, `implement`, `true`, `false`, `null`,
`int`, `float`, `string`, `bool`, `list`, `map`.

Logical operators are symbolic: `&&` (and), `||` (or), `!` (not).

Whitespace and newlines are not significant — statements are delimited by the
grammar, not by line breaks. A newline simply separates tokens.

---

## Language limitations

Moray is intentionally minimal. Things it does **not** have (yet):

- No `for` loops, `break`, or `continue`.
- No `else if` keyword (nest inside `else`).
- No string escape sequences (`\n`, `\t`, `\"`, …).
- No inheritance or generics — structs use composition; interfaces give
  polymorphism without a class hierarchy.
- No index assignment (`list[0] = x`); use `push`/`pop`. Field assignment
  (`p.x = 5`) *is* supported.
- No closures — functions and methods only capture the global scope.
- No module/import system.
- A call evaluates at most 64 arguments; a struct/function at most 64
  fields/parameters (fixed buffers).
- Declared types are not statically enforced at runtime (annotations are
  documentation; values are dynamically typed).
- No garbage collection — heap values are reclaimed by the OS at exit, not
  during execution.

---

## Project layout

| Path                  | Purpose                                              |
|-----------------------|------------------------------------------------------|
| `src/lexer.*`         | Turns source text into a stream of tokens.           |
| `src/parser.*`        | Recursive-descent parser → AST.                      |
| `src/ast.*`           | AST node definitions and allocation helpers.         |
| `src/interpreter.*`   | Tree-walking evaluator and built-in functions.       |
| `src/value.*`         | The runtime `Value` tagged union.                    |
| `src/env.*`           | Lexically-scoped variable environments.              |
| `src/vec.h`           | Tiny generic growable-vector macro.                  |
| `src/main.c`          | Entry point: reads a `.my` file and runs it.         |
| `examples/sample.my`  | Primitives, lists, and maps.                         |
| `examples/shapes.my`  | Structs, methods, and interfaces.                    |
```
