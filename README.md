# Rapid!

**A modern, general-purpose language built to be fast and easy to learn.**

Rapid! compiles straight down to native x86-64 Linux ELF binaries. No VM, no
interpreter, no garbage-collected runtime lurking underneath — just a small,
readable language on top of a straightforward, real compiler pipeline.

```
.rapid → Flex (lexer) → Bison (parser) → AST → Semantic Check → QBE IL → qbe → cc → ELF64
```

## Why Rapid!

- **Fast to write.** Short, expression-oriented syntax (`efn square(x) => x * x`)
  sits right next to full statement-based functions when you need them.
- **Fast to run.** Everything compiles to native code through [QBE](https://c9x.me/compile/)
  — no bytecode, no JIT warm-up.
- **Fast to learn.** Familiar C-family syntax, sane defaults (e.g. omit a
  return type and you get `void`), and error messages that point at real
  mistakes instead of cryptic type-theory jargon.
- **Talks to C when you need it to.** A first-class FFI story (`extern fn`,
  `link`, `keyword`) means you're never stuck reinventing libm or libcurl.

## Quick taste

```rapid
use "mathlib.rapid";

efn square(x) => x * x

fn main() {
    var name: string = io::in();
    io::out("Hello, {}! square(5) = {}", name, square(5));

    list<int> xs = [1, 2, 3];
    push(xs, 4);
    for (x in xs) {
        io::out(x);
    }
}
```

## Language features

- **Core language** — functions with typed parameters, `var`/`const`
  declarations, `if`/`else if`/`else`, `while`, classic three-part `for`,
  `for (x in xs)` iteration, `switch`/`case`, block scoping, `break`/`continue`
- **`efn` short-form functions** — single-expression functions with inferred
  return types and optional parameter types (`efn add(a, b) => a + b`)
- **First-class functions** — function values with a `fn(T1, T2): Tret` type,
  passing functions as arguments, calling through variables (no closures)
- **Rich type system** — `bool`, `string`, fixed-size arrays (`int[]`) and
  growable `list<int>`, fixed-width integers (`int8`..`int64`, `uint8`..`uint64`),
  `float`/`double` with proper IEEE-754 semantics, generic pointers
- **Module system** — `use "file.rapid";`, aliased imports (`use "..." as ns;`),
  `private` visibility, diamond-import-safe, cycle detection
- **I/O built-ins** — `io::in()`/`io::out()` (including `{}`-style formatted
  output), and raw file descriptor access via `io::open`/`read`/`write`/`close`/`errno`
- **Systems primitives** — `alloc`/`free`/`syscall` for direct, low-level
  control when you need it
- **FFI, done properly**
  - `extern fn` — declare and call external (typically C/libc) functions directly
  - `link "..."` — link additional libraries (`-l<name>`) straight from source
  - `keyword` — a zero-compiler-changes, pure text-expansion sugar layer over
    `extern fn` + `link`, including renaming a C symbol to a cleaner name
  - **C shim pattern** for signatures Rapid! can't express directly
    (pointer-to-pointer, union varargs, callback pointers) — flatten them in a
    thin `.c` file and bind the flat version with `extern fn`

## Status

Rapid! is an actively evolving hobby/systems language — the compiler is a
real Flex/Bison/QBE pipeline, not a toy interpreter, and new language
features land as milestones. Expect some rough edges and missing pieces
(no closures yet, no generics beyond `list<int>`, limited pointer arithmetic)
while the core keeps growing.

## Building

**Requirements:** `flex`, `bison`, [`qbe`](https://c9x.me/compile/) (must be
on `PATH`), `cc`/`gcc`, `make`.

```bash
apt-get install flex bison gcc make
# qbe isn't packaged on most distros — build it from source and add it to PATH

make
./rapidc examples/hello.rapid -o hello
./hello
```

## Contributing

Issues, ideas, and PRs are welcome — this is very much a work in progress
and feedback from people poking at real programs is the most useful kind.

## License
This project is licensed under the [MIT License](LICENSE).

[LICENSE]
(LICENS
