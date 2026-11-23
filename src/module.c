#include "module.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  ifndef PATH_MAX
#    define PATH_MAX MAX_PATH
#  endif
static char *realpath(const char *path, char *resolved) {
    DWORD r = GetFullPathNameA(path, PATH_MAX, resolved, NULL);
    if (r == 0 || r >= PATH_MAX) return NULL;
    return resolved;
}
#endif
#include <ctype.h>
#include <libgen.h>
#include <limits.h> /* sistem limits.h: PATH_MAX vb. */
/* rapidc'in kendi limit sabitleri AYRI bir dosyada: "limits.h" diye
   adlandirilsaydi, cc -Isrc hem <...> hem "..." icin src/'i aradigindan
   (bu projede -I boyle davraniyor, test edildi) sistem limits.h'in yerine
   GECERDI ve PATH_MAX gibi sembolleri tanimsiz birakirdi -- bu yuzden
   rapidc_limits.h adi kullanildi, isim cakismasi tamamen ortadan kalkti. */
#include "rapidc_limits.h" /* RAPIDC_MAX_MODULES / RAPIDC_MAX_SUBALIASES / RAPIDC_MAX_GLOBAL_ALIASES */
#include <unistd.h>

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

/* Forward decls: these operate on the module table (defined further
   below, alongside ModuleState) but are needed by replace_alias_calls,
   which appears earlier in the file for historical/diff-minimalism
   reasons (it's the pre-existing single-level rewrite, extended in
   place rather than moved). */
static const char *lookup_subalias(int slot, const char *alias);
static int find_slot_by_prefix(const char *prefix);

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
    /* M36: struct govdesinde ilk alandan sonraki alanlar `;` ile ayrilir
       (`a: int; count: int;`), yani onceki bosluk-olmayan karakter `;`.
       Eskiden bu durum yakalanmiyordu: dosyada `fn count` varken struct
       alani `count` de `alias__count`'a cevriliyor, `m.count` erisimleri ise
       cevrilmedigi icin ("no such field") derleme kirilyordu. `;` + IDENT + `:`
       kalibi bir ifade baglaminda ancak bir alan bildirimidir: `var`/`const`
       bildirimlerinde `:`'dan once ident, `var`/`const` anahtar kelimesinden
       SONRA gelir (onceki karakter harf), parametre listesinde `(`/`,` sonrasi
       gelir. Dolayisiyla `;` hemen ardindan gelen `IDENT :` yalniz struct alanidir. */
    if (next == ':' && prev == ';') return 1;
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

/* Reads one `::<ident>` continuation starting at `p` (which must point at
   the first `:`). On a match, returns a pointer just past the identifier
   and writes the identifier's bounds to * id_start / * id_len; otherwise
   returns NULL and leaves the out-params untouched. */
static const char *match_cc_ident(const char *p, const char **id_start, size_t *id_len) {
    if (p[0] != ':' || p[1] != ':') return NULL;
    const char *after_cc = p + 2;
    if (!is_ident_char(*after_cc) || isdigit((unsigned char)*after_cc)) return NULL;
    const char *q = after_cc;
    while (is_ident_char(*q)) q++;
    *id_start = after_cc;
    *id_len = (size_t)(q - after_cc);
    return q;
}

/* Replace every occurrence of a qualified reference rooted at `alias` -
   either the simple `alias::name` this function has always handled, or
   (new) a deeper `alias::mid1::mid2::...::name` chain - with the fully
   resolved mangled leaf name. Comment/string-literal aware, matching the
   rest of this pass. `slot` is the module whose own subalias table deeper
   segments should be walked against; pass -1 to disable multi-level
   resolution and keep the old single-hop-only behavior (used nowhere
   currently, kept so single-level callers stay obviously correct). */
static char *replace_alias_calls(const char *src, const char *alias, const char *mangled_prefix, int slot) {
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
            if (word_len == alias_len && strncmp(start, alias, alias_len) == 0 &&
                p[0] == ':' && p[1] == ':') {
                /* Walk as many further `::ident` hops as this alias's
                   own subalias table (and each subsequent module's own
                   table) can account for. Each successful hop advances
                   `cursor` and switches `cur_prefix`/`cur_slot` to the
                   module that hop resolved into; the walk stops at the
                   first segment that ISN'T a known sub-alias in the
                   current module, and that stopping segment is the
                   real leaf identifier being called/referenced. */
                const char *cursor = p;
                char cur_prefix[300];
                snprintf(cur_prefix, sizeof(cur_prefix), "%s", mangled_prefix);
                int cur_slot = slot >= 0 ? find_slot_by_prefix(mangled_prefix) : -1;

                const char *leaf_start = NULL;
                size_t leaf_len = 0;

                for (;;) {
                    const char *id_start; size_t id_len;
                    const char *after = match_cc_ident(cursor, &id_start, &id_len);
                    if (!after) break; /* no more `::ident` - shouldn't happen on first hop */
                    /* Is `id_start[0..id_len)` itself followed by `::` AND
                       a known sub-alias of the module we're currently in?
                       If so it's an intermediate namespace segment, not
                       the leaf - resolve it and keep walking. Otherwise
                       this is the leaf: stop here, un-consumed. */
                    int is_further_hop = 0;
                    if (cur_slot >= 0 && after[0] == ':' && after[1] == ':') {
                        char seg[136];
                        snprintf(seg, sizeof(seg), "%.*s", (int)id_len, id_start);
                        const char *next_prefix = lookup_subalias(cur_slot, seg);
                        if (next_prefix) {
                            snprintf(cur_prefix, sizeof(cur_prefix), "%s", next_prefix);
                            cur_slot = find_slot_by_prefix(next_prefix);
                            cursor = after;
                            is_further_hop = 1;
                        }
                    }
                    if (!is_further_hop) {
                        leaf_start = id_start;
                        leaf_len = id_len;
                        cursor = after;
                        break;
                    }
                }

                if (leaf_start) {
                    buf_append_str(&out, cur_prefix);
                    buf_append(&out, leaf_start, leaf_len);
                    p = cursor;
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
    int is_extern;      /* 1 for a plain `extern fn NAME(...)` decl */
    char *ext_params;   /* raw text between the parens (extern only) */
    char *ext_ret;      /* raw return type text (extern only)        */
} TopDecl;

/* Very small helper: skip whitespace */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Cross-file extern signature registry.
   Two DIFFERENT files that each declare the same C symbol with the same
   signature (e.g. two libraries that both need libc `strlen`) are not a
   conflict: both declarations describe one foreign symbol. Every module
   records the normalized signature of each of its externs here, tagged
   with the module's own id; when a later module declares an identical
   signature, that later declaration is dropped from ITS text before the
   merge (see drop_cross_file_extern_dups). Two identical externs inside
   the SAME file are never dropped — that stays a genuine redeclaration
   error — because a hit only counts when the recorded module id differs.
   Signatures are normalized to `name|type,type,...|ret` so a differing
   parameter *name* (`s` vs `x`) is not mistaken for a conflict, while a
   differing parameter/return *type* still is. */
typedef struct { char *sig; int mod_id; } ExtSig;
static ExtSig *g_ext_sigs = NULL;
static int g_ext_sig_n = 0, g_ext_sig_cap = 0;

/* Strips parameter names from "a: int, b: string *" -> "int,string *". */
static char *normalize_ext_sig(const char *name, const char *params, const char *ret) {
    Buf b; buf_init(&b);
    buf_append_str(&b, name);
    buf_append_str(&b, "|");
    const char *p = params ? params : "";
    int first = 1;
    while (*p) {
        p = skip_ws(p);
        if (*p == '\0') break;
        const char *seg = p;
        int depth = 0;
        while (*p && !(depth == 0 && *p == ',')) {
            if (*p == '(') depth++; else if (*p == ')') depth--;
            p++;
        }
        const char *segend = p;
        if (*p == ',') p++;
        /* type part = text after the first ':' (or the whole seg for `...`) */
        const char *colon = NULL;
        for (const char *q = seg; q < segend; q++) if (*q == ':') { colon = q; break; }
        const char *ts = colon ? colon + 1 : seg;
        while (ts < segend && (*ts==' '||*ts=='\t'||*ts=='\n'||*ts=='\r')) ts++;
        const char *te = segend;
        while (te > ts && (te[-1]==' '||te[-1]=='\t'||te[-1]=='\n'||te[-1]=='\r')) te--;
        if (!first) buf_append_str(&b, ",");
        first = 0;
        /* collapse internal whitespace so `int *` == `int*` */
        for (const char *q = ts; q < te; q++)
            if (!(*q==' '||*q=='\t'||*q=='\n'||*q=='\r')) buf_append(&b, q, 1);
    }
    buf_append_str(&b, "|");
    const char *r = ret ? ret : "";
    for (; *r; r++)
        if (!(*r==' '||*r=='\t'||*r=='\n'||*r=='\r')) buf_append(&b, r, 1);
    return b.data;
}
/* 1 if an identical signature was already recorded by a DIFFERENT module. */
static int ext_sig_seen_in_other_module(const char *sig, int mod_id) {
    for (int i = 0; i < g_ext_sig_n; i++)
        if (g_ext_sigs[i].mod_id != mod_id && strcmp(g_ext_sigs[i].sig, sig) == 0) return 1;
    return 0;
}
static void ext_sig_record(char *sig, int mod_id) {
    if (g_ext_sig_n >= g_ext_sig_cap) {
        g_ext_sig_cap = g_ext_sig_cap ? g_ext_sig_cap * 2 : 16;
        g_ext_sigs = realloc(g_ext_sigs, sizeof(ExtSig) * (size_t)g_ext_sig_cap);
    }
    g_ext_sigs[g_ext_sig_n].sig = sig;
    g_ext_sigs[g_ext_sig_n].mod_id = mod_id;
    g_ext_sig_n++;
}
/* Variadic externs reached through a module alias. An `efn` forwarding
   wrapper can't forward `...`, so instead the importer's `alias__NAME`
   call sites are rewritten straight back to the real C symbol NAME after
   the merge (see rewrite_variadic_alias_calls). One entry per
   (alias, name) pair. */
typedef struct { char *mangled; char *real; } VariadicAlias;
static VariadicAlias *g_var_alias = NULL;
static int g_var_alias_n = 0, g_var_alias_cap = 0;
/* Registers `from` -> `real` (both plain identifiers). Duplicate `from`
   entries are ignored. */
static void var_alias_record_raw(const char *from, const char *real) {
    for (int i = 0; i < g_var_alias_n; i++)
        if (strcmp(g_var_alias[i].mangled, from) == 0) return;
    if (g_var_alias_n >= g_var_alias_cap) {
        g_var_alias_cap = g_var_alias_cap ? g_var_alias_cap * 2 : 8;
        g_var_alias = realloc(g_var_alias, sizeof(VariadicAlias) * (size_t)g_var_alias_cap);
    }
    g_var_alias[g_var_alias_n].mangled = strdup(from);
    g_var_alias[g_var_alias_n].real = strdup(real);
    g_var_alias_n++;
}
static void var_alias_record(const char *alias, const char *name) {
    size_t need = strlen(alias) + strlen(name) + 3;
    char *m = malloc(need);
    snprintf(m, need, "%s__%s", alias, name);
    var_alias_record_raw(m, name);
    free(m);
}

typedef struct {
    char *path;      /* raw string as written in the use directive */
    char *alias;     /* NULL if no alias */
} UseDirective;


static int starts_with_word(const char *p, const char *word) {
    size_t n = strlen(word);
    if (strncmp(p, word, n) != 0) return 0;
    return !is_ident_char((unsigned char)p[n]);
}

/* A module's own self-declared default prefix, written once at the top
   of the library file itself:
       module str;
   An importer that writes a plain `use "strlib.rapid";` (no `as alias`)
   then gets `str::name` for free, without ever having to spell out
   `as str` at every call site — the library author picks the prefix
   once, here, and every importer inherits it by default. An importer
   can still override it with an explicit `use "strlib.rapid" as s;`,
   which always wins over the module's own declared name. */
static char *scan_module_decl(const char *src) {
    const char *p = src;
    p = skip_ws(p);
    if (!*p) return NULL;
    if (starts_with_word(p, "module")) {
        p += strlen("module");
        p = skip_ws(p);
        const char *nstart = p;
        while (is_ident_char((unsigned char)*p)) p++;
        size_t nlen = (size_t)(p - nstart);
        if (nlen == 0) return NULL;
        char *name = malloc(nlen + 1);
        memcpy(name, nstart, nlen);
        name[nlen] = '\0';
        return name;
    }
    /* Only ever legal as the very first top-level statement in a
       file; anything else means there's no self-declared prefix. */
    return NULL;
}

/* M41: `module <name> global;` - kutuphanenin public isimleri MANGLE EDILMEZ
   (importer `toInt(x)` yazar) ve importer'daki `<name>::isim` onekli kullanimi
   da ayni isme cozulur (`std::toInt(x)` == `toInt(x)`). std/std.rapid bunu
   kullanir: eski builtin'lere onek olmadan erisim (legacy kod), onek ise
   opsiyonel. Acik `use "..." as takma_ad;` her zaman kazanir ve normal
   (mangle'li) aliasli import gibi davranir. */
static int module_decl_is_global(const char *src) {
    const char *p = skip_ws(src);
    if (!starts_with_word(p, "module")) return 0;
    p = skip_ws(p + strlen("module"));
    while (is_ident_char((unsigned char)*p)) p++;
    p = skip_ws(p);
    return starts_with_word(p, "global");
}

/* Strips a leading `module <name>;` declaration out of `src`, if present,
   returning a freshly-allocated copy of the rest (or a plain strdup when
   there's nothing to strip). scan_top_level/mangle_module_text/etc. never
   need to know about this directive since it's gone before they run. */
static char *strip_module_decl(const char *src) {
    const char *p = src;
    p = skip_ws(p);
    if (starts_with_word(p, "module")) {
        const char *q = p + strlen("module");
        q = skip_ws(q);
        while (is_ident_char((unsigned char)*q)) q++;
        q = skip_ws(q);
        if (starts_with_word(q, "global")) q = skip_ws(q + strlen("global")); /* M41 */
        if (*q == ';') {
            q++;
            return strdup(q);
        }
    }
    return strdup(src);
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
            if (!*p) continue; /* unterminated string: NUL reached, don't p++ past it */
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
                    /* An `efn` wrapper can't forward `...`, so a renamed
                       variadic keyword (`keyword myprintf(fmt: string, ...)
                       => extern "printf"`) can't use the wrapper trick.
                       Instead the hidden extern above already carries the
                       real symbol (with its `...`); record NAME -> csym so
                       every call site written as `NAME(...)` is mapped
                       straight onto the real symbol after the merge (same
                       mechanism as a variadic extern reached through a
                       module alias, see rewrite_variadic_alias_calls). No
                       wrapper is emitted. */
                    var_alias_record_raw(name, csym);
                    free(argnames.data);
                    buf_append_str(&out, repl.data);
                    free(repl.data);
                    free(params); free(rettype); free(csym); free(lib); free(name);
                    (void)decl_start;
                    p = scan;
                    continue;
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

#define MAX_MODULES RAPIDC_MAX_MODULES
#define MAX_SUBALIASES RAPIDC_MAX_SUBALIASES

/* One entry of a module's own `use ... as alias;` table: the alias name
   this file used for one of its own imports, and the mangled prefix
   (e.g. "interface__") that import's public declarations actually got
   mangled to. Recorded so that a *deeper* importer's `outer::inner::x`
   chain can be resolved by walking these tables one segment at a time,
   instead of the flat single-level `alias::x` -> `alias__x` textual
   rewrite that's all replace_alias_calls could do before. */
typedef struct {
    char alias[128];
    char prefix[136]; /* alias + "__" */
} SubAlias;

typedef struct {
    char resolved_path[PATH_MAX];
    int in_progress; /* for cycle detection */
    int done;
    char *declared_module_name; /* this file's own `module <name>;`, or NULL */
    char *resolved_effective_alias; /* the alias this file was actually
        resolved under this build: the importer's explicit `as alias`, or
        else this file's own `module <name>;`, or NULL if neither. Kept
        separately from declared_module_name because those two can
        differ (an importer's explicit alias always overrides the
        file's own declared name), and find_slot_by_prefix needs to
        match on whichever one was actually used for mangling. */
    SubAlias subaliases[MAX_SUBALIASES];
    int subalias_count;
    int subalias_limit_warned; /* Sorun-4 fix: print the MAX_SUBALIASES error once per module, not once per dropped alias */
} ModuleState;

static ModuleState g_modules[MAX_MODULES];
static int g_module_count = 0;
static int g_mangle_counter = 0;

/* M41: `module X global;` ile gelen kutuphane adlari (bkz. module_decl_is_global). */
#define MAX_GLOBAL_ALIASES RAPIDC_MAX_GLOBAL_ALIASES
static char *g_global_alias[MAX_GLOBAL_ALIASES];
static int g_global_alias_n = 0;
static int is_global_alias(const char *alias) {
    for (int i = 0; i < g_global_alias_n; i++)
        if (strcmp(g_global_alias[i], alias) == 0) return 1;
    return 0;
}
static void global_alias_add(const char *alias) {
    if (is_global_alias(alias)) return;
    /* Sorun-4 fix: this used to silently drop the alias past the limit,
       the only milestone limit in this file that didn't already error --
       a dropped global alias means later `module X global;` resolution
       quietly fails to recognize X, with no diagnostic pointing at why.
       Now matches MAX_MODULES' style: print and stop silently re-dropping
       further entries (module_resolve_errors/fatal flag not threaded this
       deep, so stderr + a process-wide latch is the local equivalent). */
    if (g_global_alias_n >= MAX_GLOBAL_ALIASES) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "rapidc: too many global module aliases (limit %d)\n", MAX_GLOBAL_ALIASES);
            warned = 1;
        }
        return;
    }
    g_global_alias[g_global_alias_n++] = strdup(alias);
}

/* M41: std/std.rapid gibi "yerlesik" kutuphaneler icin yedek arama kok dizini
   (rapidc'nin kendi dizini). Goreli `use` yolu yoksa buradan denenir. */
static char g_home_dir[PATH_MAX] = "";
static int g_auto_std = 1;
void module_set_auto_std(int on) { g_auto_std = on; }
void module_set_home(const char *dir) { snprintf(g_home_dir, sizeof(g_home_dir), "%s", dir ? dir : ""); }

/* Production-hardening fix: see module.h's module_set_allow_shim doc comment. */
static int g_allow_shim = 0;
void module_set_allow_shim(int on) { g_allow_shim = on; }

/* Records, on the module at `slot`, that `alias` (one of *its own*
   `use ... as alias;` imports, or that import's inherited `module <n>;`
   name) resolved to mangled prefix `child_effective_alias__`.
   Sorun-4 fix: past MAX_SUBALIASES this used to silently drop the entry
   (unlike every other milestone limit here, which errors). A dropped
   subalias means a later `alias::name` reference silently fails to
   resolve through it with no clue why it was never recorded, so this
   now reports once per module slot instead of dropping quietly. */
static void record_subalias(int slot, const char *alias, const char *child_effective_alias) {
    if (!alias || !child_effective_alias) return;
    if (g_modules[slot].subalias_count >= MAX_SUBALIASES) {
        if (!g_modules[slot].subalias_limit_warned) {
            fprintf(stderr, "rapidc: too many sub-aliases in one module (limit %d)\n", MAX_SUBALIASES);
            g_modules[slot].subalias_limit_warned = 1;
        }
        return;
    }
    SubAlias *sa = &g_modules[slot].subaliases[g_modules[slot].subalias_count++];
    snprintf(sa->alias, sizeof(sa->alias), "%s", alias);
    snprintf(sa->prefix, sizeof(sa->prefix), "%s__", child_effective_alias);
}

/* Looks up `alias` in the module at `slot`'s own subalias table. Returns
   the mangled prefix (e.g. "interface__") on a hit, or NULL. Used to walk
   a multi-segment `a::b::c` chain one hop at a time: having resolved the
   first hop to *some* module, we need that module's own table to resolve
   the next segment against, exactly mirroring how a human reader would
   open usb.rapid and look at what `interface` means *there*. */
static const char *lookup_subalias(int slot, const char *alias) {
    if (slot < 0) return NULL;
    for (int i = 0; i < g_modules[slot].subalias_count; i++) {
        if (strcmp(g_modules[slot].subaliases[i].alias, alias) == 0) {
            return g_modules[slot].subaliases[i].prefix;
        }
    }
    return NULL;
}

/* Finds the module slot whose *mangled prefix* is `prefix` (i.e. the
   module that was resolved with effective alias `prefix` minus its
   trailing "__"). Used after resolving one segment of a chain to find
   whose subalias table the *next* segment should be looked up in. */
static int find_slot_by_prefix(const char *prefix) {
    size_t plen = strlen(prefix);
    if (plen < 2 || prefix[plen - 1] != '_' || prefix[plen - 2] != '_') return -1;
    char alias_only[136];
    snprintf(alias_only, sizeof(alias_only), "%.*s", (int)(plen - 2), prefix);
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i].resolved_effective_alias &&
            strcmp(g_modules[i].resolved_effective_alias, alias_only) == 0) {
            return i;
        }
    }
    return -1;
}

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

            /* Production-hardening fix: a `shim "path.c";` directive gets
               that C file compiled and linked into the final binary
               (main.c). Compiling an untrusted .rapid file must not also
               silently compile-and-link arbitrary C it points at, so this
               is refused unless the caller explicitly opted in with
               `-allow-shim` (module_set_allow_shim). Checked before the
               realpath/exists check below so the refusal message is the
               only thing printed — no reason to also report on a shim
               path's existence when the directive itself is rejected. */
            if (!g_allow_shim) {
                fprintf(stderr,
                    "rapidc: shim \"%s\": refusing to compile and link an external C file "
                    "without -allow-shim (shim directives let a .rapid source file pull "
                    "arbitrary native code into the output binary)\n", rel);
                return 0;
            }

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
                    dlist[dc].is_extern = 0; dlist[dc].ext_params = NULL; dlist[dc].ext_ret = NULL;
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
                    dlist[dc].is_extern = 1; dlist[dc].ext_params = NULL; dlist[dc].ext_ret = NULL;
                    /* Capture `(params): ret;` so a forwarding wrapper can
                       be synthesized later (see mangle_module_text). */
                    const char *q = skip_ws(p);
                    if (*q == '(') {
                        const char *ps = ++q; int depth = 1;
                        while (*q && depth > 0) {
                            if (*q == '(') depth++;
                            else if (*q == ')') depth--;
                            if (depth > 0) q++;
                        }
                        size_t pl = (size_t)(q - ps);
                        char *pb = malloc(pl + 1); memcpy(pb, ps, pl); pb[pl] = '\0';
                        if (*q == ')') q++;
                        q = skip_ws(q);
                        char *rb = NULL;
                        if (*q == ':') {
                            q = skip_ws(q + 1);
                            const char *rs = q;
                            while (*q && *q != ';') q++;
                            size_t rl = (size_t)(q - rs);
                            while (rl > 0 && (rs[rl-1]==' '||rs[rl-1]=='\t'||rs[rl-1]=='\n'||rs[rl-1]=='\r')) rl--;
                            rb = malloc(rl + 1); memcpy(rb, rs, rl); rb[rl] = '\0';
                        }
                        dlist[dc].ext_params = pb;
                        dlist[dc].ext_ret = rb;
                    }
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
                    dlist[dc].is_extern = 0; dlist[dc].ext_params = NULL; dlist[dc].ext_ret = NULL;
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
                    dlist[dc].is_extern = 0; dlist[dc].ext_params = NULL; dlist[dc].ext_ret = NULL;
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

/* `alias_for_this_import` is the explicit alias from the importer's own
   `use ... as alias;`, or NULL if it wrote a plain `use "path";`. When
   NULL, and the target module has a self-declared `module <name>;` of
   its own, that self-declared name is used as the effective alias
   instead — so a plain `use "strlib.rapid";` gets `str::name` for free
   whenever strlib.rapid itself starts with `module str;`. Either way,
   the alias actually used (explicit override, self-declared default, or
   still NULL if neither exists) is written back through
   `out_effective_alias` so the caller knows what to rewrite `alias::x`
   references against. */
static char *resolve_file(const char *path, int is_entry, const char *alias_for_this_import,
                           char **out_effective_alias);


/* Walks THIS module's own (already-mangled) text, line by line. For every
   top-level `extern fn NAME(params): ret;` line it computes the normalized
   signature and:
     - drops the line if an identical signature was already recorded by a
       DIFFERENT module (cross-file duplicate of one foreign symbol), or
     - otherwise records it under this module's id and keeps it.
   The line is replaced by a blank line so error line numbers stay stable.
   Identical externs repeated inside this same module are all kept (only
   the first is recorded, and later ones match a same-module record, which
   never counts) so the semantic pass still reports the redeclaration. */
static char *drop_cross_file_extern_dups(const char *text, int mod_id) {
    Buf out; buf_init(&out);
    const char *p = text;
    while (*p) {
        const char *ls = p;
        while (*p && *p != '\n') p++;
        size_t ll = (size_t)(p - ls);
        int nl = (*p == '\n');
        const char *q = ls;
        while (q < ls + ll && (*q == ' ' || *q == '\t')) q++;
        int drop = 0;
        if ((size_t)(ls + ll - q) > 10 && strncmp(q, "extern fn", 9) == 0 &&
            (q[9] == ' ' || q[9] == '\t')) {
            const char *n = q + 9;
            while (n < ls + ll && (*n == ' ' || *n == '\t')) n++;
            const char *ns = n;
            while (n < ls + ll && is_ident_char((unsigned char)*n)) n++;
            size_t nlen = (size_t)(n - ns);
            const char *lp = n;
            while (lp < ls + ll && *lp != '(') lp++;
            if (nlen > 0 && lp < ls + ll) {
                /* matching ')' */
                const char *rp = lp + 1; int depth = 1;
                while (rp < ls + ll && depth > 0) {
                    if (*rp == '(') depth++; else if (*rp == ')') depth--;
                    if (depth > 0) rp++;
                }
                if (rp < ls + ll) {
                    char *name = malloc(nlen + 1); memcpy(name, ns, nlen); name[nlen] = '\0';
                    size_t pl = (size_t)(rp - (lp + 1));
                    char *params = malloc(pl + 1); memcpy(params, lp + 1, pl); params[pl] = '\0';
                    const char *rs = rp + 1;
                    while (rs < ls + ll && (*rs == ' ' || *rs == '\t')) rs++;
                    char *ret = NULL;
                    if (rs < ls + ll && *rs == ':') {
                        rs++;
                        while (rs < ls + ll && (*rs == ' ' || *rs == '\t')) rs++;
                        const char *re = rs;
                        while (re < ls + ll && *re != ';') re++;
                        size_t rl = (size_t)(re - rs);
                        ret = malloc(rl + 1); memcpy(ret, rs, rl); ret[rl] = '\0';
                    }
                    if (ret) {
                        char *sig = normalize_ext_sig(name, params, ret);
                        if (ext_sig_seen_in_other_module(sig, mod_id)) { drop = 1; free(sig); }
                        else ext_sig_record(sig, mod_id);
                        free(ret);
                    }
                    free(name); free(params);
                }
            }
        }
        if (!drop) buf_append(&out, ls, ll + (size_t)nl);
        else if (nl) buf_append(&out, "\n", 1);
        if (*p == '\n') p++;
    }
    return out.data;
}

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

        /* Plain `extern fn NAME(...)`: NAME is the *real C symbol* the
           linker resolves. Renaming it (NAME -> alias__NAME or
           NAME__privN) makes codegen emit `$alias__NAME`, which no
           library defines -> `undefined reference`. So the declaration
           and every in-module call keep the true symbol name; only the
           *importer-visible* alias name gets a forwarding wrapper,
           appended below. (Private externs need no wrapper: nothing
           outside this file may reference them.) */
        if (d->is_extern) {
            if (!priv && alias && d->ext_params && d->ext_ret) {
                /* skip variadic externs: an efn cannot forward `...` */
                if (strstr(d->ext_params, "...") != NULL) {
                    /* No wrapper possible; importer call sites are mapped
                       back onto the real symbol after the merge. */
                    var_alias_record(alias, d->name);
                    continue;
                }
                Buf args; buf_init(&args);
                const char *pp = d->ext_params; int first = 1;
                while (*pp) {
                    pp = skip_ws(pp);
                    if (*pp == '\0') break;
                    const char *as = pp;
                    while (is_ident_char((unsigned char)*pp)) pp++;
                    size_t al = (size_t)(pp - as);
                    if (al == 0) break;
                    if (!first) buf_append_str(&args, ", ");
                    buf_append(&args, as, al);
                    first = 0;
                    int dep = 0;
                    while (*pp && !(dep == 0 && *pp == ',')) {
                        if (*pp == '(') dep++; else if (*pp == ')') dep--;
                        pp++;
                    }
                    if (*pp == ',') pp++;
                }
                Buf w; buf_init(&w);
                buf_append_str(&w, "\nefn ");
                buf_append_str(&w, alias); buf_append_str(&w, "__");
                buf_append_str(&w, d->name);
                buf_append_str(&w, "(");
                buf_append_str(&w, d->ext_params);
                buf_append_str(&w, ") => ");
                buf_append_str(&w, d->name);
                buf_append_str(&w, "(");
                buf_append_str(&w, args.data ? args.data : "");
                buf_append_str(&w, ");\n");
                size_t nl = strlen(cur), wl = strlen(w.data);
                char *grown = malloc(nl + wl + 1);
                memcpy(grown, cur, nl); memcpy(grown + nl, w.data, wl + 1);
                free(cur); cur = grown;
                free(args.data); free(w.data);
            }
            continue;
        }

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

    /* Expand this module's own `keyword` declarations NOW (they are
       otherwise only expanded once, on the fully merged text), so the
       `extern fn` lines they produce exist as real, file-tagged externs
       when the cross-file dedupe below looks for duplicates — e.g. a
       `keyword strlen(...) => extern "strlen"` in one file and a plain
       `extern fn strlen(...)` in another are the same foreign symbol.
       Runs after private/alias mangling, so a renaming keyword's public
       `efn` wrapper is already emitted under its mangled name. A malformed
       keyword makes the expander return NULL; in that case leave the text
       untouched so the final merged-text pass still reports it exactly as
       before. */
    {
        char *expanded_kw = expand_keyword_decls(cur);
        if (expanded_kw) { free(cur); cur = expanded_kw; }
    }

    /* Cross-file extern dedupe (see drop_cross_file_extern_dups). Runs on
       every module, aliased or not, after all renames/wrapper appends. */
    {
        char *deduped = drop_cross_file_extern_dups(cur, mod_id);
        free(cur);
        cur = deduped;
    }

    return cur;
}

/* Rewrites, in `importer_text`, every `alias::name(...)`/`alias::Name`
   occurrence to `alias__name` — the exact mangled form mangle_module_text
   gives that module's public declarations when it was resolved with the
   same alias. Private names in the imported module are unreachable this
   way since they were mangled to `name__privN` instead, a form
   `alias::name` textually can never produce.

   As of the two-level namespace support, also rewrites deeper chains
   `alias::mid::...::name`, by walking `alias`'s own subalias table (see
   record_subalias/lookup_subalias) one `::segment` hop at a time — a
   real resolution against each module's actual `use ... as` imports,
   not a textual join of however many idents happen to appear before
   the final `(`. `slot` identifies the referencing module (whose own
   subalias table the first hop after `alias` is checked against). */
static char *rewrite_alias_references(const char *importer_text, const char *alias, int slot) {
    char prefix[300];
    if (is_global_alias(alias)) prefix[0] = '\0'; /* M41: `std::x` -> `x` */
    else snprintf(prefix, sizeof(prefix), "%s__", alias);
    return replace_alias_calls(importer_text, alias, prefix, slot);
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
static char *resolve_file(const char *path, int is_entry, const char *self_alias,
                           char **out_effective_alias) {
    if (out_effective_alias) *out_effective_alias = NULL;
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
                /* M41: std otomatik dahil edildiyse acik `use ... as s;` yalnizca
                   ek bir onektir: `s::x` -> `x`. */
                if (self_alias && g_modules[i].declared_module_name &&
                    is_global_alias(g_modules[i].declared_module_name))
                    global_alias_add(self_alias);
                if (out_effective_alias) {
                    out_effective_alias[0] = self_alias
                        ? strdup(self_alias)
                        : (g_modules[i].declared_module_name
                               ? strdup(g_modules[i].declared_module_name)
                               : NULL);
                }
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
    g_modules[slot].declared_module_name = NULL;
    g_modules[slot].resolved_effective_alias = NULL;
    g_modules[slot].subalias_count = 0;
    g_modules[slot].subalias_limit_warned = 0;

    char *raw_full = read_whole_file(resolved);
    if (!raw_full) {
        fprintf(stderr, "rapidc: cannot open module file %s\n", resolved);
        return NULL;
    }

    /* A leading `module <name>;` is this file's own self-declared default
       prefix (see scan_module_decl). It's detected against a comment-free
       view so a stray "module x;" inside a comment above the real code
       can't be mistaken for the directive, but stripped out of the actual
       `raw` text that gets compiled/mangled, since it isn't a real
       top-level statement the rest of the pipeline understands. */
    char *precheck_stripped = strip_comments(raw_full);
    char *declared_module_name = scan_module_decl(precheck_stripped);
    int module_is_global = declared_module_name && !self_alias &&
                           module_decl_is_global(precheck_stripped); /* M41 */
    free(precheck_stripped);
    if (module_is_global) global_alias_add(declared_module_name);
    g_modules[slot].declared_module_name = declared_module_name ? strdup(declared_module_name) : NULL;

    /* The alias actually in effect for THIS module: an explicit
       `use ... as alias;` from the importer always wins; otherwise fall
       back to this file's own `module <name>;`, if it declared one. */
    const char *effective_alias = self_alias ? self_alias : declared_module_name;
    if (out_effective_alias) {
        *out_effective_alias = effective_alias ? strdup(effective_alias) : NULL;
    }
    /* M41: global modul mangle edilmez -> prefix tablosunda gorunmez. */
    g_modules[slot].resolved_effective_alias =
        (effective_alias && !module_is_global) ? strdup(effective_alias) : NULL;

    char *raw = strip_module_decl(raw_full);
    free(raw_full);

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
        /* M41: goreli yol yoksa rapidc'nin kendi dizininde ara; boylece
           `use "std/std.rapid";` her dizinden calisir. */
        if (uses[i].path[0] != '/' && g_home_dir[0] && access(child_path, R_OK) != 0) {
            char alt[PATH_MAX];
            /* Sorun-5 fix: check snprintf's return so a g_home_dir/path
               combo too long for PATH_MAX is treated as "no such fallback
               path" (skip it, fall through to the original child_path and
               its normal file-not-found error) instead of silently
               proceeding with a truncated, likely-nonexistent-or-wrong
               path. snprintf returns the length it WOULD have written;
               >= sizeof(alt) means truncation occurred. */
            int n = snprintf(alt, sizeof(alt), "%s/%s", g_home_dir, uses[i].path);
            if (n >= 0 && (size_t)n < sizeof(alt) && access(alt, R_OK) == 0) {
                snprintf(child_path, sizeof(child_path), "%s", alt);
            }
        }

        char *child_effective_alias = NULL;
        char *child_merged = resolve_file(child_path, 0, uses[i].alias, &child_effective_alias);
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

        /* `child_effective_alias` is `uses[i].alias` when this file gave
           an explicit `as alias`, or (when it didn't) the child module's
           own self-declared `module <name>;`, or NULL if neither exists.
           Either way it's the exact prefix the child's public decls were
           just mangled with, so rewriting THIS file's `alias::name` (or
           `module_name::name`) references against it always matches. */
        if (child_effective_alias) {
            char *rewritten = rewrite_alias_references(own_text, child_effective_alias, slot);
            free(own_text);
            own_text = rewritten;

            /* Record this module's own view of `child_effective_alias`
               so a *deeper* importer's outer::child_effective_alias::x
               chain can later be walked one hop into this file's table
               (see resolve_qualified_chain). Without this, a two-level
               call like usb::interface::claim(...) had no way to learn,
               from outside usb.rapid, that `interface` means the module
               mangled to "interface__" *inside* usb.rapid specifically -
               that mapping only ever existed as a local rewrite done to
               usb.rapid's own text and was discarded once resolve_file
               returned. */
            if (!is_global_alias(child_effective_alias))
                record_subalias(slot, child_effective_alias, child_effective_alias);
        }
        free(child_effective_alias);
    }

    /* Mangle this file's own private names (always) and, if some importer
       gave this file an alias, this file's own public names too — using
       the exact same file-unique mod_id-based scheme for private names
       regardless, so two different files' same-named private helpers
       never collide even though both are textually merged into one
       compile unit. */
    char *self_mangled = mangle_module_text(own_text, decls, decl_count, is_entry,
                                            module_is_global ? NULL : effective_alias);
    free(own_text);
    free(declared_module_name);

    Buf result;
    buf_init(&result);
    buf_append_str(&result, imports_merged.data);
    {
        /* M42: bu modulun kendi metninden once konum isareti: lexer satir
           sayacini 1'e ve dosya adini `resolved`a ayarlar, boylece hata
           mesajlari birlestirilmis metnin degil gercek dosyanin satirini
           gosterir. Isaret satiri lexer'da yorum gibi tuketilir. */
        char safe[PATH_MAX];
        size_t k = 0;
        for (const char *q = resolved; *q && k + 1 < sizeof(safe); q++)
            safe[k++] = (*q == '\n' || *q == '\r') ? '_' : *q;
        safe[k] = '\0';
        char marker[PATH_MAX + 32];
        snprintf(marker, sizeof(marker), "//@src 1 %s\n", safe);
        buf_append_str(&result, marker);
    }
    buf_append_str(&result, self_mangled);

    free(imports_merged.data);
    free(self_mangled);
    free(raw);
    free(stripped);
    for (int i = 0; i < use_count; i++) { free(uses[i].path); if (uses[i].alias) free(uses[i].alias); }
    free(uses);
    for (int i = 0; i < decl_count; i++) { free(decls[i].name); free(decls[i].ext_params); free(decls[i].ext_ret); }
    free(decls);

    g_modules[slot].in_progress = 0;
    g_modules[slot].done = 1;

    return result.data;
}

/* Maps `alias__NAME` back to the real C symbol NAME for every variadic
   extern reached through an alias (see var_alias_record). Uses
   rename_ident_real, so string/char literals, comments and struct field
   names are left alone. The variadic `extern fn NAME(..., ...)` line
   itself already carries the real name, so no declaration is touched. */
static char *rewrite_variadic_alias_calls(const char *src) {
    char *cur = strdup(src);
    for (int i = 0; i < g_var_alias_n; i++) {
        char *next = rename_ident_real(cur, g_var_alias[i].mangled, g_var_alias[i].real);
        free(cur);
        cur = next;
    }
    return cur;
}

/* Resets all per-compile module-resolution state (the module table, the
   extern-signature and variadic-alias dedup tables, the global-alias list)
   to its initial values, so resolve_modules() can be called repeatedly
   from the same process (e.g. an embedding library, LSP, watch-mode or
   build-server) without stale data or leaked memory carrying over between
   compiles.

   Deliberately NOT touched here: g_home_dir, g_auto_std, g_allow_shim.
   Those are process-level configuration set once via module_set_home() /
   module_set_auto_std() / module_set_allow_shim() (see main.c), not
   per-compile state -- resetting them here would silently discard the
   embedder's configuration on every call, which is not what "reset
   between compiles" should mean. An embedder that genuinely wants to
   change that configuration between compiles should call those setters
   again itself.

   Must run before any other module-resolution call. */
void module_reset(void) {
    for (int i = 0; i < g_module_count; i++) {
        free(g_modules[i].declared_module_name);
        free(g_modules[i].resolved_effective_alias);
    }
    memset(g_modules, 0, sizeof(g_modules));
    g_module_count = 0;
    g_mangle_counter = 0;

    for (int i = 0; i < g_global_alias_n; i++) free(g_global_alias[i]);
    g_global_alias_n = 0;

    for (int i = 0; i < g_ext_sig_n; i++) free(g_ext_sigs[i].sig);
    g_ext_sig_n = 0;

    for (int i = 0; i < g_var_alias_n; i++) { free(g_var_alias[i].mangled); free(g_var_alias[i].real); }
    g_var_alias_n = 0;
}

char *resolve_modules(const char *entry_path) {
    module_reset();
    /* M41: std/std.rapid'i otomatik dahil et (metin olarak kullanici kodunun ardindan):
       legacy kod (use satiri olmayan, `toInt(x)`/`len(s)` yazan) degismeden
       derlenir. Dosya bulunamazsa (ozel kurulum) sessizce atlanir. Kullanicinin
       kendi `use "std/std.rapid";` satiri sonradan "zaten dahil" olarak yutulur
       (diamond kurali), yani cift dahil etme / cakisma olmaz. */
    Buf pre; buf_init(&pre);
    if (g_auto_std && g_home_dir[0]) {
        char std_path[PATH_MAX];
        /* Sorun-5 fix: same truncation check as above -- an oversized
           g_home_dir just means auto-std silently can't be located (same
           user-visible behavior as "dosya bulunamazsa sessizce atlanir"
           already documented above), never a stat() on a cut-off path. */
        int n = snprintf(std_path, sizeof(std_path), "%s/std/std.rapid", g_home_dir);
        if (n >= 0 && (size_t)n < sizeof(std_path) && access(std_path, R_OK) == 0) {
            char *std_text = resolve_file(std_path, 0, NULL, NULL);
            if (!std_text) return NULL;
            buf_append_str(&pre, std_text);
            buf_append_str(&pre, "\n");
            free(std_text);
        }
    }
    char *entry_merged = resolve_file(entry_path, 1, NULL, NULL);
    if (!entry_merged) { free(pre.data); return NULL; }
    char *merged;
    if (pre.data && pre.len) {
        Buf all; buf_init(&all);
        /* std SONA eklenir: fonksiyon sirasi onemsiz, ama kullanici dosyasinin
           satir numaralari (parse error: line N) kaymaz. */
        buf_append_str(&all, entry_merged);
        buf_append_str(&all, "\n");
        buf_append_str(&all, pre.data);
        merged = all.data;
        free(entry_merged);
    } else {
        merged = entry_merged;
    }
    free(pre.data);
    /* M41: `use` olmadan yazilmis `std::isim` oneklerini de coz (kullanici dosyalari
       icin rewrite_alias_references yalniz acik `use` edilenlerde calisir). */
    for (int i = 0; i < g_global_alias_n; i++) {
        char *rw = replace_alias_calls(merged, g_global_alias[i], "", -1);
        free(merged);
        merged = rw;
    }

    /* M29: expand `keyword ...;` declarations (see expand_keyword_decls)
       as the very last step, once the whole program is a single text
       blob — this way a `keyword` declared in an imported file works
       exactly like one declared in the entry file. */
    char *expanded = expand_keyword_decls(merged);
    free(merged);
    if (!expanded) return NULL;
    char *final = rewrite_variadic_alias_calls(expanded);
    free(expanded);
    return final;
}
