#include "module.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libgen.h>
#include <limits.h>

/* ---------------------------------------------------------------------
   Growable string buffer
   --------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf *b) {
    b->cap = 4096;
    b->len = 0;
    b->data = malloc(b->cap);
    b->data[0] = '\0';
}

static void buf_ensure(Buf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) b->cap *= 2;
        b->data = realloc(b->data, b->cap);
    }
}

static void buf_append(Buf *b, const char *s, size_t n) {
    buf_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_append_str(Buf *b, const char *s) {
    buf_append(b, s, strlen(s));
}

/* ---------------------------------------------------------------------
   Whole-file text loading
   --------------------------------------------------------------------- */

static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ---------------------------------------------------------------------
   Simple identifier-aware rename: replaces every whole-word occurrence of
   `from` with `to` in `src`, skipping over string/char literals and
   comments so we never mangle text that happens to appear inside them.
   Returns a freshly malloc'd buffer.
   --------------------------------------------------------------------- */

static int is_ident_char(int c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Skips backward over whitespace in `out` (the buffer built so far) and
   reports the last non-whitespace character emitted, or '\0' if `out` is
   empty. Used to look at what immediately precedes the identifier `p` is
   currently pointing at, without having to re-scan `src` from the start. */
static char last_nonspace_emitted(const Buf *out) {
    for (size_t i = out->len; i > 0; i--) {
        char c = out->data[i - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return c;
    }
    return '\0';
}

/* Looks ahead from `p` (which points just past an identifier) skipping
   whitespace, and reports the next non-whitespace character, or '\0' at
   end of input. Used to tell a struct-literal/definition field name
   (`IDENT :` inside `{ ... }` or after `,`) apart from an ordinary
   identifier that merely happens to be followed by a colon elsewhere
   (e.g. a `var x: Type` declaration is never reached with is_field_name
   context, see caller). */
static char next_nonspace(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return *p;
}

/* Renaming top-level fn/efn/struct/enum names must never touch a struct
   *field* name, even when it's spelled identically — field names and
   top-level declaration names live in entirely separate namespaces in
   this language (`p.value` / `Point{value: 1}` / `value: int;` inside a
   struct body can never refer to a same-named top-level function or
   another file's struct), so mangling them together is always wrong,
   not just incidentally risky. Two shapes identify a field-name
   position textually:
     - immediately preceded by `.`      -> field access (`b.value`)
     - immediately followed by `:` AND
       immediately preceded by `{` or `,` -> struct field decl/literal
       (`Box { value: v }`, or the `value: int;` field list in a
       `struct Box { ... }` body)
   Neither shape can also be a legal position for a top-level-name
   reference (a function call is always `ident(`, a bare value/type use
   is never preceded by `.` or wedged between `{`/`,` and `:`), so this
   check has no false negatives against the renames this function is
   actually supposed to perform. */
static int is_field_name_position(const Buf *out, const char *after) {
    char prev = last_nonspace_emitted(out);
    if (prev == '.') return 1;
    char next = next_nonspace(after);
    if (next == ':' && (prev == '{' || prev == ',')) return 1;
    return 0;
}

static char *rename_ident_real(const char *src, const char *from, const char *to) {
    size_t from_len = strlen(from);
    Buf out;
    buf_init(&out);
    const char *p = src;
    while (*p) {
        if (*p == '"') {
            const char *start = p;
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (*p == '"') p++;
            buf_append(&out, start, (size_t)(p - start));
            continue;
        }
        if (*p == '\'') {
            const char *start = p;
            p++;
            while (*p && *p != '\'') {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (*p == '\'') p++;
            buf_append(&out, start, (size_t)(p - start));
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            const char *start = p;
            while (*p && *p != '\n') p++;
            buf_append(&out, start, (size_t)(p - start));
            continue;
        }
        if (is_ident_char(*p) && !isdigit((unsigned char)*p)) {
            const char *start = p;
            while (is_ident_char(*p)) p++;
            size_t word_len = (size_t)(p - start);
            if (word_len == from_len && strncmp(start, from, from_len) == 0 &&
                !is_field_name_position(&out, p)) {
                buf_append_str(&out, to);
            } else {
                buf_append(&out, start, word_len);
            }
            continue;
        }
        buf_append(&out, p, 1);
        p++;
    }
    return out.data;
}

/* Replace every occurrence of literal substring `from` (not identifier-
   bounded; used for the two-character "alias::name" pattern) with `to`.
   Also comment/string-literal aware. */
static char *replace_alias_calls(const char *src, const char *alias, const char *mangled_prefix) {
    /* Looks for `<alias>::<ident>` and rewrites it to `<mangled_prefix><ident>`.
       `alias` must match as a whole identifier immediately before `::`. */
    size_t alias_len = strlen(alias);
    Buf out;
    buf_init(&out);
    const char *p = src;
    while (*p) {
        if (*p == '"') {
            const char *start = p;
            p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p == '"') p++;
            buf_append(&out, start, (size_t)(p - start));
            continue;
        }
        if (*p == '\'') {
            const char *start = p;
            p++;
            while (*p && *p != '\'') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p == '\'') p++;
            buf_append(&out, start, (size_t)(p - start));
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            const char *start = p;
            while (*p && *p != '\n') p++;
            buf_append(&out, start, (size_t)(p - start));
            continue;
        }
        if (is_ident_char(*p) && !isdigit((unsigned char)*p)) {
            const char *start = p;
            while (is_ident_char(*p)) p++;
            size_t word_len = (size_t)(p - start);
            /* is this exactly `alias` followed by `::` followed by an ident? */
            if (word_len == alias_len && strncmp(start, alias, alias_len) == 0 &&
                p[0] == ':' && p[1] == ':') {
                const char *after_cc = p + 2;
                if (is_ident_char(*after_cc) && !isdigit((unsigned char)*after_cc)) {
                    const char *id_start = after_cc;
                    const char *q = after_cc;
                    while (is_ident_char(*q)) q++;
                    size_t id_len = (size_t)(q - id_start);
                    buf_append_str(&out, mangled_prefix);
                    buf_append(&out, id_start, id_len);
                    p = q;
                    continue;
                }
            }
            buf_append(&out, start, word_len);
            continue;
        }
        buf_append(&out, p, 1);
        p++;
    }
    return out.data;
}

/* ---------------------------------------------------------------------
   Top-level declaration scanning: finds each top-level `fn`/`efn`/`struct`/
   `enum` name (optionally preceded by `private`) and each top-level
   `use "path"[ as alias];` directive, in source order. Deliberately naive
   (no full tokenizer) — skips strings/chars/comments, and only looks at
   lines whose first non-space token is one of the recognized keywords,
   which matches this language's fixed top-level-only declaration style.
   --------------------------------------------------------------------- */

typedef struct {
    char *name;
    int is_private;
} TopDecl;

typedef struct {
    char *path;      /* raw string as written in the use directive */
    char *alias;     /* NULL if no alias */
} UseDirective;

/* Very small helper: skip whitespace */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static int starts_with_word(const char *p, const char *word) {
    size_t n = strlen(word);
    if (strncmp(p, word, n) != 0) return 0;
    return !is_ident_char((unsigned char)p[n]);
}

/* Strips comments and returns a comment-free copy (still string/char-literal
   safe), so the simple line/keyword scan below doesn't get confused by
   "// use "fake.rapid";" or similar inside a comment. */
static char *strip_comments(const char *src) {
    Buf out;
    buf_init(&out);
    const char *p = src;
    while (*p) {
        if (*p == '"') {
            const char *start = p;
            p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p == '"') p++;
            buf_append(&out, start, (size_t)(p - start));
            continue;
        }
        if (*p == '\'') {
            const char *start = p;
            p++;
            while (*p && *p != '\'') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p == '\'') p++;
            buf_append(&out, start, (size_t)(p - start));
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue; /* drop it, don't even keep the newline twice */
        }
        buf_append(&out, p, 1);
        p++;
    }
    return out.data;
}

/* ---------------------------------------------------------------------
   M29: `keyword` declarations — user-defined FFI "keywords" without
   touching the compiler's own source (lexer.l/parser.y/semantic.c/
   codegen.c stay untouched; this is a pure text-level macro expansion
   that runs on the fully merged source, right before it is handed to
   the lexer).

   Syntax (source-level only, disappears before parsing):

       keyword NAME(params...) : rettype => extern "csymbol" [from "lib"];

   `params...` and `rettype` use exactly the same grammar as an ordinary
   `extern fn` parameter list / return type (including a trailing `...`
   for varargs) because that's precisely what this expands into. Example:

       keyword sqrt(x: double): double => extern "sqrt" from "m";
       keyword strlen(s: string): int  => extern "strlen";

       fn main() {
           io::out(sqrt(2.0));
           io::out(strlen("merhaba"));
       }

   expands (textually) into:

       link "m";
       extern fn sqrt(x: double): double;
       extern fn strlen(s: string): int;

       fn main() { ... }

   so `sqrt`/`strlen` become ordinary callable names exactly the way an
   `extern fn` name would — no new AST node, no new token, no new
   semantic/codegen case. Everything downstream (call-site type
   checking, `$<csymbol>` codegen, `-l<lib>` link flag) is the existing
   M27/M28 `extern fn` + `link` machinery, entirely unmodified. If NAME
   and csymbol differ, the generated `extern fn` still calls the C
   symbol under NAME's mangled-emit path is unaffected — codegen always
   emits a call to the *declared* extern fn's own name, so to actually
   reach a differently-named C symbol we emit a tiny per-keyword shim
   line instead: `extern fn NAME(...): ret;` is declared under csymbol
   directly when NAME == csymbol (the common case), and when they
   differ we still declare the extern under NAME (so call sites read
   naturally) but rename it to the real csymbol via a linker alias is
   out of scope — so for now `keyword` requires NAME to be the very
   identifier callers use AND the symbol rapidc calls; a `from "lib"`
   clause only ever affects linking, matching how `extern fn` already
   works. This keeps the feature a pure sugar/rewrite with zero new
   runtime behavior. */

typedef struct {
    long start;  /* offset of the 'keyword' token in src           */
    long end;    /* offset right after the terminating ';'         */
    char *replacement; /* text to splice in instead                */
} KeywordExpansion;

/* Copies characters from *psrc up to (not including) `stop_char` at
   nesting depth 0 (parens are tracked so a `,`/`)` inside a nested
   `fn(...)::ret` type in the param list doesn't confuse the scan) into
   a freshly malloc'd, NUL-terminated buffer, and advances *psrc past
   the copied span (leaving `stop_char` unconsumed). Returns NULL if
   the input ends before `stop_char` is found at depth 0. */
static char *copy_until_balanced(const char **psrc, char stop_char) {
    const char *p = *psrc;
    const char *start = p;
    int depth = 0;
    while (*p) {
        if (*p == '(' ) depth++;
        else if (*p == ')') { if (depth == 0 && stop_char == ')') break; depth--; }
        else if (depth == 0 && *p == stop_char) break;
        else if (*p == '"') {
            p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        }
        p++;
    }
    if (*p != stop_char) return NULL;
    size_t len = (size_t)(p - start);
    char *buf = malloc(len + 1);
    memcpy(buf, start, len);
    buf[len] = '\0';
    *psrc = p;
    return buf;
}

/* Scans the whole (comment-stripped) source for top-level `keyword ...;`
   declarations and returns a new buffer with each one replaced by the
   `extern fn` (+ optional `link "lib";`) text it expands to. Malformed
   `keyword` declarations are left as-is (untouched byte-for-byte) so the
   normal parser produces its own, ordinary syntax-error message pointing
   at the real problem, rather than this pass swallowing it silently. */
static char *expand_keyword_decls(const char *src) {
    Buf out;
    buf_init(&out);
    const char *p = src;

    while (*p) {
        if (starts_with_word(p, "keyword")) {
            const char *decl_start = p;
            const char *q = p + 7; /* strlen("keyword") */
            q = skip_ws(q);

            /* NAME */
            const char *name_start = q;
            while (is_ident_char((unsigned char)*q)) q++;
            size_t name_len = (size_t)(q - name_start);

            const char *after_name = skip_ws(q);

            if (name_len == 0 || *after_name != '(') {
                /* Not a well-formed `keyword NAME(...)` — emit the
                   'keyword' token verbatim and let the parser reject it
                   with a real error (it isn't a recognized token, so
                   this just reproduces today's "unexpected character"
                   style failure, no worse than before this pass existed). */
                buf_append(&out, p, 7);
                p += 7;
                continue;
            }

            const char *params_open = after_name + 1; /* just past '(' */
            const char *scan = params_open;
            char *params = copy_until_balanced(&scan, ')');
            if (!params) { buf_append(&out, p, 7); p += 7; continue; }
            scan++; /* consume ')' */

            scan = skip_ws(scan);
            if (*scan != ':') { buf_append(&out, p, 7); p += 7; free(params); continue; }
            scan++;
            scan = skip_ws(scan);

            const char *ret_start = scan;
            /* return type runs up to '=>' */
            const char *arrow = strstr(scan, "=>");
            if (!arrow) { buf_append(&out, p, 7); p += 7; free(params); continue; }
            size_t ret_len = (size_t)(arrow - ret_start);
            while (ret_len > 0 && (ret_start[ret_len - 1] == ' ' || ret_start[ret_len - 1] == '\t' ||
                                    ret_start[ret_len - 1] == '\n' || ret_start[ret_len - 1] == '\r')) ret_len--;
            char *rettype = malloc(ret_len + 1);
            memcpy(rettype, ret_start, ret_len);
            rettype[ret_len] = '\0';

            scan = arrow + 2;
            scan = skip_ws(scan);
            if (!starts_with_word(scan, "extern")) {
                buf_append(&out, p, 7); p += 7; free(params); free(rettype); continue;
            }
            scan += 6;
            scan = skip_ws(scan);
            if (*scan != '"') { buf_append(&out, p, 7); p += 7; free(params); free(rettype); continue; }
            scan++;
            const char *csym_start = scan;
            while (*scan && *scan != '"') scan++;
            if (*scan != '"') { buf_append(&out, p, 7); p += 7; free(params); free(rettype); continue; }
            size_t csym_len = (size_t)(scan - csym_start);
            char *csym = malloc(csym_len + 1);
            memcpy(csym, csym_start, csym_len);
            csym[csym_len] = '\0';
            scan++; /* closing quote */

            char *lib = NULL;
            scan = skip_ws(scan);
            if (starts_with_word(scan, "from")) {
                scan += 4;
                scan = skip_ws(scan);
                if (*scan != '"') { buf_append(&out, p, 7); p += 7; free(params); free(rettype); free(csym); continue; }
                scan++;
                const char *lib_start = scan;
                while (*scan && *scan != '"') scan++;
                if (*scan != '"') { buf_append(&out, p, 7); p += 7; free(params); free(rettype); free(csym); continue; }
                size_t lib_len = (size_t)(scan - lib_start);
                lib = malloc(lib_len + 1);
                memcpy(lib, lib_start, lib_len);
                lib[lib_len] = '\0';
                scan++;
                scan = skip_ws(scan);
            }

            if (*scan != ';') {
                buf_append(&out, p, 7); p += 7;
                free(params); free(rettype); free(csym); free(lib);
                continue;
            }
            scan++; /* consume ';' */

            /* NAME need not equal the C symbol: when they differ, `keyword`
               emits a hidden `extern fn` declared under the *real* csymbol
               (so codegen's `$<name>` call reaches the actual foreign
               symbol, exactly like a hand-written `extern fn <csymbol>`
               would) plus a tiny `efn` forwarding wrapper under the
               public NAME that just calls it — so call sites still read
               `NAME(args)` naturally, no different from the NAME==csymbol
               case. Both pieces are ordinary, already-existing grammar
               (`extern_decl` + `efn`), so this still adds no new AST node,
               token, or semantic/codegen case; it only ever emits text
               the parser already knows how to handle. The hidden extern's
               name is namespaced (`__kw_ffi_<csymbol>`) to avoid colliding
               with a real top-level `<csymbol>` the user might also
               declare separately. */
            char *name = malloc(name_len + 1);
            memcpy(name, name_start, name_len);
            name[name_len] = '\0';

            Buf repl;
            buf_init(&repl);
            if (lib) {
                buf_append_str(&repl, "link \"");
                buf_append_str(&repl, lib);
                buf_append_str(&repl, "\";\n");
            }

            if (strcmp(name, csym) == 0) {
                buf_append_str(&repl, "extern fn ");
                buf_append_str(&repl, name);
                buf_append_str(&repl, "(");
                buf_append_str(&repl, params);
                buf_append_str(&repl, "): ");
                buf_append_str(&repl, rettype);
                buf_append_str(&repl, ";\n");
            } else {
                /* The hidden extern MUST be declared under the real C
                   symbol name (csym) itself: codegen emits extern fn
                   calls as the bare `$<declared-name>` (no mangling), so
                   whatever identifier this `extern fn` is declared under
                   is exactly the symbol the linker will look for. An
                   earlier version of this code declared it under a
                   synthetic `__kw_ffi_<csym>` name instead — that name
                   doesn't exist in any library, so linking any renamed
                   keyword (e.g. `keyword mysqrt(...) => extern "sqrt"
                   from "m";`) always failed with `undefined reference to
                   __kw_ffi_sqrt`. Declaring it as plain `csym` fixes
                   this; a namespaced hidden name is unnecessary here
                   because `extern fn` identifiers live in the same
                   top-level namespace as everything else, and colliding
                   with a user's own top-level `csym` would already be
                   caught as an ordinary redeclaration error, same as a
                   hand-written `extern fn <csym>` would be. */
                const char *hidden = csym;

                /* Hidden extern under the real C symbol. */
                buf_append_str(&repl, "extern fn ");
                buf_append_str(&repl, hidden);
                buf_append_str(&repl, "(");
                buf_append_str(&repl, params);
                buf_append_str(&repl, "): ");
                buf_append_str(&repl, rettype);
                buf_append_str(&repl, ";\n");

                /* Public efn wrapper under NAME, forwarding every
                   parameter by name (positional order preserved) to the
                   hidden extern. Parameter names are re-parsed out of
                   `params` (already `IDENT COLON type[, ...]`-shaped, or
                   a trailing/only `...` which an efn wrapper can't
                   forward — variadic keyword renames aren't supported,
                   caught below with a clear error instead of emitting
                   bad code). */
                Buf argnames;
                buf_init(&argnames);
                const char *pp = params;
                int first = 1;
                int bad_variadic = 0;
                while (*pp) {
                    pp = skip_ws(pp);
                    if (*pp == '\0') break;
                    if (strncmp(pp, "...", 3) == 0) { bad_variadic = 1; break; }
                    const char *an_start = pp;
                    while (is_ident_char((unsigned char)*pp)) pp++;
                    size_t an_len = (size_t)(pp - an_start);
                    if (an_len == 0) break;
                    if (!first) buf_append_str(&argnames, ", ");
                    buf_append(&argnames, an_start, an_len);
                    first = 0;
                    /* skip to next ',' at depth 0 or end */
                    int depth2 = 0;
                    while (*pp && !(depth2 == 0 && *pp == ',')) {
                        if (*pp == '(') depth2++;
                        else if (*pp == ')') depth2--;
                        pp++;
                    }
                    if (*pp == ',') pp++;
                }

                if (bad_variadic) {
                    fprintf(stderr,
                        "rapidc: keyword '%s': renaming a variadic FFI symbol "
                        "(\"%s\") is not supported\n", name, csym);
                    free(argnames.data);
                    free(params); free(rettype); free(csym); free(lib); free(name);
                    free(out.data);
                    return NULL;
                }

                buf_append_str(&repl, "efn ");
                buf_append_str(&repl, name);
                buf_append_str(&repl, "(");
                buf_append_str(&repl, params);
                buf_append_str(&repl, ") => ");
                buf_append_str(&repl, hidden);
                buf_append_str(&repl, "(");
                buf_append_str(&repl, argnames.data);
                buf_append_str(&repl, ");\n");
                free(argnames.data);
            }

            buf_append_str(&out, repl.data);

            free(repl.data);
            free(params); free(rettype); free(csym); free(lib); free(name);
            (void)decl_start;
            p = scan;
            continue;
        }

        if (*p == '"') {
            const char *start = p;
            p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p == '"') p++;
            buf_append(&out, start, (size_t)(p - start));
            continue;
        }

        buf_append(&out, p, 1);
        p++;
    }

    return out.data;
}

/* ---------------------------------------------------------------------
   Module resolution driver
   --------------------------------------------------------------------- */

#define MAX_MODULES 256

typedef struct {
    char resolved_path[PATH_MAX];
    int in_progress; /* for cycle detection */
    int done;
} ModuleState;

static ModuleState g_modules[MAX_MODULES];
static int g_module_count = 0;
static int g_mangle_counter = 0;

static void dirname_of(const char *path, char *out, size_t outsz) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *d = dirname(tmp);
    snprintf(out, outsz, "%s", d);
}

static void join_path(const char *dir, const char *rel, char *out, size_t outsz) {
    if (rel[0] == '/') {
        snprintf(out, outsz, "%s", rel);
    } else {
        snprintf(out, outsz, "%s/%s", dir, rel);
    }
}

/* M31: scan `stripped` (comments already removed, same buffer scan_top_level
   reads) for every top-level `shim "path.c";` directive, resolve each path
   relative to `this_dir` (the declaring file's own directory — identical
   convention to how `use "...";` paths are resolved a few lines below in
   resolve_file), and register the resolved absolute path onto
   g_program.link_objs via link_obj_add (which already de-duplicates, so a
   shim reached via `use` from several files, or declared more than once,
   is only ever recorded once). This is a separate, narrower scan than
   scan_top_level's fn/efn/struct/enum/use pass on purpose: shim directives
   don't participate in name mangling or the alias namespace at all, they
   only ever feed main.c's link step, so there is no reason to thread them
   through TopDecl/mangle_module_text. realpath() is used (not a plain
   join) so a shim reached two different textual ways (e.g. "./x.c" vs
   "x.c", or via two different `use` chains that both resolve to the same
   file) still de-duplicates correctly against link_obj_add's strcmp. A
   shim path that doesn't exist on disk is reported here immediately
   (clearer than deferring to the eventual `cc -c` failure in main.c, which
   would otherwise only say "no such file" without pointing at the
   declaring .rapid file or line). */
static int scan_shim_decls(const char *src, const char *this_dir) {
    const char *p = src;
    while (*p) {
        p = skip_ws(p);
        if (!*p) break;

        if (starts_with_word(p, "shim")) {
            const char *save = p;
            p += strlen("shim");
            p = skip_ws(p);
            if (*p != '"') { p = save + 1; continue; }
            const char *pstart = ++p;
            while (*p && *p != '"') p++;
            if (*p != '"') { p = save + 1; continue; }
            size_t plen = (size_t)(p - pstart);
            char rel[PATH_MAX];
            if (plen >= sizeof(rel)) plen = sizeof(rel) - 1;
            memcpy(rel, pstart, plen);
            rel[plen] = '\0';
            p++; /* closing quote */
            p = skip_ws(p);
            if (*p == ';') p++;

            char joined[PATH_MAX];
            join_path(this_dir, rel, joined, sizeof(joined));
            char abs[PATH_MAX];
            if (!realpath(joined, abs)) {
                fprintf(stderr, "rapidc: shim \"%s\": no such file (resolved to %s)\n", rel, joined);
                return 0;
            }
            link_obj_add(abs);
            continue;
        }

        /* Skip past anything else one identifier/char at a time — this
           scan only needs to find `shim` tokens, everything else (fn
           bodies, other directives, string/char literals already stripped
           of comments) is irrelevant and safe to step over character by
           character. */
        if (is_ident_char((unsigned char)*p)) {
            while (is_ident_char((unsigned char)*p)) p++;
        } else {
            p++;
        }
    }
    return 1;
}

/* Extract every `use "path"[ as alias];` directive that appears at
   top-level (i.e. not inside a comment/string — strip_comments already
   removed comments; strings inside a use directive are the path itself,
   which we deliberately still parse via the quotes). Also collects every
   top-level fn/efn/struct/enum name (with private flag) declared in this
   file, in source order, so the caller can mangle them. */
static void scan_top_level(const char *src, UseDirective **uses, int *use_count,
                            TopDecl **decls, int *decl_count) {
    int uc = 0, ucap = 8;
    UseDirective *ulist = malloc(sizeof(UseDirective) * ucap);
    int dc = 0, dcap = 16;
    TopDecl *dlist = malloc(sizeof(TopDecl) * dcap);

    const char *p = src;
    while (*p) {
        p = skip_ws(p);
        if (!*p) break;

        int is_private = 0;
        const char *save = p;
        if (starts_with_word(p, "private")) {
            is_private = 1;
            p += strlen("private");
            p = skip_ws(p);
        }

        if (starts_with_word(p, "use")) {
            p += strlen("use");
            p = skip_ws(p);
            if (*p == '"') {
                const char *pstart = ++p;
                while (*p && *p != '"') p++;
                size_t plen = (size_t)(p - pstart);
                char *pathbuf = malloc(plen + 1);
                memcpy(pathbuf, pstart, plen);
                pathbuf[plen] = '\0';
                if (*p == '"') p++;
                p = skip_ws(p);
                char *alias = NULL;
                if (starts_with_word(p, "as")) {
                    p += strlen("as");
                    p = skip_ws(p);
                    const char *astart = p;
                    while (is_ident_char((unsigned char)*p)) p++;
                    size_t alen = (size_t)(p - astart);
                    alias = malloc(alen + 1);
                    memcpy(alias, astart, alen);
                    alias[alen] = '\0';
                    p = skip_ws(p);
                }
                if (*p == ';') p++;
                if (uc >= ucap) { ucap *= 2; ulist = realloc(ulist, sizeof(UseDirective) * ucap); }
                ulist[uc].path = pathbuf;
                ulist[uc].alias = alias;
                uc++;
                continue;
            } else {
                /* malformed use, let the real parser report it later */
                p = save + 1;
                continue;
            }
        }

        if (starts_with_word(p, "fn") || starts_with_word(p, "efn")) {
            int is_efn = starts_with_word(p, "efn");
            p += is_efn ? strlen("efn") : strlen("fn");
            p = skip_ws(p);
            const char *nstart = p;
            while (is_ident_char((unsigned char)*p)) p++;
            size_t nlen = (size_t)(p - nstart);
            if (nlen > 0) {
                if (dc >= dcap) { dcap *= 2; dlist = realloc(dlist, sizeof(TopDecl) * dcap); }
                dlist[dc].name = malloc(nlen + 1);
                memcpy(dlist[dc].name, nstart, nlen);
                dlist[dc].name[nlen] = '\0';
                dlist[dc].is_private = is_private;
                dc++;
            }
            /* Don't try to skip the body here — the outer loop just keeps
               scanning char-by-char for the next top-level keyword; since
               we only match "fn"/"efn"/"struct"/"enum"/"use"/"private" right
               after whitespace, and those five words won't spuriously occur
               at the start of a scan position inside a function body except
               as the *token* fn/struct/enum/use themselves (which cannot be
               legally nested at top-level scope in this grammar anyway),
               this naive scan is safe for well-formed input. */
            continue;
        }

        /* M29 fix: `extern fn NAME(...)` and `keyword NAME(...)` are also
           top-level, callable, alias-mangleable names — the exact same
           namespace as `fn`/`efn` (an importer writes `alias::NAME(...)`
           for any of the four alike; the language never distinguishes
           "this call target happens to be FFI-backed"). Before this fix
           scan_top_level only knew about fn/efn/struct/enum, so an
           aliased `use "net.rapid" as net;` would rewrite call sites to
           `net__http_get(...)` (via rewrite_alias_references, which is
           purely textual and doesn't care what kind of decl NAME is)
           while the declaration site's own text — mangled here — never
           got renamed to `net__http_get`, since http_get was invisible
           to mangle_module_text's decls list. Declaring it here closes
           that gap: an extern/keyword NAME is scanned as an ordinary
           TopDecl, so mangle_module_text renames its declaration exactly
           the way it already renames a public fn/efn, and the two sides
           (declaration, call site) agree again. Note keyword's own
           `=> extern "csymbol"` rename machinery (expand_keyword_decls)
           runs later, after all alias mangling, purely as a textual
           `keyword <NAME>(...)` scan that doesn't inspect NAME's
           content — so it transparently keeps working on an
           already-mangled NAME (e.g. `net__http_get`) with no change
           needed there. */
        if (starts_with_word(p, "extern")) {
            const char *save_extern = p;
            p += strlen("extern");
            p = skip_ws(p);
            if (starts_with_word(p, "fn")) {
                p += strlen("fn");
                p = skip_ws(p);
                const char *nstart = p;
                while (is_ident_char((unsigned char)*p)) p++;
                size_t nlen = (size_t)(p - nstart);
                if (nlen > 0) {
                    if (dc >= dcap) { dcap *= 2; dlist = realloc(dlist, sizeof(TopDecl) * dcap); }
                    dlist[dc].name = malloc(nlen + 1);
                    memcpy(dlist[dc].name, nstart, nlen);
                    dlist[dc].name[nlen] = '\0';
                    dlist[dc].is_private = is_private;
                    dc++;
                }
                continue;
            }
            /* `extern` not followed by `fn` isn't a decl this grammar
               knows about; let the real parser report it. Rewind past
               just the `extern` word we consumed so the char-by-char
               fallback below still advances (never re-loops forever). */
            p = save_extern;
            p++;
            continue;
        }

        if (starts_with_word(p, "keyword")) {
            p += strlen("keyword");
            p = skip_ws(p);
            const char *nstart = p;
            while (is_ident_char((unsigned char)*p)) p++;
            size_t nlen = (size_t)(p - nstart);
            if (nlen > 0) {
                if (dc >= dcap) { dcap *= 2; dlist = realloc(dlist, sizeof(TopDecl) * dcap); }
                dlist[dc].name = malloc(nlen + 1);
                memcpy(dlist[dc].name, nstart, nlen);
                dlist[dc].name[nlen] = '\0';
                dlist[dc].is_private = is_private;
                dc++;
            }
            continue;
        }

        if (starts_with_word(p, "struct") || starts_with_word(p, "enum")) {
            int kw_len = starts_with_word(p, "struct") ? (int)strlen("struct") : (int)strlen("enum");
            p += kw_len;
            p = skip_ws(p);
            const char *nstart = p;
            while (is_ident_char((unsigned char)*p)) p++;
            size_t nlen = (size_t)(p - nstart);
            if (nlen > 0) {
                if (dc >= dcap) { dcap *= 2; dlist = realloc(dlist, sizeof(TopDecl) * dcap); }
                dlist[dc].name = malloc(nlen + 1);
                memcpy(dlist[dc].name, nstart, nlen);
                dlist[dc].name[nlen] = '\0';
                dlist[dc].is_private = is_private;
                dc++;
            }
            continue;
        }

        /* Not a recognized top-level starter; advance one char (skips over
           closing braces of previous function bodies etc). */
        p++;
    }

    *uses = ulist;
    *use_count = uc;
    *decls = dlist;
    *decl_count = dc;
}

static char *resolve_file(const char *path, int is_entry, const char *alias_for_this_import);

/* Applies private-mangling and (if this module was imported via `use ...
   as alias;`) public-prefix-mangling to `text`, based on the
   previously-scanned `decls`. Both kinds of renames are applied to the
   module's OWN text (so the declaration site and every internal
   self-reference agree), and public+aliased names use the exact same
   `alias__name` scheme the importer's call sites get rewritten to via
   rewrite_alias_references() below — so the two sides always match.
   `main`, when this isn't the entry file, is treated as always-private
   (a module cannot supply the program's entry point). */
static char *mangle_module_text(const char *text, TopDecl *decls, int decl_count,
                                 int is_entry, const char *alias) {
    char *cur = strdup(text);
    int mod_id = g_mangle_counter++;

    for (int i = 0; i < decl_count; i++) {
        TopDecl *d = &decls[i];
        int force_private = (!is_entry && strcmp(d->name, "main") == 0);
        int priv = d->is_private || force_private;

        char to[512];
        if (priv) {
            snprintf(to, sizeof(to), "%s__priv%d", d->name, mod_id);
        } else if (alias) {
            snprintf(to, sizeof(to), "%s__%s", alias, d->name);
        } else {
            continue; /* public, no alias: keep the name as-is */
        }

        char *next = rename_ident_real(cur, d->name, to);
        free(cur);
        cur = next;
    }

    return cur;
}

/* Rewrites, in `importer_text`, every `alias::name(...)`/`alias::Name`
   occurrence to `alias__name` — the exact mangled form mangle_module_text
   gives that module's public declarations when it was resolved with the
   same alias. Private names in the imported module are unreachable this
   way since they were mangled to `name__privN` instead, a form
   `alias::name` textually can never produce. */
static char *rewrite_alias_references(const char *importer_text, const char *alias) {
    char prefix[300];
    snprintf(prefix, sizeof(prefix), "%s__", alias);
    return replace_alias_calls(importer_text, alias, prefix);
}

/* Resolves one file: reads it, strips comments, scans use-directives and
   top-level decls, recursively resolves each import (passing down the
   alias, if any, that *this file's* `use` line gave that import), rewrites
   this file's own references to `alias::x` to match, mangles this file's
   own private symbols and (if `self_alias` is non-NULL, i.e. this file was
   itself imported with an alias) its own public symbols, and returns:
     [ merged text of all imports, in first-use order ] + [ this file's own mangled text ]
   so imports always appear before the file that uses them (matches a
   simple single-pass, dependency-first link order). Compile-once: a file
   already fully resolved earlier in the build contributes empty text on
   later imports, so its declarations aren't duplicated. */
static char *resolve_file(const char *path, int is_entry, const char *self_alias) {
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        fprintf(stderr, "rapidc: cannot find module file %s\n", path);
        return NULL;
    }

    for (int i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i].resolved_path, resolved) == 0) {
            if (g_modules[i].in_progress) {
                fprintf(stderr, "rapidc: circular use of module %s\n", resolved);
                return NULL;
            }
            if (g_modules[i].done) {
                return strdup("");
            }
        }
    }

    if (g_module_count >= MAX_MODULES) {
        fprintf(stderr, "rapidc: too many modules (milestone limit)\n");
        return NULL;
    }
    int slot = g_module_count++;
    snprintf(g_modules[slot].resolved_path, sizeof(g_modules[slot].resolved_path), "%s", resolved);
    g_modules[slot].in_progress = 1;
    g_modules[slot].done = 0;

    char *raw = read_whole_file(resolved);
    if (!raw) {
        fprintf(stderr, "rapidc: cannot open module file %s\n", resolved);
        return NULL;
    }

    char *stripped = strip_comments(raw);

    UseDirective *uses; int use_count;
    TopDecl *decls; int decl_count;
    scan_top_level(stripped, &uses, &use_count, &decls, &decl_count);

    char this_dir[PATH_MAX];
    dirname_of(resolved, this_dir, sizeof(this_dir));

    if (!scan_shim_decls(stripped, this_dir)) {
        free(raw); free(stripped);
        for (int k = 0; k < use_count; k++) { free(uses[k].path); if (uses[k].alias) free(uses[k].alias); }
        free(uses);
        for (int k = 0; k < decl_count; k++) free(decls[k].name);
        free(decls);
        return NULL;
    }

    Buf imports_merged;
    buf_init(&imports_merged);

    char *own_text = strdup(raw);

    for (int i = 0; i < use_count; i++) {
        char child_path[PATH_MAX];
        join_path(this_dir, uses[i].path, child_path, sizeof(child_path));

        char *child_merged = resolve_file(child_path, 0, uses[i].alias);
        if (!child_merged) {
            free(raw); free(stripped); free(own_text);
            for (int k = 0; k < use_count; k++) { free(uses[k].path); if (uses[k].alias) free(uses[k].alias); }
            free(uses);
            for (int k = 0; k < decl_count; k++) free(decls[k].name);
            free(decls);
            return NULL;
        }
        buf_append_str(&imports_merged, child_merged);
        buf_append_str(&imports_merged, "\n");
        free(child_merged);

        if (uses[i].alias) {
            /* This file called that import `alias`, so every `alias::name`
               written in THIS file's own text must become `alias__name` to
               match how the child's public declarations were just mangled
               (see the self_alias handling below, applied on the child's
               own recursive call). */
            char *rewritten = rewrite_alias_references(own_text, uses[i].alias);
            free(own_text);
            own_text = rewritten;
        }
    }

    /* Mangle this file's own private names (always) and, if some importer
       gave this file an alias, this file's own public names too — using
       the exact same file-unique mod_id-based scheme for private names
       regardless, so two different files' same-named private helpers
       never collide even though both are textually merged into one
       compile unit. */
    char *self_mangled = mangle_module_text(own_text, decls, decl_count, is_entry, self_alias);
    free(own_text);

    Buf result;
    buf_init(&result);
    buf_append_str(&result, imports_merged.data);
    buf_append_str(&result, self_mangled);

    free(imports_merged.data);
    free(self_mangled);
    free(raw);
    free(stripped);
    for (int i = 0; i < use_count; i++) { free(uses[i].path); if (uses[i].alias) free(uses[i].alias); }
    free(uses);
    for (int i = 0; i < decl_count; i++) free(decls[i].name);
    free(decls);

    g_modules[slot].in_progress = 0;
    g_modules[slot].done = 1;

    return result.data;
}

char *resolve_modules(const char *entry_path) {
    g_module_count = 0;
    g_mangle_counter = 0;
    char *merged = resolve_file(entry_path, 1, NULL);
    if (!merged) return NULL;

    /* M29: expand `keyword ...;` declarations (see expand_keyword_decls)
       as the very last step, once the whole program is a single text
       blob — this way a `keyword` declared in an imported file works
       exactly like one declared in the entry file. */
    char *expanded = expand_keyword_decls(merged);
    free(merged);
    return expanded;
}
