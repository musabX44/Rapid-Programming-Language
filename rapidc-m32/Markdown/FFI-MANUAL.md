# rapidc FFI Manual

This document walks through the three ways to reach C libraries from
the Rapid! language — `extern fn`, `keyword`, and the C shim pattern —
end to end, in the order "what it's for / how to declare it / how to
use it / why it's designed this way." They were added without
touching the compiler source (`parser.y`, `semantic.c`, `codegen.c`)
at all; each one is either a grammar rule directly (`extern fn`) or a
purely textual shortcut built on top of that rule (`keyword`).

---

## Table of Contents

1. [What FFI is, and why it exists](#1-what-ffi-is-and-why-it-exists)
2. [Layer 1: `extern fn` — the foundation](#2-layer-1-extern-fn--the-foundation)
3. [`link "..."` — linking a library](#3-link---linking-a-library)
4. [Layer 2: `keyword` — the shortcut](#4-layer-2-keyword--the-shortcut)
5. [`keyword` rename: name ≠ C symbol](#5-keyword-rename-name--c-symbol)
6. [Layer 3: C shim — signatures the language can't express](#6-layer-3-c-shim--signatures-the-language-cant-express)
7. [Building: the `-obj` and `-l` flags](#7-building-the--obj-and--l-flags)
8. [Which of the three layers to pick, and when](#8-which-of-the-three-layers-to-pick-and-when)
9. [Limits and known pitfalls](#9-limits-and-known-pitfalls)
10. [End-to-end example: libm](#10-end-to-end-example-libm)
11. [End-to-end example: a C library with a shim](#11-end-to-end-example-a-c-library-with-a-shim)

---

## 1) What FFI is, and why it exists

**FFI** (Foreign Function Interface) is the ability to call, from
Rapid! code, a function that wasn't written in Rapid! (one compiled
with C).

**Why it's needed:** rapidc is a small language on its own; a large
part of file I/O, math functions (`sqrt`, `sin`, `fabs`...), network
access (libcurl), databases (sqlite3), and the like have already been
written and tested in C libraries for decades. Rather than rewriting
these from scratch in rapidc, it's far cheaper to bind directly to the
existing C ABI (Application Binary Interface — the contract for how
functions are called in memory).

**How it's possible:** rapidc compiles code to the QBE intermediate
language, then to native x86-64 assembly, and links it in the final
step with the system `cc` (see the pipeline in `README.md`:
`.rapid → AST → QBE IL → qbe → cc → ELF64`). Since a `.o`/`.so`
compiled with C can be included in that same `cc` link step, all
rapidc needs to do is say, at compile time, "look for a symbol with
this name externally." That's exactly what `extern fn` does.

---

## 2) Layer 1: `extern fn` — the foundation

### What it does

`extern fn` tells rapidc "this function isn't defined in my source,
it exists somewhere on the C side, and its signature is this." You
don't write a body (no `{ ... }`), because the code already lives on
the C side.

### How to declare it

```
extern fn <name>(<param>: <type>, ...): <return_type>;
```

Example:

```
extern fn sqrt(x: double): double;
extern fn strlen(s: string): int;
extern fn printf(fmt: string, ...): int;   // varargs
extern fn exit(code: int): void;
```

Rules (from the `extern_decl` rule in `parser.y`):

- No body; it ends with `;` (instead of the `{ ... }` block of a
  regular `fn`).
- The return type is **always written explicitly** — a regular `fn`
  has a shortcut where omitting it means void, but `extern fn` has no
  such shortcut; even a void return must be written as `: void`. Why:
  for a signature coming from outside, silently assuming "void if
  unwritten" could produce a build that silently mismatches the real
  C signature — writing it explicitly reduces the chance of error.
- The last parameter can be `...` for varargs (as in the `printf`
  example above) — but this is exclusive to `extern fn`; a regular
  `fn`/`efn` cannot take varargs (`parser.y` has a separate
  `extern_param_list` grammar branch, independent from the plain
  `param_list`, for exactly this reason).

### How to use it

Once declared, it's called like an ordinary function — there's no
difference at all from the caller's side:

```
extern fn sqrt(x: double): double;
link "m";

fn main() {
    io::out(sqrt(2.0));   // 1.41421
}
```

### Why it's designed this way

- **It doesn't require a new case in codegen.** `extern fn` enters
  semantic.c's signature registration table (Pass 1) just like a
  normal function, and codegen calls it as `$<name>` too — exactly
  like a `fn` you wrote yourself in Rapid!. The only difference: for
  this name rapidc doesn't generate its own body, assuming instead
  that it'll come from outside (from C), and leaves the symbol
  **undefined**; at the linking stage, `cc` finds and binds that
  symbol in libc or the given library.
- **The signature is checked, but the body isn't verified.** rapidc
  checks that the argument type in a call like `sqrt(2.0)` is `double`
  (semantic.c), but it has no way of knowing that `sqrt` actually
  behaves that way — that's entirely on you, i.e. on writing the
  correct signature. If you write the wrong signature (e.g. writing
  `int` when `double` is expected), the build succeeds but corrupted
  data is read/written at runtime — this is a universal risk of C
  FFI, not something specific to rapidc.
- **It can also be used as a first-class value (M28).** The name of
  an `extern fn` can be assigned to a variable without being called,
  and then called indirectly (`var f = printf; f("hello");`), just
  like regular `fn`s — this comes for free since it enters the
  signature registration table the same way.

---

## 3) `link "..."` — linking a library

### What it does

If a symbol like `sqrt` doesn't live in `libc` but in `libm`, you need
to tell the linker "link libm too" — just like writing
`cc prog.c -lm`. The `link "m";` directive does exactly that.

### How to declare / use it

```
link "m";     // linked as -lm
```

It can be written anywhere in the file, in any order (it's
declarative, and doesn't affect code flow). There can be multiple
`link` directives; each turns into a separate `-l<name>` flag.

### Why it's designed this way

`link` doesn't even produce an AST node — as soon as the parser sees
it, it's added to the `g_program.link_libs` list (where `main.c` puts
together the final `cc` command line, this list is arranged in order
as `-l<name> -l<name> ...`). In other words, `link` lives in a purely
"build instruction" layer that codegen never knows about — so that
you don't have to manually add `-lm` to the build system
(`Makefile` or the `cc` command line) yourself.

---

## 4) Layer 2: `keyword` — the shortcut

### What it does

Writing `extern fn` plus (if needed) `link` means writing the same 2
lines over and over for a commonly used C function. `keyword` is a
purely textual shortcut that condenses these into **a single line** —
it adds no new compiler behavior, it just reduces the number of lines
you have to write.

### How to declare it

```
keyword <name>(<param>: <type>, ...): <return_type> => extern "<C_symbol>" [from "<library>"];
```

Example:

```
keyword sqrt(x: double): double => extern "sqrt" from "m";
keyword strlen(s: string): int  => extern "strlen";
keyword printf(fmt: string, ...): int => extern "printf";
keyword exit(code: int): void => extern "exit";
```

### How to use it

Once declared, you call `name` as if it were one of Rapid!'s own
built-in commands:

```
fn main() {
    io::out(sqrt(2.0));         // 1.41421
    io::out(strlen("hello"));   // 5
    printf("number: %ld\n", 42);
    exit(0);
}
```

The `from "library"` part is optional — if you omit it, only symbols
already implicitly linked in libc (like `strlen`, `printf`, `exit`)
can be targeted.

### Why it's designed this way: the "pure text macro" principle

This is `keyword`'s single most important design decision: **the
compiler source (lexer/parser/semantic/codegen) is never touched.** A
`keyword` declaration is recognized in a text-expansion pass that runs
right before the source is handed to the lexer, at the end of the
module-merging step (`module.c: expand_keyword_decls`), and is
expanded **verbatim** into the already-existing `extern fn` + `link`
syntax:

```
keyword sqrt(x: double): double => extern "sqrt" from "m";
```

turns into the following before the lexer ever sees it:

```
link "m";
extern fn sqrt(x: double): double;
```

That's why `keyword` **doesn't require** a new token, a new AST node,
a new semantic-check branch, or a new codegen case — the type
checking at the call site, the `$<name>` codegen, and the
`-l<library>` link flag are all inherited entirely from the existing
`extern fn`/`link` machinery. `keyword` isn't "a new feature," it's
"an automatic combination of two existing features."

---

## 5) `keyword` rename: name ≠ C symbol

### When it's needed

Sometimes you don't want to use the real symbol name from the C side
as-is on the Rapid! side — for example, a shim function you wrote
yourself might be named `rapidc_http_get`, but in your Rapid! code you
just want to say `http_get(...)`. In `keyword`, the name (`name`) and
the C symbol (`csymbol`) can differ:

```
keyword http_get(url: string, out_status: int*): string
    => extern "rapidc_http_get";
```

### How it works

When the name and the C symbol don't match, `expand_keyword_decls`
produces **two parts**:

1. A hidden, namespaced `extern fn` under the real C symbol, to avoid
   collisions:
   ```
   extern fn __kw_ffi_rapidc_http_get(url: string, out_status: int*): string;
   ```
2. A small `efn` **forwarding wrapper** under the public name you see,
   which passes all parameters positionally to the hidden extern:
   ```
   efn http_get(url: string, out_status: int*) => __kw_ffi_rapidc_http_get(url, out_status);
   ```

So when you call `http_get(...)`, you're actually hopping through two
functions: `http_get` (the efn wrapper) → `__kw_ffi_rapidc_http_get`
(the real C symbol).

### Why it's designed this way

`extern fn`'s codegen always calls the declared name as the plain
`$<name>` symbol (the M27 design) — adding a rename/alias layer
between `name` and the real C symbol would have meant adding it
**directly to codegen**, which would conflict with `keyword`'s goal of
"not touching the compiler code." Instead, two existing mechanisms
(`extern fn` + `efn`) are combined to reach the same result textually:
`efn` already knows how to say "a function that calls a short
expression and returns its result," so the real work is handed off to
it.

### Known limit: varargs can't be renamed

```
keyword my_printf(fmt: string, ...): int => extern "printf";   // ERROR
```

This is rejected outright with a compile-time error: *"renaming a
variadic FFI symbol is not supported."* Why: the `efn` wrapper passes
parameters forward **by name** (`http_get(url, out_status) =>
__kw_ffi_...(url, out_status)`), but `...` isn't a name — how many
extra arguments will come is unknown at compile time, so `efn` can't
forward it. If the name matches (`keyword printf(...) => extern
"printf";`), there's no problem, because in that case no `efn`
wrapper is generated at all — `extern fn` is used directly (see
Section 4).

---

## 6) Layer 3: C shim — signatures the language can't express

### When it's needed

`extern fn` can only express the types rapidc already understands
(`int`, `double`, `string`, `T*` — single-level pointer, etc.). Some C
functions fall outside this:

- **Pointer to pointer** (`T**`) — like sqlite3's
  `sqlite3_open(const char*, sqlite3**)` signature; rapidc's pointer
  type is single-level (`T*`), so `T**` is a grammar error.
- **Union-typed varargs** — functions like libcurl's
  `curl_easy_setopt`, which accept int, string, or a function pointer
  in the same call shape; this can't be expressed with a single
  `extern fn` signature.
- **C function pointer callbacks** (e.g. libcurl's `WRITEFUNCTION`) —
  rapidc's `fn` type doesn't guarantee an ABI that translates directly
  into a C function pointer.

### How to declare it

The solution: write a thin C file (a shim) that **flattens** the C
function in between. The shim calls the complex real API and returns
the result with a plain signature rapidc can understand (what
`extern fn` can express: scalar types, single-level pointers,
strings).

Example (simplified, from `ffi/sqlite3_shim.c`):

```c
// real C signature: int sqlite3_open(const char *filename, sqlite3 **ppDb);
// T** can't be expressed -> make the handle a "long" return value instead
long rapidc_sqlite3_open(const char *filename) {
    sqlite3 *db;
    int rc = sqlite3_open(filename, &db);
    g_last_rc = rc;              // error code kept in a separate global
    return (long)db;             // pass the handle along as an opaque number
}

int rapidc_sqlite3_open_rc(void) {
    return g_last_rc;
}
```

On the Rapid! side, this thin layer is declared with a normal
`extern fn`:

```
extern fn rapidc_sqlite3_open(filename: string): int;
extern fn rapidc_sqlite3_open_rc(): int;
```

The handle (`sqlite3*`) is carried on the Rapid! side as an **opaque
`int`** whose fields are never accessed — it's only ever passed back
into other `sqlite3_*` calls. Rapid! doesn't even need to know it's a
pointer.

### Why it's designed this way

This pattern isn't specific to rapidc — nearly every real-world
language (Rust's `bindgen`, Go's `cgo`, Python's C extensions) applies
the same principle: the parts of a C ABI that **can't be translated
directly** into the target language (pointer-to-pointer, struct by
value, unions, variadics themselves, function-pointer callbacks) get
"flattened" through a thin shim layer and presented to the target
language that way. Since rapidc itself doesn't compile `.c` files (see
Section 7), **you compile this shim ahead of time with `cc`** and add
it to the final link step by hand.

`keyword` is often used together with a shim — since the shim produces
a plain C function, you can use `keyword`'s rename feature to present
that plain function to Rapid! under whatever short name you'd like
(see the `http_get` example in Section 11).

---

## 7) Building: the `-obj` and `-l` flags

### What it does

rapidc only compiles `.rapid` files on its own — it never compiles or
links a `.c` file itself. If you're using a shim, you need to
**first** turn the shim into a `.o` with `cc` yourself, and **manually
add** it to rapidc's final link command.

### How to use it

```bash
# 1) Compile the shim
cc -c ffi/sqlite3_shim.c -o ffi/sqlite3_shim.o

# 2) Add that .o when linking with rapidc
rapidc examples/sqlite_demo.rapid -obj ffi/sqlite3_shim.o -o demo
```

- **`-obj <file.o>`** — adds the given `.o` file to the final `cc`
  command, alongside the actual `.o`/assembly rapidc produces itself.
- **`-l<library>`** — does exactly the same job as the `link "...";`
  source directive (adding an `-l<name>` flag), but for when you want
  to do it from the command line without touching the source.

### Why it's designed this way

rapidc's job description is "translate .rapid into native code
compatible with the C ABI" — not to be a general-purpose C
compiler/build system. Taking on `.c` compilation too would mean
duplicating work `cc` already does very well, and would drag a whole
C build-configuration surface (`-I`/`-D`/optimization flags, etc.)
into rapidc. Instead, rapidc prefers to place the code it generates
next to an **existing** `.o` and tell `cc` "link the two together" —
in keeping with the Unix philosophy of "let each tool do one thing
well."

---

## 8) Which of the three layers to pick, and when

| Situation | Use |
|---|---|
| You'll call a single C function once, no shortcut needed | `extern fn` + `link` if needed |
| A C function you'll use often, whose signature rapidc can express directly (scalar types, single-level pointer) | `keyword` (name matches) |
| You want to present a shim function you wrote yourself under a shorter/cleaner name | `keyword` (with rename) |
| The C function's signature contains something rapidc can't express (`T**`, union varargs, callback) | Write a **C shim** first, then declare the shim with `extern fn` (and `keyword` if needed) |

Simple rule: **`extern fn` is always a sufficient and correct**
starting point — `keyword` exists only for "writing less," it's never
mandatory. When in doubt, start with `extern fn`; if it works, shorten
it to `keyword` afterward if you like.

---

## 9) Limits and known pitfalls

- **`extern fn` doesn't verify the C side's signature.** If you write
  the wrong type, the build succeeds but corrupted data is read/
  written at runtime. This is the universal nature of C FFI — not
  specific to rapidc.
- **`keyword` can't rename varargs** (see Section 5) — if the name
  matches the C symbol (`keyword printf(...) => extern "printf";`)
  there's no problem; if they differ, the build stops with a clear
  error.
- **`keyword`, when renamed, leaks error messages.** When you get a
  link error (`undefined reference to __kw_ffi_...`), the symbol
  you'll see isn't the name you wrote, it's the automatically
  generated hidden name — this is normal, can be confusing, but is
  harmless.
- **Aliased `use "...rapid" as alias;` now correctly mangles
  `extern fn`/`keyword` names too** (`db::sqlite3_close` →
  `db__sqlite3_close`, consistently on both the definition and call
  sides) — the module system gathers a name into the same top-level
  namespace regardless of whether it's a `fn`/`efn`/`extern fn`/
  `keyword` (`scan_top_level`), so `alias::name` works the same way
  for both. You can import FFI bindings either aliased or unaliased.
- **`keyword` only recognizes top-level lines in the file** — a
  string literal containing the word "keyword" (e.g.
  `io::out("keyword test");`) isn't accidentally expanded; a full
  line scan with string-literal skipping guarantees this.
- **Shim `.o` files aren't part of rapidc's build system** — you need
  to manually keep the shim up to date/compiled with every build;
  rapidc doesn't track this for you (see Section 7).

---

## 10) End-to-end example: libm

```
// math_demo.rapid
extern fn sqrt(x: double): double;
extern fn fabs(x: double): double;
link "m";

fn main() {
    io::out(sqrt(2.0));    // 1.41421
    io::out(fabs(-5.5));   // 5.5
}
```

Building and running:

```bash
rapidc math_demo.rapid -o math_demo
./math_demo
```

Here `-obj` isn't needed, because `sqrt`/`fabs` can be expressed
directly with `extern fn` — no shim is needed in between, just linking
to libm with `link "m";` is enough.

Same thing with `keyword`, as one-line declarations:

```
keyword sqrt(x: double): double => extern "sqrt" from "m";
keyword fabs(x: double): double => extern "fabs" from "m";

fn main() {
    io::out(sqrt(2.0));
    io::out(fabs(-5.5));
}
```

---

## 11) End-to-end example: a C library with a shim

libcurl's `curl_easy_setopt` API (union-typed varargs + a function
pointer callback) can't be expressed directly with `extern fn`, so a
shim is needed.

**Step 1 — Write the shim** (`net_shim.c`):

```c
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>

static size_t write_cb(void *data, size_t sz, size_t n, void *userp) {
    /* accumulate the body into a buffer, return the byte count consumed */
    ...
}

char *rapidc_http_get(const char *url, long *out_status) {
    CURL *c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    /* ... collect the body ... */
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, out_status);
    curl_easy_cleanup(c);
    return strdup(body);   // returns a plain string to the Rapid! side
}

void rapidc_http_free(char *body) { free(body); }
```

**Step 2 — Introduce the shim to Rapid! with a `keyword` rename**
(`net.rapid`):

```
link "curl";

keyword http_get(url: string, out_status: int*): string
    => extern "rapidc_http_get";

keyword http_free(body: string): void
    => extern "rapidc_http_free";
```

**Step 3 — Use it:**

```
use "net.rapid";

fn main() {
    var status: int* = alloc(8);
    var body: string = http_get("http://example.com", status);
    io::out(*status);   // HTTP status code, e.g. 200
    io::out(body);      // response body
    http_free(body);
    free(status);
}
```

**Step 4 — Build:**

```bash
cc -c net_shim.c -o net_shim.o
rapidc app.rapid -obj net_shim.o -o app
# link "curl"; is already in the source, so -lcurl is added automatically
```

In this example all three layers work together: the C shim (flattens
the complex libcurl API) + the `keyword` rename (turns the shim's ugly
`rapidc_http_get` name into the clean `http_get`) + `link` (links
libcurl). On the user's side (Step 3) there's no trace of C/FFI
visible at all — `http_get` is called as if it were one of Rapid!'s
own built-in functions.
