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

Moray has four value types plus `null`:

| Type     | Keyword  | Literals                  | Backing C type |
|----------|----------|---------------------------|----------------|
| Integer  | `int`    | `42`, `-7`, `0`           | `long`         |
| Float    | `float`  | `3.14`, `0.5`             | `double`       |
| String   | `string` | `"hello"`                 | `char *`       |
| Boolean  | `bool`   | `true`, `false`           | `int`          |
| Null     | —        | `null`                    | —              |

Numeric literals without a fractional part are stored as `int`; literals with a
`.` are `float`. There is no `null` type keyword — `null` is only a value.

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
            | return_stmt
            | if_stmt
            | while_stmt
            | expr_stmt ;

var_decl    = type IDENT "=" expression ;
assignment  = IDENT "=" expression ;
fn_def      = "fn" IDENT "(" params? ")" block ;
params      = param ( "," param )* ;
param       = type IDENT ;
return_stmt = "return" expression? ;
if_stmt     = "if" expression block ( "else" block )? ;
while_stmt  = "while" expression block ;
expr_stmt   = expression ;
block       = "{" statement* "}" ;

type        = "int" | "float" | "string" | "bool" ;

expression  = logic_or ;
logic_or    = logic_and ( "||" logic_and )* ;
logic_and   = equality ( "&&" equality )* ;
equality    = comparison ( ( "==" | "!=" ) comparison )* ;
comparison  = term ( ( "<" | "<=" | ">" | ">=" ) term )* ;
term        = factor ( ( "+" | "-" ) factor )* ;
factor      = unary ( ( "*" | "/" | "%" ) unary )* ;
unary       = ( "-" | "!" ) unary | primary ;
primary     = NUMBER | STRING | "true" | "false" | "null"
            | IDENT
            | IDENT "(" arguments? ")"
            | "(" expression ")" ;
arguments   = expression ( "," expression )* ;
```

**Reserved keywords:** `fn`, `return`, `if`, `else`, `while`,
`true`, `false`, `null`, `int`, `float`, `string`, `bool`.

Logical operators are symbolic: `&&` (and), `||` (or), `!` (not).

Whitespace and newlines are not significant — statements are delimited by the
grammar, not by line breaks. A newline simply separates tokens.

---

## Language limitations

Moray is intentionally minimal. Things it does **not** have (yet):

- No `for` loops, `break`, or `continue`.
- No `else if` keyword (nest inside `else`).
- No string escape sequences (`\n`, `\t`, `\"`, …).
- No user-defined types/structs.
- No closures — functions only capture the global scope.
- No module/import system.
- A function call evaluates at most 64 arguments (fixed buffer).
- Declared types are not statically enforced at runtime.

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
| `examples/sample.my`  | Example program demonstrating the language.          |
```
