# rapidc FFI libraries

This folder contains a userland FFI binding built on `link "..."` +
`extern fn`: **`sqlite3.rapid`**, a minimal binding to libsqlite3.

## How to use `sqlite3.rapid`

```
use "../ffi/sqlite3.rapid";

fn main() {
    var handle: int = rapidc_sqlite3_open("database.db");
    if (rapidc_sqlite3_open_rc() != 0) {
        io::out("couldn't open");
        return;
    }
    sqlite3_exec(handle, "CREATE TABLE IF NOT EXISTS person (id INTEGER PRIMARY KEY, name TEXT);", 0, 0, 0);
    sqlite3_exec(handle, "INSERT INTO person (name) VALUES ('Yasin');", 0, 0, 0);
    io::out(sqlite3_changes(handle));
    sqlite3_close(handle);
}
```

Build (a single command is enough from M31 onward):

```
rapidc examples/sqlite_demo.rapid -o demo
```

- `sqlite3.rapid` now includes `shim "sqlite3_shim.c";`, so
  `sqlite3_shim.c` is automatically compiled by rapidc with `cc -c`
  and added to the final link step — no separate
  `cc -c ffi/sqlite3_shim.c -o ffi/sqlite3_shim.o` step is needed.
- The `libsqlite3` runtime library must be installed on your system,
  and the symlink the linker looks for, `libsqlite3.so` (without a
  version number), must exist — on some distros this only comes with
  the `-dev` package:
  `sudo ln -sf /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 /usr/lib/x86_64-linux-gnu/libsqlite3.so`
  (or `apt install libsqlite3-dev`).

## Why there's a C shim (`sqlite3_shim.c`)

The real C signature of `sqlite3_open` is:

```c
int sqlite3_open(const char *filename, sqlite3 **ppDb);
```

`ppDb` is a **pointer to a pointer** (an output parameter). rapidc's
pointer type only supports a single level (`T*`) — `T**` is a grammar
error. That's why we can't declare `sqlite3_open` directly with
`extern fn`.

`sqlite3_shim.c` provides a thin C wrapper named `rapidc_sqlite3_open`
that calls the real `sqlite3_open` and returns the resulting
`sqlite3*` handle as a `long` **return value**; the error code is read
separately via a `rapidc_sqlite3_open_rc()` call. The rapidc side
declares this thin layer with `extern fn` and carries the handle
around as an opaque `int` throughout (never accessing its fields,
just passing it back to the other sqlite3_* calls).

This pattern is standard in real-world languages as well: the parts
of a C ABI that can't be translated directly into a host language
(`rapidc`, Rust, Go, ...) — pointer-to-pointer, struct-by-value,
unions, variadics themselves, etc. — get "flattened" through a thin C
shim and presented to the host language that way.

## Known limitations

- **`sqlite3_exec` can't read result rows** — the callback is always
  passed `0`/NULL, so it can only be used for commands that don't
  return results, like `CREATE`/`INSERT`/`UPDATE`/`DELETE`. Reading
  rows with `SELECT` requires the
  `sqlite3_prepare_v2`/`sqlite3_step`/`sqlite3_column_*` API, which
  isn't in this first release.
- rapidc compiles `.c` files declared via the `shim "...";` directive
  itself, using `cc -c` (M31) — with no extra include-path/define/
  optimization flags, producing `<file>.c.o` next to the file. If
  compiling the shim requires a special flag (`-I`, `-D`, ...), this
  isn't supported yet — you'll need to compile such a shim by hand
  ahead of time and add it to the final link step with `-obj <file.o>`.
