#include "codegen.h"
#include "var_struct_map.h"
#include "rapidc_limits.h"
#include <string.h>

/* Sorun-5 fix: bu limitler artik limits.h'ta TEK yerde tanimli; buradaki
   isimler (semantic.c'deki karsiliklariyla birebir ayni ad degil, ama ayni
   anlama gelen MAX_VARS/MAX_LOCALS gibi) geriye donuk uyumluluk icin
   korunuyor, sadece kaynaklari degisti -- cagri yerlerinin hicbiri
   degismedi. */
#define MAX_LOCALS RAPIDC_MAX_LOCALS_PER_FUNC
#define MAX_STRINGS RAPIDC_MAX_STRINGS
#define MAX_FUNCS RAPIDC_MAX_FUNCS

/* A local variable. Arrays are kept as a pointer (QBE temp) to a stack block
   allocated with `alloc8`; everything else (int/bool/string) is a plain QBE
   `l`-typed temp holding the value directly (strings are pointers to the
   data segment / to a copy, ints/bools are the raw 64-bit value).
   For variables that have their address taken (&x), we allocate them on the
   stack and access via load/store. */
typedef struct {
    char *names[MAX_LOCALS];
    TypeKind types[MAX_LOCALS];
    TypeKind elem_types[MAX_LOCALS]; /* M26: pointee type when types[i] == TYPE_PTR */
    TypeKind elem_elem_types[MAX_LOCALS]; /* M32: pointee of the pointee, valid only when elem_types[i] == TYPE_PTR */
    int array_len[MAX_LOCALS]; /* element count, for TYPE_INT_ARRAY locals */
    int addr_taken[MAX_LOCALS]; /* 1 if &var is used somewhere */
    int count;
} LocalTable;

/* Function signature info the codegen needs about *other* functions when
   emitting a call (arity + whether it returns void, for QBE call syntax). */
typedef struct {
    char *name;
    TypeKind ret_type;
    TypeKind ret_elem_type; /* M26: pointee type when ret_type == TYPE_PTR */
    TypeKind ret_elem_elem_type; /* M32: pointee of the pointee, valid only when ret_elem_type == TYPE_PTR */
    int param_count;
    int is_extern;   /* M27: call the bare symbol ($name, no $f_ mangling), no definition to emit */
    int is_variadic; /* M27: append a trailing `...` marker to the call site */
} FuncInfo;

static FILE *out;
static int tmp_counter;
static int label_counter;
static int had_fmt_int;    /* whether we need the "%ld\n" format string */
static int had_fmt_str;    /* whether we need the "%s\n" format string */
static int had_fmt_byte;   /* whether we need the "%c\n" format string */
static int had_fmt_double; /* M25: whether we need the "%g\n" format string */

static char *g_strings[MAX_STRINGS];
static int g_string_count;

static FuncInfo g_funcs[MAX_FUNCS];
static int g_func_count;

#define MAX_LOOP_DEPTH RAPIDC_MAX_LOOP_DEPTH
static int loop_continue_label[MAX_LOOP_DEPTH]; /* label to jump to on `continue` */
static int loop_break_label[MAX_LOOP_DEPTH];    /* label to jump to on `break` (also used for switch) */
static int loop_is_for_in[MAX_LOOP_DEPTH];      /* 1 if this loop level is a for-in (continue must also bump the hidden index) */
static int loop_for_in_idx_tmp[MAX_LOOP_DEPTH]; /* hidden index temp number, for for-in levels */
static int loop_depth = 0;

static int new_tmp(void) {
    return tmp_counter++;
}

/* BUG-4 fix: string-concat memory leak. Each `a + b` on strings lowers to a
   $rapidc_strcat call that malloc's a fresh heap string (see runtime.c). In
   a chain like `a + b + c`, the intermediate `a + b` result is consumed
   only by the outer concat and then never named by any variable, so the
   user has no handle to free() it -- it was leaked by construction,
   independent of what the user's source does. `for (...) { s = s + x; }`
   style loops compound this into unbounded heap growth.

   Fix: record the temp number of every strcat result produced while
   generating a single statement's expression(s). At the end of that
   statement, free every recorded temp except the one actually holding the
   statement's final/stored value (that one is either bound to a variable
   the user owns and can free() themselves, or is itself being returned/
   stored and must survive). This mirrors what a human would do by hand if
   they manually introduced a temp variable for every intermediate
   concatenation and freed all but the last. */
#define MAX_STRCAT_TMPS RAPIDC_MAX_STRCAT_TMPS
static int g_strcat_tmps[MAX_STRCAT_TMPS];
static int g_strcat_tmp_count = 0;

static void strcat_tmp_reset(void) {
    g_strcat_tmp_count = 0;
}

static void strcat_tmp_record(int t) {
    /* Past the cap we simply stop tracking further temps in this one
       statement -- an expression concatenating 256+ strings is already an
       extreme outlier, and failing to free a few extra intermediates in
       that case is no worse than the pre-fix behavior for all of them. */
    if (g_strcat_tmp_count < MAX_STRCAT_TMPS) {
        g_strcat_tmps[g_strcat_tmp_count++] = t;
    }
}

/* Parses "%tN" -> N, or -1 if buf isn't exactly a bare temp reference (e.g.
   a variable name, a global string label, or empty). Only bare temps can
   possibly be one of our tracked strcat results. */
static int tmp_num_from_buf(const char *buf) {
    if (!buf || buf[0] != '%' || buf[1] != 't') return -1;
    char *end;
    long n = strtol(buf + 2, &end, 10);
    if (end == buf + 2 || *end != '\0') return -1;
    return (int)n;
}

/* Frees every tracked strcat temp from the current statement except
   keep_buf (the temp actually holding the statement's surviving value, if
   any -- pass NULL/"" if nothing should be kept, e.g. a bare expression
   statement whose value is discarded entirely, in which case ALL tracked
   temps are freed since none of them escape the statement). Called once at
   the end of gen_stmt for every statement kind that can contain a string
   `+`. */
static void strcat_tmp_free_all_except(const char *keep_buf) {
    int keep = tmp_num_from_buf(keep_buf);
    for (int i = 0; i < g_strcat_tmp_count; i++) {
        if (g_strcat_tmps[i] == keep) continue;
        fprintf(out, "\tcall $rapidc_free(l %%t%d)\n", g_strcat_tmps[i]);
    }
    strcat_tmp_reset();
}

static int new_label(void) {
    return label_counter++;
}

static int local_find(LocalTable *lt, const char *name) {
    for (int i = 0; i < lt->count; i++) {
        if (strcmp(lt->names[i], name) == 0) return i;
    }
    return -1;
}

/* Set when codegen hits a hard limit it cannot recover from locally;
   codegen_emit() reports failure to the caller (main.c cleans up temp files)
   instead of scribbling past the end of a fixed-size table. */
static int g_codegen_error = 0;

/* Sorun-4 fix: previously every while/for/for-in/switch entry wrote
   loop_*_label[loop_depth] and incremented loop_depth with NO bounds
   check at all -- MAX_LOOP_DEPTH (32) levels of nesting silently
   overflowed these fixed arrays (stack corruption), unlike every other
   milestone limit in this file which already errors cleanly. Call this
   right before the loop_*_label[loop_depth] writes at each push site;
   mirrors the g_expr_depth / MAX_EXPR_DEPTH pattern above. Returns 1 if
   it's safe to proceed (array write to loop_depth is in-bounds), 0 if
   the limit was hit (caller must skip the write and not increment). */
static int loop_depth_push_ok(void) {
    if (loop_depth >= MAX_LOOP_DEPTH) {
        if (!g_codegen_error)
            fprintf(stderr, "rapidc: codegen: loops/switch nested too deeply (limit %d)\n", MAX_LOOP_DEPTH);
        g_codegen_error = 1;
        return 0;
    }
    return 1;
}

static void local_add(LocalTable *lt, char *name, TypeKind type, int array_len, TypeKind elem_type, TypeKind elem_elem_type) {
    /* LocalTable lives on gen_function()'s stack; an unchecked write past
       MAX_LOCALS is silent stack corruption, not just a wrong answer. */
    if (lt->count >= MAX_LOCALS) {
        if (!g_codegen_error)
            fprintf(stderr, "rapidc: codegen: too many local variables in one function (limit %d)\n", MAX_LOCALS);
        g_codegen_error = 1;
        return;
    }
    lt->types[lt->count] = type;
    lt->elem_types[lt->count] = elem_type; /* M26 */
    lt->elem_elem_types[lt->count] = elem_elem_type; /* M32 */
    lt->array_len[lt->count] = array_len;
    lt->addr_taken[lt->count] = 0;
    lt->names[lt->count] = name;
    lt->count++;
}

/* Registers a string literal in the data segment (dedup by content) and
   returns its label index. */
static int string_intern(const char *s) {
    for (int i = 0; i < g_string_count; i++) {
        if (strcmp(g_strings[i], s) == 0) return i;
    }
    if (g_string_count >= MAX_STRINGS) {
        /* Onceden sinir kontrolu yoktu: 256'dan fazla FARKLI string literal
           g_strings[]'i tasirip rapidc'nin kendisini segfault ettiriyordu. */
        fprintf(stderr, "rapidc: too many distinct string literals (limit %d)\n", MAX_STRINGS);
        exit(1);
    }
    g_strings[g_string_count] = strdup(s);
    return g_string_count++;
}

static FuncInfo *func_info_find(const char *name) {
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, name) == 0) return &g_funcs[i];
    }
    return NULL;
}

/* M8: struct helpers. A struct-typed local is a pointer to a stack block
   (alloc8'd at declaration); %v_<name> holds that base pointer, and fields
   live at fixed byte offsets (8 bytes each, computed by struct_def_new).
   The var -> struct-name association is recorded by semantic.c via the
   var_struct_map so field accesses can be resolved here. */
static StructDef *struct_of_var(const char *var) {
    const char *sn = var_struct_map_get(var);
    return sn ? struct_find(sn) : NULL;
}

static Field *struct_field_find(StructDef *sd, const char *name) {
    if (!sd) return NULL;
    for (Field *f = sd->fields; f; f = f->next) {
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

/* BUG-02/BUG-03 fix: codegen's mirror of semantic.c's struct_def_of_expr —
   resolves the StructDef of any struct-typed expr (EXPR_IDENT via
   var_struct_map as struct_of_var already did, or EXPR_FIELD recursively
   via the field's own struct_name), not just a bare variable. Needed so
   offset lookups for a chained field (`o.inner.a`) know what struct
   `o.inner` is an instance of. */
static StructDef *struct_def_of_expr(Expr *e) {
    if (!e) return NULL;
    if (e->kind == EXPR_IDENT) return struct_of_var(e->name);
    if (e->kind == EXPR_FIELD) {
        StructDef *base_sd = struct_def_of_expr(e->lhs);
        Field *fd = struct_field_find(base_sd, e->name);
        return (fd && fd->type == TYPE_STRUCT && fd->struct_name) ? struct_find(fd->struct_name) : NULL;
    }
    return NULL;
}

/* Emits `storel <val>, <base + offset>` (base is a temp name like "%t5"). */
static void emit_store_at(const char *base, int offset, const char *val) {
    if (offset == 0) {
        fprintf(out, "\tstorel %s, %s\n", val, base);
        return;
    }
    int a = new_tmp();
    fprintf(out, "\t%%t%d =l add %s, %d\n", a, base, offset);
    fprintf(out, "\tstorel %s, %%t%d\n", val, a);
}

/* Zero-initializes every field of the struct at `base` (temp name). */
static void emit_struct_zero(StructDef *sd, const char *base) {
    for (Field *f = sd ? sd->fields : NULL; f; f = f->next) {
        emit_store_at(base, f->offset, "0");
    }
}

/* BUG-03 fix: copies word-for-word into an
   *existing* destination address (dst_addr) instead of allocating a fresh
   block — what field-to-field struct assignment needs (`o.inner = i;`
   must copy i's bytes into o's already-existing `inner` slot, not
   reassign that slot to point at i's storage, which would alias src and
   dst the same way plain `%v_dst =l copy %v_src` would for a variable). */
static void emit_struct_copy_into(StructDef *sd, const char *dst_addr, const char *src_addr) {
    int size = (sd && sd->size > 0) ? sd->size : 8;
    for (int off = 0; off < size; off += 8) {
        int t = new_tmp();
        if (off == 0) {
            fprintf(out, "\t%%t%d =l loadl %s\n", t, src_addr);
        } else {
            int a = new_tmp();
            fprintf(out, "\t%%t%d =l add %s, %d\n", a, src_addr, off);
            fprintf(out, "\t%%t%d =l loadl %%t%d\n", t, a);
        }
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "%%t%d", t);
        emit_store_at(dst_addr, off, tbuf);
    }
}

/* Recovers the static type of an already-semantically-checked expression.
   Codegen needs this (rather than just emitting values blindly) to decide
   e.g. whether `+` means integer addition or string concatenation, and
   whether io::out needs "%ld\n" or "%s\n". Mirrors semantic.c's check_expr
   type rules exactly, but does no error reporting (the program already
   passed semantic_check by the time codegen runs). */
static TypeKind static_type_of(LocalTable *lt, Expr *e);
static TypeKind static_elem_elem_type_of(LocalTable *lt, Expr *e);

/* M26: recovers the pointee (elem_type) of an already-checked TYPE_PTR
   expression. Codegen's mirror of semantic.c's expr_elem_type_of() —
   needed for the same reason: static_type_of only returns the flat
   TYPE_PTR tag, losing what it points to, which codegen needs to pick the
   right load/store width instruction (loadsb vs loadd vs ...). */
static TypeKind static_elem_type_of(LocalTable *lt, Expr *e) {
    switch (e->kind) {
        case EXPR_TRY: return static_elem_type_of(lt, e->arg);   /* M43 */
        case EXPR_CATCH: return static_elem_type_of(lt, e->lhs); /* M43 */
        case EXPR_ADDR_OF: {
            /* &x — elem_type is x's own static type. Handles plain idents,
               array/list elements (&arr[i]), and struct fields (&p.f)
               alike since static_type_of already dispatches on all of
               those shapes. */
            return static_type_of(lt, e->arg);
        }
        case EXPR_DEREF: {
            /* M32: *pp where pp: T** — pp's own elem_type is TYPE_PTR (a
               pointer, meaning *pp's static type is TYPE_PTR too); *pp's
               OWN pointee (what static_elem_type_of must report here) is
               one level further in, i.e. pp's elem_elem_type. If pp is
               only T* (arg_elem != TYPE_PTR), fall back to the original
               one-level behavior. */
            TypeKind arg_elem = static_elem_type_of(lt, e->arg);
            if (arg_elem == TYPE_PTR) return static_elem_elem_type_of(lt, e->arg);
            return arg_elem;
        }
        case EXPR_IDENT: {
            int idx = local_find(lt, e->name);
            return idx == -1 ? TYPE_INT : lt->elem_types[idx];
        }
        case EXPR_CALL: {
            if (strcmp(e->name, "alloc") == 0) return e->elem_type; /* set by semantic.c */
            FuncInfo *fi = func_info_find(e->name);
            return fi ? fi->ret_elem_type : TYPE_INT;
        }
        case EXPR_FIELD: {
            /* BUG-02/BUG-03 fix: struct_def_of_expr handles nested
               EXPR_FIELD bases, not just EXPR_IDENT. */
            StructDef *sd = struct_def_of_expr(e->lhs);
            Field *fd = struct_field_find(sd, e->name);
            if (fd) return fd->elem_type;
            return TYPE_INT;
        }
        case EXPR_BINOP:
            /* T* +/- int → T*, same elem_type as the pointer side. C lets
               `int + T*` as well as `T* + int`, so recover the pointee from
               whichever operand is actually the pointer. */
            if (static_type_of(lt, e->rhs) == TYPE_PTR) {
                return static_elem_type_of(lt, e->rhs);
            }
            return static_elem_type_of(lt, e->lhs);
        default:
            return TYPE_INT;
    }
}

/* M32: recovers the pointee-of-the-pointee (elem_elem_type) of an already-
   checked TYPE_PTR expression whose elem_type is itself TYPE_PTR (i.e. this
   expr is T**). Codegen's mirror of semantic.c's expr_elem_elem_type_of() —
   same shapes as static_elem_type_of() above, one level further in. Only
   meaningful when the caller has already established
   static_elem_type_of(lt, e) == TYPE_PTR. */
static TypeKind static_elem_elem_type_of(LocalTable *lt, Expr *e) {
    switch (e->kind) {
        case EXPR_ADDR_OF:
            /* &p where p: T* — this node is T**, and its elem_elem_type is
               p's own elem_type (what p itself points to). */
            return static_elem_type_of(lt, e->arg);
        case EXPR_DEREF:
            /* **ppp where ppp: T*** isn't reachable (T*** doesn't parse),
               so a EXPR_DEREF here means e->arg is itself a TYPE_PTR whose
               elem_type is TYPE_PTR — not a shape this milestone produces.
               Kept for structural symmetry with static_elem_type_of. */
            return TYPE_INT;
        case EXPR_IDENT: {
            int idx = local_find(lt, e->name);
            return idx == -1 ? TYPE_INT : lt->elem_elem_types[idx];
        }
        case EXPR_CALL: {
            if (strcmp(e->name, "alloc") == 0) return e->elem_elem_type; /* set by semantic.c */
            FuncInfo *fi = func_info_find(e->name);
            return fi ? fi->ret_elem_elem_type : TYPE_INT;
        }
        case EXPR_FIELD: {
            /* BUG-02/BUG-03 fix: see static_elem_type_of above. */
            StructDef *sd = struct_def_of_expr(e->lhs);
            Field *fd = struct_field_find(sd, e->name);
            if (fd) return fd->elem_elem_type;
            return TYPE_INT;
        }
        case EXPR_BINOP:
            /* T** +/- int → T**, same elem_elem_type as the pointer side. */
            return static_elem_elem_type_of(lt, e->lhs);
        default:
            return TYPE_INT;
    }
}

static TypeKind static_type_of(LocalTable *lt, Expr *e) {
    switch (e->kind) {
        case EXPR_TRY: return static_type_of(lt, e->arg);    /* M43 */
        case EXPR_CATCH: return static_type_of(lt, e->lhs);  /* M43 */
        case EXPR_ERROR_LIT: return TYPE_INT;                 /* M43 */
        case EXPR_INT_LITERAL: return TYPE_INT;
        case EXPR_FLOAT_LITERAL: return TYPE_DOUBLE; /* M25: matches semantic.c's default */
        case EXPR_CHAR_LITERAL: return TYPE_BYTE;
        case EXPR_NULL_LITERAL: return TYPE_PTR; /* M33 */
        case EXPR_BOOL_LITERAL: return TYPE_BOOL;
        case EXPR_STRING_LITERAL: return TYPE_STRING;
        case EXPR_IDENT: {
            int idx = local_find(lt, e->name);
            return idx == -1 ? TYPE_INT : lt->types[idx];
        }
        case EXPR_IO_OUT_CALL: return TYPE_VOID;
        case EXPR_IO_IN_CALL: return TYPE_STRING;
        case EXPR_IO_FORMAT_CALL: return TYPE_VOID;
        case EXPR_IO_SYSCALL_CALL: return TYPE_INT;
        case EXPR_ARRAY_LITERAL: return TYPE_INT_LIST; /* M35: only reachable as a list<int>/list<byte> initializer now; see semantic.c */
        case EXPR_INDEX: {
            TypeKind ct = static_type_of(lt, e->lhs);
            if (ct == TYPE_STRING || ct == TYPE_BYTE_LIST) return TYPE_BYTE;
            if (ct == TYPE_PTR) return static_elem_type_of(lt, e->lhs); /* M26: T*[i] reads T */
            return TYPE_INT;
        }
        case EXPR_ADDR_OF: {
            /* M26: &x now yields TYPE_PTR for any type_ptr_ok() operand,
               not just int/byte. The actual elem_type is recovered by
               static_elem_type_of() below (mirrors semantic.c's
               expr_elem_type_of) — static_type_of only needs to report the
               flat TYPE_PTR tag here. */
            return TYPE_PTR;
        }
        case EXPR_DEREF: {
            return static_elem_type_of(lt, e->arg);
        }
        case EXPR_CALL: {
            /* M41: len/listCopy/syscall/toInt/toString/toFloat/toDouble artik
               std/std.rapid'de normal fonksiyonlar (donus tipi FuncInfo'dan).
               push/alloc/free ve toInt(enum) icsel kaldi. */
            if (strcmp(e->name, "push") == 0) return TYPE_VOID;
            if (strcmp(e->name, "alloc") == 0) return TYPE_PTR;
            if (strcmp(e->name, "free") == 0) return TYPE_VOID;
            if (strcmp(e->name, "__enum_to_int") == 0) return TYPE_INT;
            FuncInfo *fi = func_info_find(e->name);
            return fi ? fi->ret_type : TYPE_VOID;
        }
        case EXPR_FIELD: {
            /* M8: base must be struct-typed (guaranteed by semantic).
               BUG-02 fix: struct_def_of_expr handles a nested EXPR_FIELD
               base (o.inner), not just EXPR_IDENT (o). */
            StructDef *sd = struct_def_of_expr(e->lhs);
            Field *fd = struct_field_find(sd, e->name);
            if (fd) return fd->type;
            return TYPE_INT;
        }
        case EXPR_ENUM_LITERAL: return TYPE_ENUM;
        case EXPR_STRUCT_LITERAL: return TYPE_STRUCT;
        case EXPR_NEG: return static_type_of(lt, e->arg); /* M29: same type as operand */
        case EXPR_NOT: return TYPE_BOOL;
        case EXPR_LOGAND: case EXPR_LOGOR: return TYPE_BOOL;
        case EXPR_FUNC_REF: return TYPE_FN; /* M11 */
        case EXPR_CALL_INDIRECT: return e->fn_sig ? e->fn_sig->ret : TYPE_INT; /* M11/M40 */
        case EXPR_BINOP:
            switch (e->op) {
                case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
                    return TYPE_BOOL;
                case OP_ADD: case OP_SUB: {
                    TypeKind ltype = static_type_of(lt, e->lhs);
                    TypeKind rtype = static_type_of(lt, e->rhs);
                    /* M26: T* +/- T* → int (pointer difference);
                       T* +/- int → T* (generalized off TYPE_PTR). M26-add:
                       int + T* is allowed too (C commutativity), so return
                       TYPE_PTR whenever either side is a pointer and the op
                       is addition. */
                    if (ltype == TYPE_PTR && rtype == TYPE_PTR) return TYPE_INT;
                    if (ltype == TYPE_PTR) return TYPE_PTR;
                    if (rtype == TYPE_PTR && e->op == OP_ADD) return TYPE_PTR;
                    /* M25: float +/- double (either order) promotes to
                       double, matching both semantic.c's type_float_promote
                       and the actual QBE class gen_expr's EXPR_BINOP emits —
                       static_type_of must agree with gen_expr's real output
                       class here, or callers (like STMT_VAR_DECL's
                       exts/truncd width-matching) apply a conversion against
                       the wrong assumed source class. */
                    if (type_is_float(ltype) || type_is_float(rtype)) {
                        if (ltype == TYPE_DOUBLE || rtype == TYPE_DOUBLE) return TYPE_DOUBLE;
                        return TYPE_FLOAT;
                    }
                    return ltype; /* int+int->int, string+string->string */
                }
                case OP_MUL: case OP_DIV: {
                    /* M25: mul/div on float operands promotes the same way
                       as add/sub above; everything else (int-family) keeps
                       the previous plain-int answer. */
                    TypeKind ltype = static_type_of(lt, e->lhs);
                    TypeKind rtype = static_type_of(lt, e->rhs);
                    if (type_is_float(ltype) || type_is_float(rtype)) {
                        if (ltype == TYPE_DOUBLE || rtype == TYPE_DOUBLE) return TYPE_DOUBLE;
                        return TYPE_FLOAT;
                    }
                    return TYPE_INT;
                }
                case OP_POW: {
                    /* Same float-promotion rule as MUL/DIV above; semantic.c
                       already rejected anything that isn't integer/float. */
                    TypeKind ltype = static_type_of(lt, e->lhs);
                    TypeKind rtype = static_type_of(lt, e->rhs);
                    if (type_is_float(ltype) || type_is_float(rtype)) {
                        if (ltype == TYPE_DOUBLE || rtype == TYPE_DOUBLE) return TYPE_DOUBLE;
                        return TYPE_FLOAT;
                    }
                    return TYPE_INT;
                }
                case OP_MOD: case OP_BAND: case OP_BOR: case OP_BXOR:
                case OP_SHL: case OP_SHR:
                    /* Integer-only per semantic.c; result is always int. */
                    return TYPE_INT;
            }
            return TYPE_VOID;
    }
    return TYPE_VOID;
}

/* M9: byte offset from a container's base pointer to its first element.
   Fixed arrays are a raw block (element 0 at offset 0); dynamic lists have
   a 16-byte {len,cap} header before the data (see runtime.c). */
static int elem_base_offset(TypeKind container_type) {
    return (container_type == TYPE_INT_LIST || container_type == TYPE_BYTE_LIST) ? 16 : 0;
}

/* M20: byte stride between elements. Everything is 8-byte (int slots)
   except list<byte>, which packs single bytes contiguously. */
static int elem_stride(TypeKind container_type) {
    return container_type == TYPE_BYTE_LIST ? 1 : 8;
}

/* M26: byte size of one value of a (pointed-to) TypeKind, for pointer
   arithmetic scaling. Only the width-bearing scalar kinds matter here —
   the element type of a pointer always lands in one of them; struct/enum
   and the catch-all fall back to 8 (struct fields are 8-aligned/8-sized in
   this release, so sizeof is effectively 8 for the cases that currently
   reach here). */
static int type_size(TypeKind t) {
    switch (t) {
        case TYPE_BYTE:  case TYPE_INT8:  case TYPE_UINT8:  return 1;
        case TYPE_INT16: case TYPE_UINT16: return 2;
        case TYPE_INT32: case TYPE_UINT32: return 4;
        case TYPE_FLOAT: return 4;
        default: return 8; /* TYPE_INT, BOOL, DOUBLE, STRING, PTR, ENUM, STRUCT */
    }
}

/* M?: true when an element value of class `et` needs a QBE width/mode
   conversion before being stored into a `T[]` whose element kind is `elem`
   (only float-family cross-width happens in practice, since int-family
   elements are width-compatible within the `l` class). */
static int needs_float_conv(TypeKind et, TypeKind elem) {
    return type_is_float(et) && type_is_float(elem) &&
           type_qbe_class(et) != type_qbe_class(elem);
}

/* Emits a call to the runtime bounds checker for list<int>/list<byte>
   indexing. `hdr` is the list's header pointer (its first 8 bytes are the
   current length, see runtime.c); `idx_val` is the already-evaluated QBE
   index value. Aborts the program with a clean message instead of letting
   an out-of-range index silently corrupt the heap. `ctx` is a short,
   static C string describing the operation, embedded as a string literal
   so this costs one string_intern slot, not a fresh malloc per call site. */
static void gen_list_bounds_check(const char *hdr, const char *idx_val, const char *ctx) {
    int len_tmp = new_tmp();
    fprintf(out, "\t%%t%d =l loadl %s\n", len_tmp, hdr);
    int ctx_idx = string_intern(ctx);
    fprintf(out, "\tcall $rapidc_bounds_check(l %s, l %%t%d, l $str%d)\n",
            idx_val, len_tmp, ctx_idx);
}

/* Emits a bounds check for string[i]: calls rapidc_strlen at runtime to get
   the string's length (strings are NUL-terminated, no stored header), then
   delegates to rapidc_bounds_check which aborts with a clean error on OOB.
   The bound passed is len+1 (not len) to allow s[len] (the NUL terminator):
   C-style string scanning reads the NUL to detect end-of-string, and the
   buffer always allocates len+1 bytes, so that access is always valid. Only
   s[len+1] and beyond (the true OOB region) are rejected. */
static void gen_string_bounds_check(const char *str_val, const char *idx_val) {
    int len_tmp = new_tmp();
    fprintf(out, "\t%%t%d =l call $rapidc_strlen(l %s)\n", len_tmp, str_val);
    int len1_tmp = new_tmp();
    fprintf(out, "\t%%t%d =l add %%t%d, 1\n", len1_tmp, len_tmp);
    int ctx_idx = string_intern("string index");
    fprintf(out, "\tcall $rapidc_bounds_check(l %s, l %%t%d, l $str%d)\n",
            idx_val, len1_tmp, ctx_idx);
}

/* Emits the address of element `idx_val` of `base_val` (a QBE temp/value
   holding the container's base pointer) into a fresh temp, returned as a
   "%tN" string in buf. Shared by EXPR_INDEX, &arr[i], and indexed-assign.
   For list<int>/list<byte> (the only container kinds with a header, hence
   a known length at runtime) this first emits a bounds check against the
   header's length field — see gen_list_bounds_check. T* pointer indexing
   and string[i] go through separate codegen paths (not this function) and
   remain unchecked, same as C, since there is no length to check against. */
static char *gen_elem_addr(TypeKind container_type, const char *base_val, const char *idx_val, char *buf, size_t bufsz) {
    if (container_type == TYPE_INT_LIST || container_type == TYPE_BYTE_LIST) {
        gen_list_bounds_check(base_val, idx_val, "list index");
    }
    int off = new_tmp();
    fprintf(out, "\t%%t%d =l mul %s, %d\n", off, idx_val, elem_stride(container_type));
    int addr = new_tmp();
    int hdr_off = elem_base_offset(container_type);
    if (hdr_off) {
        int base_plus_hdr = new_tmp();
        fprintf(out, "\t%%t%d =l add %s, %d\n", base_plus_hdr, base_val, hdr_off);
        fprintf(out, "\t%%t%d =l add %%t%d, %%t%d\n", addr, base_plus_hdr, off);
    } else {
        fprintf(out, "\t%%t%d =l add %s, %%t%d\n", addr, base_val, off);
    }
    snprintf(buf, bufsz, "%%t%d", addr);
    return buf;
}

/* Address-of-element for `T*` (TYPE_PTR) indexing, with the index scaled by
   sizeof(T) (1 for byte, so no mul). Dynamic lists still go through
   gen_elem_addr (they carry a header). M35: fixed-size `T[]` (TYPE_ARRAY)
   is gone — this function's container_type is now always TYPE_PTR or a
   list kind by the time it's called; the elem lookup falls back to
   TYPE_INT for any other shape as a defensive default, but that path
   should be unreachable post-semantic-check. */
static char *gen_scaled_idx_addr(LocalTable *lt, TypeKind container_type, Expr *arr_expr, const char *base_val, const char *idx_val, char *buf, size_t bufsz) {
    if (container_type == TYPE_INT_LIST || container_type == TYPE_BYTE_LIST) {
        return gen_elem_addr(container_type, base_val, idx_val, buf, bufsz);
    }
    TypeKind elem = (container_type == TYPE_PTR)
                    ? static_elem_type_of(lt, arr_expr) : TYPE_INT;
    int sz = type_size(elem);
    char offbuf[32];
    if (sz == 1) {
        snprintf(offbuf, sizeof(offbuf), "%s", idx_val);
    } else {
        int off = new_tmp();
        fprintf(out, "\t%%t%d =l mul %s, %d\n", off, idx_val, sz);
        snprintf(offbuf, sizeof(offbuf), "%%t%d", off);
    }
    int addr = new_tmp();
    fprintf(out, "\t%%t%d =l add %s, %s\n", addr, base_val, offbuf);
    snprintf(buf, bufsz, "%%t%d", addr);
    return buf;
}

static void mark_addr_taken_expr(LocalTable *lt, Expr *e);
static void mark_addr_taken_stmt(LocalTable *lt, Stmt *s);
static void mark_addr_taken(LocalTable *lt, Stmt *body);

static void mark_addr_taken_expr(LocalTable *lt, Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_ADDR_OF:
            if (e->arg && e->arg->kind == EXPR_IDENT) {
                int idx = local_find(lt, e->arg->name);
                if (idx != -1) lt->addr_taken[idx] = 1;
            }
            break;
        case EXPR_BINOP:
            mark_addr_taken_expr(lt, e->lhs);
            mark_addr_taken_expr(lt, e->rhs);
            break;
        case EXPR_CALL:
            for (ExprList *l = e->args; l; l = l->next)
                mark_addr_taken_expr(lt, l->expr);
            break;
        case EXPR_CALL_INDIRECT:
            mark_addr_taken_expr(lt, e->lhs);
            for (ExprList *l = e->args; l; l = l->next)
                mark_addr_taken_expr(lt, l->expr);
            break;
        case EXPR_INDEX:
            mark_addr_taken_expr(lt, e->lhs);
            mark_addr_taken_expr(lt, e->rhs);
            break;
        case EXPR_ARRAY_LITERAL:
            for (ExprList *l = e->elements; l; l = l->next)
                mark_addr_taken_expr(lt, l->expr);
            break;
        case EXPR_DEREF:
            mark_addr_taken_expr(lt, e->arg);
            break;
        case EXPR_IO_OUT_CALL:
            mark_addr_taken_expr(lt, e->arg);
            break;
        case EXPR_IO_IN_CALL:
            break;
        case EXPR_IO_FORMAT_CALL:
            mark_addr_taken_expr(lt, e->lhs);
            for (ExprList *l = e->args; l; l = l->next)
                mark_addr_taken_expr(lt, l->expr);
            break;
        case EXPR_IO_SYSCALL_CALL:
            for (ExprList *l = e->args; l; l = l->next)
                mark_addr_taken_expr(lt, l->expr);
            break;
        case EXPR_TRY: /* M43 */
            mark_addr_taken_expr(lt, e->arg);
            break;
        case EXPR_CATCH: /* M43 */
            mark_addr_taken_expr(lt, e->lhs);
            mark_addr_taken_expr(lt, e->rhs);
            break;
        default:
            break;
    }
}

/* BUG-3 fix: struct-typed `var p: T = ...;` declarations that live inside a
   loop body previously emitted a fresh `alloc8` on *every* iteration (QBE's
   alloc8 is evaluated dynamically wherever it appears in the instruction
   stream, and reserves stack space for the rest of the function's
   lifetime — it is not freed when control leaves the enclosing block/loop
   iteration). Looping N times over a struct-literal var-decl therefore grew
   the stack by one struct's worth of bytes per iteration, with nothing ever
   reclaimed, eventually overflowing the OS stack limit for large N.

   Fix: give every struct-typed var-decl statement in the function (found by
   walking the whole body once up front, including inside loop/if/for/switch
   bodies — mirroring mark_addr_taken's existing walk) a single, stable
   `alloc8` slot allocated once in the function prologue, keyed by the
   Stmt* itself. STMT_VAR_DECL's codegen then reuses that slot on every
   visit (re-zeroing/re-storing into the same memory) instead of calling
   alloc8 again, so a loop body re-initializes the struct in place with no
   net stack growth per iteration. */
#define MAX_HOISTED_STRUCTS RAPIDC_MAX_HOISTED_STRUCTS
static Stmt *hoisted_struct_stmt[MAX_HOISTED_STRUCTS];
static int hoisted_struct_slot[MAX_HOISTED_STRUCTS];
static int hoisted_struct_count;

static int hoisted_struct_find(Stmt *s) {
    for (int i = 0; i < hoisted_struct_count; i++) {
        if (hoisted_struct_stmt[i] == s) return hoisted_struct_slot[i];
    }
    return -1;
}

static void hoist_struct_locals_stmt(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_VAR_DECL:
        case STMT_CONST_DECL:
            /* M36: struct haritasi fonksiyon-yerel; bu dolasici her ic ice
               blogun struct-tipli yerellerine iner, o yuzden kayit burada. */
            if (s->struct_name) var_struct_map_set(s->name, s->struct_name);
            if (s->type == TYPE_STRUCT && hoisted_struct_count < MAX_HOISTED_STRUCTS) {
                StructDef *sd = s->struct_name ? struct_find(s->struct_name) : NULL;
                int base = new_tmp();
                fprintf(out, "\t%%t%d =l alloc8 %d\n", base, sd && sd->size > 0 ? sd->size : 8);
                hoisted_struct_stmt[hoisted_struct_count] = s;
                hoisted_struct_slot[hoisted_struct_count] = base;
                hoisted_struct_count++;
            }
            break;
        case STMT_IF:
            for (Stmt *b = s->then_body; b; b = b->next) hoist_struct_locals_stmt(b);
            for (Stmt *b = s->else_body; b; b = b->next) hoist_struct_locals_stmt(b);
            break;
        case STMT_WHILE:
        case STMT_DO_WHILE:
        case STMT_FOR_IN:
            for (Stmt *b = s->body; b; b = b->next) hoist_struct_locals_stmt(b);
            break;
        case STMT_FOR:
            hoist_struct_locals_stmt(s->init);
            for (Stmt *b = s->body; b; b = b->next) hoist_struct_locals_stmt(b);
            break;
        case STMT_SWITCH:
            for (CaseClause *c = s->cases; c; c = c->next) {
                for (Stmt *b = c->body; b; b = b->next) hoist_struct_locals_stmt(b);
            }
            break;
        default:
            break;
    }
}

static void hoist_struct_locals(Stmt *body) {
    for (Stmt *s = body; s; s = s->next) hoist_struct_locals_stmt(s);
}

static char *gen_expr(LocalTable *lt, Expr *e, char *buf, size_t bufsz);

/* M43: hata yonetimi yardimcilari. g_cur_ret_type gen_function'da ayarlanir;
   emit_zero_ret mevcut fonksiyonun tipine uygun "bos" donus uretir (hata
   yolunda deger onemsiz, rapid_err zaten set edilmistir). */
static TypeKind g_cur_ret_type = TYPE_VOID;
static void emit_zero_ret(void) {
    switch (g_cur_ret_type) {
        case TYPE_VOID:   fprintf(out, "\tret\n"); break;
        case TYPE_FLOAT:  fprintf(out, "\tret s_0\n"); break;
        case TYPE_DOUBLE: fprintf(out, "\tret d_0\n"); break;
        default:          fprintf(out, "\tret 0\n"); break;
    }
}

static char *gen_expr_inner(LocalTable *lt, Expr *e, char *buf, size_t bufsz);

/* SECURITY/DoS: gen_expr_inner recurses once per AST nesting level with no
   bound. A few thousand levels of parenthesized nesting ((((1+1)+1)+1)...)
   exhausts the C call stack and crashes rapidc with SIGSEGV instead of a
   clean diagnostic -- reachable from any (possibly third-party/untrusted)
   .rapid source file, same threat model as the M38 link/shim hardening.
   This thin wrapper (every recursive call inside gen_expr_inner still goes
   through the public gen_expr name, so no call site needs to change) adds
   an explicit depth counter mirroring the MAX_LOCALS pattern local_add()
   already uses: past the limit, emit one error and fail the codegen pass
   via g_codegen_error instead of recursing further. */
#define MAX_EXPR_DEPTH RAPIDC_MAX_EXPR_DEPTH
static int g_expr_depth = 0;

static char *gen_expr(LocalTable *lt, Expr *e, char *buf, size_t bufsz) {
    if (++g_expr_depth > MAX_EXPR_DEPTH) {
        if (!g_codegen_error)
            fprintf(stderr, "rapidc: codegen: expression nested too deeply (limit %d)\n", MAX_EXPR_DEPTH);
        g_codegen_error = 1;
        g_expr_depth--;
        snprintf(buf, bufsz, "%%t0");
        return buf;
    }
    char *r = gen_expr_inner(lt, e, buf, bufsz);
    g_expr_depth--;
    return r;
}

/* Emits a struct literal's field initializers directly into an
   already-allocated destination slot (basebuf), with no alloc8 of its own.
   Factored out of EXPR_STRUCT_LITERAL so STMT_VAR_DECL can target its
   hoisted per-declaration-site slot (BUG-3 fix) without going through an
   intermediate block that would itself leak stack on every loop
   iteration. */
static void gen_struct_literal_into(LocalTable *lt, Expr *e, StructDef *sd, const char *basebuf) {
    emit_struct_zero(sd, basebuf);
    for (StructLiteralField *fl = e->struct_fields; fl; fl = fl->next) {
        Field *fd = struct_field_find(sd, fl->field);
        if (fd && fd->type == TYPE_STRUCT) {
            StructDef *fsd = fd->struct_name ? struct_find(fd->struct_name) : NULL;
            int faddr = new_tmp();
            fprintf(out, "\t%%t%d =l add %s, %d\n", faddr, basebuf, fd->offset);
            char faddrbuf[32];
            snprintf(faddrbuf, sizeof(faddrbuf), "%%t%d", faddr);
            if (fl->value->kind == EXPR_STRUCT_LITERAL) {
                /* Bugfix (same root cause as BUG-3/BUG-5): a nested
                   struct-literal field (`Outer{ inner: Inner{...}, ... }`)
                   must generate its fields directly into the parent's
                   field slot, not through gen_expr's general
                   EXPR_STRUCT_LITERAL case — that path always alloc8's its
                   own fresh temp block for the nested literal before this
                   function copies out of it, and that intermediate block
                   is exactly the same function-lifetime, never-freed
                   allocation BUG-3 already fixed for the *outer* literal.
                   Inside a loop (`var tmp: Outer = Outer{inner: Inner{...},
                   ...};` re-evaluated every iteration), that leaked block
                   accumulates once per iteration same as the original
                   BUG-3 repro, just one nesting level down. Generating
                   straight into the field's own slot (already reserved as
                   part of the outer struct's storage) avoids the
                   intermediate block entirely. */
                StructDef *lit_sd = fsd ? fsd : (fl->value->name ? struct_find(fl->value->name) : NULL);
                gen_struct_literal_into(lt, fl->value, lit_sd, faddrbuf);
            } else {
                char vbuf[32];
                char *v = gen_expr(lt, fl->value, vbuf, sizeof(vbuf));
                emit_struct_copy_into(fsd, faddrbuf, v);
                /* String-concat leak fix: v is now stored into the
                   field's slot and survives; any other strcat
                   intermediate this one field's initializer expression
                   produced is dead. Freeing per-field (rather than once
                   for the whole literal) keeps this safe to call from
                   gen_stmt's VAR_DECL/ASSIGN struct-literal paths, which
                   bypass gen_stmt's own single end-of-statement cleanup. */
                strcat_tmp_free_all_except(v);
            }
        } else {
            char vbuf[32];
            char *v = gen_expr(lt, fl->value, vbuf, sizeof(vbuf));
            emit_store_at(basebuf, fd ? fd->offset : 0, v);
            strcat_tmp_free_all_except(v);
        }
    }
}

static void mark_addr_taken_stmt(LocalTable *lt, Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_VAR_DECL:
        case STMT_CONST_DECL:
            mark_addr_taken_expr(lt, s->expr);
            break;
        case STMT_RETURN:
        case STMT_FAIL: /* M43 */
            mark_addr_taken_expr(lt, s->expr);
            break;
        case STMT_EXPR:
            mark_addr_taken_expr(lt, s->expr);
            break;
        case STMT_IF:
            mark_addr_taken_expr(lt, s->expr);
            for (Stmt *b = s->then_body; b; b = b->next) mark_addr_taken_stmt(lt, b);
            if (s->else_body) {
                if (s->else_body->kind == STMT_IF && s->else_body->next == NULL) {
                    mark_addr_taken_stmt(lt, s->else_body);
                } else {
                    for (Stmt *b = s->else_body; b; b = b->next) mark_addr_taken_stmt(lt, b);
                }
            }
            break;
        case STMT_ASSIGN:
            mark_addr_taken_expr(lt, s->expr);
            break;
        case STMT_INDEX_ASSIGN:
            mark_addr_taken_expr(lt, s->target);
            mark_addr_taken_expr(lt, s->index);
            mark_addr_taken_expr(lt, s->value);
            break;
        case STMT_DEREF_ASSIGN:
            mark_addr_taken_expr(lt, s->target);
            mark_addr_taken_expr(lt, s->value);
            break;
        case STMT_WHILE:
        case STMT_DO_WHILE:
            mark_addr_taken_expr(lt, s->expr);
            for (Stmt *b = s->body; b; b = b->next) mark_addr_taken_stmt(lt, b);
            break;
        case STMT_FOR:
            mark_addr_taken_stmt(lt, s->init);
            mark_addr_taken_expr(lt, s->expr);
            mark_addr_taken_stmt(lt, s->post);
            for (Stmt *b = s->body; b; b = b->next) mark_addr_taken_stmt(lt, b);
            break;
        case STMT_FOR_IN:
            mark_addr_taken_expr(lt, s->expr);
            for (Stmt *b = s->body; b; b = b->next) mark_addr_taken_stmt(lt, b);
            break;
        case STMT_SWITCH:
            mark_addr_taken_expr(lt, s->expr);
            for (CaseClause *c = s->cases; c; c = c->next) {
                if (c->value) mark_addr_taken_expr(lt, c->value);
                for (Stmt *b = c->body; b; b = b->next) mark_addr_taken_stmt(lt, b);
            }
            break;
        default:
            break;
    }
}

static void mark_addr_taken(LocalTable *lt, Stmt *body) {
    for (Stmt *s = body; s; s = s->next) {
        mark_addr_taken_stmt(lt, s);
    }
}

static int gen_stmt(LocalTable *lt, Stmt *s);

/* Emits a sequence of statements. Returns 1 if the block is guaranteed to terminate (return). */
static int gen_block(LocalTable *lt, Stmt *body) {
    for (Stmt *s = body; s; s = s->next) {
        if (gen_stmt(lt, s)) return 1;
    }
    return 0;
}

/* Emits IL for expr, returns the QBE temp name holding the result (e.g. "%t3"), or NULL if void. */
static char *gen_expr_inner(LocalTable *lt, Expr *e, char *buf, size_t bufsz) {
    switch (e->kind) {
        case EXPR_INT_LITERAL:
        case EXPR_CHAR_LITERAL:
        case EXPR_BOOL_LITERAL:
        case EXPR_NULL_LITERAL: { /* M33: null is runtime value 0 */
            int t = new_tmp();
            fprintf(out, "\t%%t%d =l copy %ld\n", t, e->num);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_FLOAT_LITERAL: {
            /* M25: literals default to double (matches semantic.c); QBE
               wants an `s_`/`d_` prefixed constant for float/double
               immediates (`d_3.14`), not a bare decimal. */
            int t = new_tmp();
            fprintf(out, "\t%%t%d =d copy d_%.17g\n", t, e->fnum);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_STRING_LITERAL: {
            int idx = string_intern(e->str);
            int t = new_tmp();
            fprintf(out, "\t%%t%d =l copy $str%d\n", t, idx);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_IDENT: {
            int idx = local_find(lt, e->name);
            if (idx != -1 && lt->addr_taken[idx]) {
                /* Address-taken variable: load from stack address. M25:
                   float/double locals are stack-stored with loads/loadd
                   (matching their QBE class) instead of loadl. */
                TypeKind vt = lt->types[idx];
                char cls = type_qbe_class(vt);
                const char *loadop = (vt == TYPE_FLOAT) ? "loads" : (vt == TYPE_DOUBLE) ? "loadd" : "loadl";
                int t = new_tmp();
                fprintf(out, "\t%%t%d =%c %s %%v_%s\n", t, cls, loadop, e->name);
                snprintf(buf, bufsz, "%%t%d", t);
                return buf;
            } else {
                /* Normal variable: direct SSA temp */
                snprintf(buf, bufsz, "%%v_%s", e->name);
                return buf;
            }
        }
        case EXPR_IO_OUT_CALL: {
            char argbuf[32];
            char *arg = gen_expr(lt, e->arg, argbuf, sizeof(argbuf));
            TypeKind arg_kind = static_type_of(lt, e->arg);
            if (arg_kind == TYPE_STRING) {
                /* M37: NULL-guvenli runtime yardimcisi (bkz. runtime.c). */
                fprintf(out, "\tcall $rapidc_out_str(l %s)\n", arg);
            } else if (arg_kind == TYPE_BYTE) {
                had_fmt_byte = 1;
                fprintf(out, "\tcall $printf(l $fmt_byte, l %s, ...)\n", arg);
            } else if (type_is_float(arg_kind)) {
                /* M25: printf's varargs promote float -> double automatically
                   in C, but QBE's own vararg call syntax has no such
                   promotion — a `float` value must be widened to double
                   explicitly before it's passed, or printf (which always
                   reads a double out of the vararg slot for %g) reads
                   garbage. */
                had_fmt_double = 1;
                if (arg_kind == TYPE_FLOAT) {
                    int t = new_tmp();
                    fprintf(out, "\t%%t%d =d exts %s\n", t, arg);
                    fprintf(out, "\tcall $printf(l $fmt_double, d %%t%d, ...)\n", t);
                } else {
                    fprintf(out, "\tcall $printf(l $fmt_double, d %s, ...)\n", arg);
                }
            } else {
                had_fmt_int = 1;
                fprintf(out, "\tcall $printf(l $fmt_int, l %s, ...)\n", arg);
            }
            return NULL;
        }
        case EXPR_IO_IN_CALL: {
            /* M16: io::in() - read one line from stdin as a fresh string */
            int t = new_tmp();
            fprintf(out, "\t%%t%d =l call $rapidc_readline()\n", t);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_IO_FORMAT_CALL: {
            /* M16: io::format(fmt, args...) - build two parallel stack
               arrays (byte 'kinds' and 8-byte 'vals'), one entry per
               argument, then hand them to the runtime helper which does
               the actual "{}" substitution + printf-style formatting. */
            char fmtbuf[32];
            char *fmt = gen_expr(lt, e->lhs, fmtbuf, sizeof(fmtbuf));

            int argc = 0;
            for (ExprList *l = e->args; l; l = l->next) argc++;

            int kinds_base = new_tmp();
            int vals_base = new_tmp();
            fprintf(out, "\t%%t%d =l alloc8 %d\n", kinds_base, argc > 0 ? argc : 1);
            fprintf(out, "\t%%t%d =l alloc8 %d\n", vals_base, (argc > 0 ? argc : 1) * 8);

            int i = 0;
            for (ExprList *l = e->args; l; l = l->next, i++) {
                char argbuf[32];
                char *arg = gen_expr(lt, l->expr, argbuf, sizeof(argbuf));
                TypeKind at = static_type_of(lt, l->expr);
                char kind_char = (at == TYPE_STRING) ? 's' : (at == TYPE_BYTE) ? 'c' : 'i';

                if (i == 0) {
                    fprintf(out, "\tstoreb %d, %%t%d\n", (int)kind_char, kinds_base);
                    fprintf(out, "\tstorel %s, %%t%d\n", arg, vals_base);
                } else {
                    int kaddr = new_tmp();
                    fprintf(out, "\t%%t%d =l add %%t%d, %d\n", kaddr, kinds_base, i);
                    fprintf(out, "\tstoreb %d, %%t%d\n", (int)kind_char, kaddr);
                    int vaddr = new_tmp();
                    fprintf(out, "\t%%t%d =l add %%t%d, %d\n", vaddr, vals_base, i * 8);
                    fprintf(out, "\tstorel %s, %%t%d\n", arg, vaddr);
                }
            }

            fprintf(out, "\tcall $rapidc_format(l %s, l %%t%d, l %%t%d, l %d)\n",
                    fmt, kinds_base, vals_base, argc);
            return NULL;
        }
        case EXPR_IO_SYSCALL_CALL: {
            /* M18: io::open/read/write/close - straight passthrough calls
               into the runtime wrappers, same style as sysprog::alloc etc. */
            const char *rt_name =
                strcmp(e->name, "open") == 0  ? "rapidc_open"  :
                strcmp(e->name, "read") == 0  ? "rapidc_read"  :
                strcmp(e->name, "write") == 0 ? "rapidc_write" :
                strcmp(e->name, "close") == 0 ? "rapidc_close" : "rapidc_errno";

            char argbufs[3][32];
            char *argvals[3];
            int n = 0;
            for (ExprList *l = e->args; l && n < 3; l = l->next, n++) {
                argvals[n] = gen_expr(lt, l->expr, argbufs[n], sizeof(argbufs[n]));
            }

            int t = new_tmp();
            fprintf(out, "\t%%t%d =l call $%s(", t, rt_name);
            for (int i = 0; i < n; i++) {
                if (i > 0) fprintf(out, ", ");
                fprintf(out, "l %s", argvals[i]);
            }
            fprintf(out, ")\n");
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_ARRAY_LITERAL: {
            /* Allocate a fresh stack block, store each element at base+stride*i,
               return the (stable) base pointer as the array "value". M?: element
               width now follows the literal's element kind (e->elem_type, which
               semantic.c set), not a hardcoded 8-byte int slot. */
            int n = e->elem_count;
            TypeKind elem = e->elem_type ? e->elem_type : (e->elements ? static_type_of(lt, e->elements->expr) : TYPE_INT);
            if (elem == TYPE_VOID) elem = TYPE_INT;
            int sz = type_size(elem);
            const char *storeop;
            switch (elem) {
                case TYPE_INT8: case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL:
                    storeop = "storeb"; break;
                case TYPE_INT16: case TYPE_UINT16:
                    storeop = "storeh"; break;
                case TYPE_INT32: case TYPE_UINT32:
                    storeop = "storew"; break;
                case TYPE_FLOAT:
                    storeop = "stores"; break;
                case TYPE_DOUBLE:
                    storeop = "stored"; break;
                default:
                    storeop = "storel"; break; /* TYPE_INT/TYPE_STRUCT/TYPE_PTR */
            }
            int base = new_tmp();
            fprintf(out, "\t%%t%d =l alloc8 %d\n", base, n > 0 ? n * sz : sz);
            char cls = type_qbe_class(elem);
            int i = 0;
            for (ExprList *l = e->elements; l; l = l->next, i++) {
                /* Coerce a narrower/wider element value into this array's
                   element class (e.g. int literal 65 for a byte[]; a double
                   literal for a float[]), mirroring the var-decl float
                   widen/narrow step — QBE has no implicit cross-class move. */
                TypeKind et = static_type_of(lt, l->expr);
                char elbuf[32];
                char *el = gen_expr(lt, l->expr, elbuf, sizeof(elbuf));
                char convbuf[32];
                if (needs_float_conv(et, elem)) {
                    int c = new_tmp();
                    const char *op = (cls == 'd') ? "exts" : "truncd";
                    fprintf(out, "\t%%t%d =%c %s %s\n", c, cls, op, el);
                    snprintf(convbuf, sizeof(convbuf), "%%t%d", c);
                    el = convbuf;
                }
                if (i == 0) {
                    fprintf(out, "\t%s %s, %%t%d\n", storeop, el, base);
                } else {
                    int addr = new_tmp();
                    fprintf(out, "\t%%t%d =l add %%t%d, %d\n", addr, base, i * sz);
                    fprintf(out, "\t%s %s, %%t%d\n", storeop, el, addr);
                }
            }
            snprintf(buf, bufsz, "%%t%d", base);
            return buf;
        }
        case EXPR_INDEX: {
            char arrbuf[32], idxbuf[32];
            char *arr = gen_expr(lt, e->lhs, arrbuf, sizeof(arrbuf));
            char *idx = gen_expr(lt, e->rhs, idxbuf, sizeof(idxbuf));
            TypeKind container_type = static_type_of(lt, e->lhs);
            int t = new_tmp();
            if (container_type == TYPE_STRING) {
                /* string[i]: bounds-check via strlen, then pointer arithmetic
                   by 1 byte and load a single byte. */
                gen_string_bounds_check(arr, idx);
                int addr = new_tmp();
                fprintf(out, "\t%%t%d =l add %s, %s\n", addr, arr, idx);
                fprintf(out, "\t%%t%d =l loadub %%t%d\n", t, addr);
            } else if (container_type == TYPE_BYTE_LIST) {
                /* M20: list<byte>[i]: header + packed 1-byte elements */
                char addrbuf[32];
                char *addr = gen_elem_addr(container_type, arr, idx, addrbuf, sizeof(addrbuf));
                fprintf(out, "\t%%t%d =l loadub %s\n", t, addr);
            } else if (container_type == TYPE_PTR) {
                /* M26: T*[i] — address = base + i*sizeof(T), load
                   with the element's width. `i` is scaled by type_size(elem)
                   (1 for byte*, no mul needed), matching p+n arithmetic.
                   (M35: the T[][i] fixed-size-array half of this used to
                   share this branch; that type is gone now.) */
                TypeKind elem = static_elem_type_of(lt, e->lhs);
                int sz = type_size(elem);
                char offbuf[32];
                if (sz == 1) {
                    snprintf(offbuf, sizeof(offbuf), "%s", idx);
                } else {
                    int s = new_tmp();
                    fprintf(out, "\t%%t%d =l mul %s, %d\n", s, idx, sz);
                    snprintf(offbuf, sizeof(offbuf), "%%t%d", s);
                }
                int addr = new_tmp();
                fprintf(out, "\t%%t%d =l add %s, %s\n", addr, arr, offbuf);
                char addrbuf[32];
                snprintf(addrbuf, sizeof(addrbuf), "%%t%d", addr);
                char cls = type_qbe_class(elem);
                const char *loadop;
                switch (elem) {
                    case TYPE_INT8:  loadop = "loadsb"; break;
                    case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL: loadop = "loadub"; break;
                    case TYPE_INT16:  loadop = "loadsh"; break;
                    case TYPE_UINT16: loadop = "loaduh"; break;
                    case TYPE_INT32:  loadop = "loadsw"; break;
                    case TYPE_UINT32: loadop = "loaduw"; break;
                    case TYPE_FLOAT:  loadop = "loads"; break;
                    case TYPE_DOUBLE: loadop = "loadd"; break;
                    default: loadop = "loadl"; break; /* TYPE_INT/TYPE_STRUCT/TYPE_PTR */
                }
                fprintf(out, "\t%%t%d =%c %s %s\n", t, cls, loadop, addrbuf);
            } else {
                char addrbuf[32];
                char *addr = gen_elem_addr(container_type, arr, idx, addrbuf, sizeof(addrbuf));
                fprintf(out, "\t%%t%d =l loadl %s\n", t, addr);
            }
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_ADDR_OF: {
            /* Address-of: get the address of an lvalue (variable, array
               element, or struct field). For address-taken variables, the
               variable holds the stack address. */
            if (e->arg->kind == EXPR_IDENT) {
                int idx = local_find(lt, e->arg->name);
                if (idx != -1 && lt->addr_taken[idx]) {
                    snprintf(buf, bufsz, "%%v_%s", e->arg->name);
                    return buf;
                }
            } else if (e->arg->kind == EXPR_INDEX) {
                /* Address of array/list/pointer element. For fixed-size arrays
                   (T[]) and pointers (T*) the index is scaled by sizeof(T);
                   for dynamic lists the gen_elem_addr header+stride path is
                   reused as before. */
                char arrbuf[32], idxbuf[32], addrbuf[32];
                char *arr = gen_expr(lt, e->arg->lhs, arrbuf, sizeof(arrbuf));
                char *idx = gen_expr(lt, e->arg->rhs, idxbuf, sizeof(idxbuf));
                TypeKind container_type = static_type_of(lt, e->arg->lhs);
                char *addr = gen_scaled_idx_addr(lt, container_type, e->arg->lhs, arr, idx, addrbuf, sizeof(addrbuf));
                snprintf(buf, bufsz, "%s", addr);
                return buf;
            } else if (e->arg->kind == EXPR_FIELD && e->arg->lhs->kind == EXPR_IDENT) {
                /* M8: address of struct field: &p.y = base + offset */
                char bbuf[32];
                char *base = gen_expr(lt, e->arg->lhs, bbuf, sizeof(bbuf));
                StructDef *sd = struct_of_var(e->arg->lhs->name);
                Field *fd = struct_field_find(sd, e->arg->name);
                int addr = new_tmp();
                fprintf(out, "\t%%t%d =l add %s, %d\n", addr, base, fd ? fd->offset : 0);
                snprintf(buf, bufsz, "%%t%d", addr);
                return buf;
            }
            /* Should not reach here if pre-pass is correct, but handle anyway */
            char argbuf[32];
            char *arg = gen_expr(lt, e->arg, argbuf, sizeof(argbuf));
            int t = new_tmp();
            fprintf(out, "\t%%t%d =l copy %s\n", t, arg);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_DEREF: {
            /* M26: dereference — load from the pointer address, with the
               load width/opcode picked from the pointee's elem_type
               instead of a hardcoded byte-vs-int check. Signed integer
               widths use loadsb/loadsh/loadsw (sign-extending); unsigned
               widths (and byte, historically always unsigned) use
               loadub/loaduh/loaduw (zero-extending); 64-bit int/uint and
               struct base addresses use loadl unchanged; float/double get
               their own QBE register class (loads/loadd) via
               type_qbe_class(elem), matching the M25 float-local pattern
               above instead of assuming `l` like every non-float case. */
            char argbuf[32];
            char *arg = gen_expr(lt, e->arg, argbuf, sizeof(argbuf));
            TypeKind elem = static_elem_type_of(lt, e->arg);
            char cls = type_qbe_class(elem);
            const char *loadop;
            switch (elem) {
                case TYPE_INT8:  loadop = "loadsb"; break;
                case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL: loadop = "loadub"; break;
                case TYPE_INT16:  loadop = "loadsh"; break;
                case TYPE_UINT16: loadop = "loaduh"; break;
                case TYPE_INT32:  loadop = "loadsw"; break;
                case TYPE_UINT32: loadop = "loaduw"; break;
                case TYPE_FLOAT:  loadop = "loads"; break;
                case TYPE_DOUBLE: loadop = "loadd"; break;
                default: loadop = "loadl"; break; /* TYPE_INT/TYPE_INT64/TYPE_UINT64/TYPE_STRUCT/TYPE_PTR */
            }
            int t = new_tmp();
            fprintf(out, "\t%%t%d =%c %s %s\n", t, cls, loadop, arg);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_CALL: {
            /* M12: sysprog builtins - thin wrappers over libc malloc/free
               and a raw 6-arg Linux syscall, implemented as tiny C helpers
               in runtime.c (rapidc_alloc/rapidc_free/rapidc_syscall6) so
               codegen doesn't need to hand-roll the x86-64 `syscall`
               instruction or manage registers itself; QBE's own register
               allocator handles argument placement for the call just like
               any other runtime call. */
            if (strcmp(e->name, "alloc") == 0) {
                char abuf[32];
                char *sz = gen_expr(lt, e->args->expr, abuf, sizeof(abuf));
                int t = new_tmp();
                fprintf(out, "\t%%t%d =l call $rapidc_alloc(l %s)\n", t, sz);
                snprintf(buf, bufsz, "%%t%d", t);
                return buf;
            }
            if (strcmp(e->name, "free") == 0) {
                char abuf[32];
                char *ptr = gen_expr(lt, e->args->expr, abuf, sizeof(abuf));
                fprintf(out, "\tcall $rapidc_free(l %s)\n", ptr);
                return NULL;
            }
            /* M41: toInt(enum) - enum degeri zaten bir tamsayi, kopya yeterli. */
            if (strcmp(e->name, "__enum_to_int") == 0) {
                char abuf[32];
                char *a = gen_expr(lt, e->args->expr, abuf, sizeof(abuf));
                int t = new_tmp();
                fprintf(out, "\t%%t%d =l copy %s\n", t, a);
                snprintf(buf, bufsz, "%%t%d", t);
                return buf;
            }
            /* M9: list builtins. `push` grows/reallocates at runtime, so the
               result must be written back into the list variable itself
               (semantic.c already required push's first argument to be a
               plain identifier) — everything else is a normal-looking call
               into the runtime helpers in runtime.c. */
            if (strcmp(e->name, "push") == 0) {
                char lbuf[32], vbuf[32];
                char *listval = gen_expr(lt, e->args->expr, lbuf, sizeof(lbuf));
                char *val = gen_expr(lt, e->args->next->expr, vbuf, sizeof(vbuf));
                TypeKind list_type = static_type_of(lt, e->args->expr);
                int t = new_tmp();
                if (list_type == TYPE_BYTE_LIST) {
                    fprintf(out, "\t%%t%d =l call $rapidc_blist_push(l %s, l %s)\n", t, listval, val);
                } else {
                    fprintf(out, "\t%%t%d =l call $rapidc_list_push(l %s, l %s)\n", t, listval, val);
                }
                const char *varname = e->args->expr->name;
                int idx = local_find(lt, varname);
                if (idx != -1 && lt->addr_taken[idx]) {
                    fprintf(out, "\tstorel %%t%d, %%v_%s\n", t, varname);
                } else {
                    fprintf(out, "\t%%v_%s =l copy %%t%d\n", varname, t);
                }
                return NULL;
            }
            char argbufs[16][32];
            char *argvals[16];
            int n = 0;
            for (ExprList *l = e->args; l; l = l->next, n++) {
                argvals[n] = gen_expr(lt, l->expr, argbufs[n], sizeof(argbufs[n]));
            }
            FuncInfo *fi = func_info_find(e->name);
            int is_void = fi && fi->ret_type == TYPE_VOID;

            fprintf(out, "\t");
            int t = -1;
            if (!is_void) {
                t = new_tmp();
                fprintf(out, "%%t%d =%c ", t, fi ? type_qbe_class(fi->ret_type) : 'l');
            }
            /* M27: extern functions call the bare symbol ($printf, $sin, ...)
               — no $f_ mangling, since that name lives outside rapidc-
               generated code (libc or another object linked in). */
            fprintf(out, "call %s%s(", (fi && fi->is_extern) ? "$" : "$f_", e->name);
            {
                int i = 0;
                for (ExprList *l = e->args; l; l = l->next, i++) {
                    /* M25: each argument's QBE class must match its own
                       static type (s/d for float/double params), not a
                       blanket "l" — a float argument passed as `l` would
                       corrupt the SysV float-register argument passing. */
                    TypeKind at = static_type_of(lt, l->expr);
                    fprintf(out, "%c %s%s", type_qbe_class(at), argvals[i], i + 1 < n ? ", " : "");
                }
                /* M27: variadic extern (e.g. printf) — QBE needs an explicit
                   `...` marker after the fixed arguments so it knows where
                   the named parameters end and varargs begin, exactly like
                   the existing io::out/io::format call sites already do. */
                if (fi && fi->is_variadic) {
                    fprintf(out, "%s...", n > 0 ? ", " : "");
                }
            }
            fprintf(out, ")\n");

            if (is_void) return NULL;
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_FUNC_REF: {
            /* M11/M28: a bare function name used as a value — its "value"
               is just the address of its QBE symbol (no closures, so
               nothing else needs capturing). A regular user fn/efn lives
               at the mangled `$f_<name>` symbol, but an `extern fn` (M27)
               is an outside symbol (libc/another linked object) and was
               never emitted with that mangling in the first place — its
               address is just the bare `$<name>` symbol, same distinction
               EXPR_CALL already makes for direct calls above. */
            FuncInfo *fi = func_info_find(e->name);
            int t = new_tmp();
            fprintf(out, "\t%%t%d =l copy %s%s\n", t, (fi && fi->is_extern) ? "$" : "$f_", e->name);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_CALL_INDIRECT: {
            /* M11: call through a fn-typed variable/param — the value held
               is a function's symbol address, so this is a QBE indirect
               call (`call %reg(...)`) instead of a direct `call $f_name`.
               Return value is always treated as a non-void `l` here since
               static_type_of can't know the pointed-to function's real
               signature in this milestone (TYPE_FN carries no payload). */
            char calleebuf[32];
            char *callee = gen_expr(lt, e->lhs, calleebuf, sizeof(calleebuf));
            char argbufs[16][32];
            char *argvals[16];
            char argcls[16];
            int n = 0;
            FnType *ft = e->fn_sig; /* M40: imza biliniyorsa sinif/dondurme dogru uretilir */
            for (ExprList *l = e->args; l && n < 16; l = l->next, n++) {
                argvals[n] = gen_expr(lt, l->expr, argbufs[n], sizeof(argbufs[n]));
                argcls[n] = 'l';
                if (ft && n < ft->param_count && n < 8 && type_is_float(ft->pt[n])) {
                    TypeKind at = static_type_of(lt, l->expr);
                    char want = type_qbe_class(ft->pt[n]);
                    argcls[n] = want;
                    if (type_is_float(at) && at != ft->pt[n]) {
                        int w = new_tmp();
                        if (want == 'd') fprintf(out, "\t%%t%d =d exts %s\n", w, argvals[n]);
                        else             fprintf(out, "\t%%t%d =s truncd %s\n", w, argvals[n]);
                        snprintf(argbufs[n], sizeof(argbufs[n]), "%%t%d", w);
                        argvals[n] = argbufs[n];
                    }
                }
            }
            int is_void = ft && ft->ret == TYPE_VOID;
            char rcls = (ft && type_is_float(ft->ret)) ? type_qbe_class(ft->ret) : 'l';
            int t = new_tmp();
            if (is_void) fprintf(out, "\tcall %s(", callee);
            else         fprintf(out, "\t%%t%d =%c call %s(", t, rcls, callee);
            for (int i = 0; i < n; i++) {
                fprintf(out, "%c %s%s", argcls[i], argvals[i], i + 1 < n ? ", " : "");
            }
            fprintf(out, ")\n");
            if (is_void) return NULL;
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_FIELD: {
            /* M8: `p.f` — struct var's %v_<name> holds the stack base
               pointer; load the field from base + offset.
               BUG-01 fix: when the field is itself struct-typed, its
               "value" (mirroring how a struct variable's value is its
               base pointer, see STMT_VAR_DECL/EXPR_STRUCT_LITERAL) is the
               *address* of the nested struct's storage inside the parent
               — i.e. `addr` itself — not a scalar loaded through that
               address. Loading it as an `l` here previously handed the
               first 8 bytes of the nested struct's own contents back as
               if they were a pointer, so a later `.field` on that bogus
               "pointer" dereferenced garbage (crash or silent garbage
               value depending on what was there). */
            char bbuf[32];
            char *base = gen_expr(lt, e->lhs, bbuf, sizeof(bbuf));
            /* BUG-02 fix: struct_def_of_expr resolves the base's struct
               even when the base is itself a nested field (o.inner.a),
               not just a bare variable (o.inner). */
            StructDef *sd = struct_def_of_expr(e->lhs);
            Field *fd = struct_field_find(sd, e->name);
            int addr = new_tmp();
            fprintf(out, "\t%%t%d =l add %s, %d\n", addr, base, fd ? fd->offset : 0);
            if (fd && fd->type == TYPE_STRUCT) {
                snprintf(buf, bufsz, "%%t%d", addr);
                return buf;
            }
            int t = new_tmp();
            fprintf(out, "\t%%t%d =l loadl %%t%d\n", t, addr);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_ENUM_LITERAL: {
            /* M8: enums are plain ints at runtime. */
            EnumDef *ed = e->enum_name ? enum_find(e->enum_name) : NULL;
            int v = ed ? enum_member_value(ed, e->member_name) : 0;
            int t = new_tmp();
            fprintf(out, "\t%%t%d =l copy %d\n", t, v);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_STRUCT_LITERAL: {
            /* M8: allocate a fresh stack block, zero every field, then store
               each listed initializer at its field offset. The base pointer
               is the struct's "value". */
            /* BUG-2 fix: gen_struct_literal_into now recurses correctly
               when a field is itself struct-typed — it byte-copies the
               nested literal's fields into this field's slot (via
               emit_struct_copy_into) instead of storing the nested
               literal's base pointer as if it were a scalar value, which
               previously left nested fields holding uninitialized stack
               garbage. */
            StructDef *sd = e->name ? struct_find(e->name) : NULL;
            int base = new_tmp();
            fprintf(out, "\t%%t%d =l alloc8 %d\n", base, sd && sd->size > 0 ? sd->size : 8);
            char basebuf[32];
            snprintf(basebuf, sizeof(basebuf), "%%t%d", base);
            gen_struct_literal_into(lt, e, sd, basebuf);
            snprintf(buf, bufsz, "%%t%d", base);
            return buf;
        }
        case EXPR_BINOP: {
            char lbuf[32], rbuf[32];
            char *l = gen_expr(lt, e->lhs, lbuf, sizeof(lbuf));
            char *r = gen_expr(lt, e->rhs, rbuf, sizeof(rbuf));
            TypeKind ltype = static_type_of(lt, e->lhs);
            TypeKind rtype = static_type_of(lt, e->rhs);
            int t = new_tmp();
            /* M25: if either operand is float-family, this is a floating
               binop. Pick a single common class (double wins over float,
               matching semantic.c's type_float_promote), widen whichever
               side is the narrower `s` with `exts` first (QBE's compare/
               arith instructions require both operands to already be the
               same class — there's no implicit float<->double promotion at
               the IL level the way C has). Comparisons still yield a plain
               `l` (0/1) result even though the *inputs* are s/d. */
            if (type_is_float(ltype) || type_is_float(rtype)) {
                char cls = (ltype == TYPE_DOUBLE || rtype == TYPE_DOUBLE) ? 'd' : 's';
                if (cls == 'd' && ltype == TYPE_FLOAT) {
                    int wl = new_tmp();
                    fprintf(out, "\t%%t%d =d exts %s\n", wl, l);
                    snprintf(lbuf, sizeof(lbuf), "%%t%d", wl);
                    l = lbuf;
                }
                if (cls == 'd' && rtype == TYPE_FLOAT) {
                    int wr = new_tmp();
                    fprintf(out, "\t%%t%d =d exts %s\n", wr, r);
                    snprintf(rbuf, sizeof(rbuf), "%%t%d", wr);
                    r = rbuf;
                }
                switch (e->op) {
                    case OP_EQ: fprintf(out, "\t%%t%d =l ceq%c %s, %s\n", t, cls, l, r); break;
                    case OP_NE: fprintf(out, "\t%%t%d =l cne%c %s, %s\n", t, cls, l, r); break;
                    case OP_LT: fprintf(out, "\t%%t%d =l clt%c %s, %s\n", t, cls, l, r); break;
                    case OP_LE: fprintf(out, "\t%%t%d =l cle%c %s, %s\n", t, cls, l, r); break;
                    case OP_GT: fprintf(out, "\t%%t%d =l cgt%c %s, %s\n", t, cls, l, r); break;
                    case OP_GE: fprintf(out, "\t%%t%d =l cge%c %s, %s\n", t, cls, l, r); break;
                    case OP_ADD: fprintf(out, "\t%%t%d =%c add %s, %s\n", t, cls, l, r); break;
                    case OP_SUB: fprintf(out, "\t%%t%d =%c sub %s, %s\n", t, cls, l, r); break;
                    case OP_MUL: fprintf(out, "\t%%t%d =%c mul %s, %s\n", t, cls, l, r); break;
                    case OP_DIV: fprintf(out, "\t%%t%d =%c div %s, %s\n", t, cls, l, r); break;
                    case OP_POW:
                        /* Float power has no QBE instruction; lowered to a
                           runtime helper, same convention as string + via
                           rapidc_strcat below. `d`/`s` class picked by cls. */
                        if (cls == 'd') {
                            fprintf(out, "\t%%t%d =d call $rapidc_fpow(d %s, d %s)\n", t, l, r);
                        } else {
                            fprintf(out, "\t%%t%d =s call $rapidc_fpow_s(s %s, s %s)\n", t, l, r);
                        }
                        break;
                    case OP_MOD: case OP_BAND: case OP_BOR: case OP_BXOR:
                    case OP_SHL: case OP_SHR:
                        /* unreachable: semantic.c restricts these to
                           integer operands, so this branch (float-family)
                           never sees them. */
                        break;
                }
                snprintf(buf, bufsz, "%%t%d", t);
                return buf;
            }
            switch (e->op) {
                case OP_EQ: case OP_NE: {
                    if (ltype == TYPE_STRING && rtype == TYPE_STRING) {
                        /* M36: string ==/!= artik ICERIK karsilastirir (onceden
                           pointer kimligi: "hello" == ("hel"+"lo") false idi).
                           Iki taraf da TYPE_STRING oldugunda strcmp'e iner;
                           `s == null` gibi (null TYPE_PTR'dir) durumlar eskisi
                           gibi pointer karsilastirmasi olarak kalir, cunku
                           strcmp(NULL, ...) cokerdi. */
                        int c = new_tmp();
                        fprintf(out, "\t%%t%d =l call $rapidc_strcmp(l %s, l %s)\n", c, l, r);
                        fprintf(out, "\t%%t%d =l %s %%t%d, 0\n", t, e->op == OP_EQ ? "ceql" : "cnel", c);
                    } else if (e->op == OP_EQ) {
                        fprintf(out, "\t%%t%d =l ceql %s, %s\n", t, l, r);
                    } else {
                        fprintf(out, "\t%%t%d =l cnel %s, %s\n", t, l, r);
                    }
                    break;
                }
                case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
                    if (ltype == TYPE_STRING) {
                        /* Lexicographic string comparison: lower to a
                           strcmp() call (returns <0/0/>0) and compare that
                           against 0, same runtime-helper convention as
                           string `+` (rapidc_strcat) above. Unlike `==`/
                           `!=`, which still compare pointer identity for
                           strings (a pre-existing limitation elsewhere in
                           this switch, not touched here), `<`/`<=`/`>`/`>=`
                           have no sensible pointer-identity reading at
                           all, so they go through content comparison from
                           the start. */
                        int c = new_tmp();
                        fprintf(out, "\t%%t%d =l call $rapidc_strcmp(l %s, l %s)\n", c, l, r);
                        char cbuf[32];
                        snprintf(cbuf, sizeof(cbuf), "%%t%d", c);
                        switch (e->op) {
                            case OP_LT: fprintf(out, "\t%%t%d =l csltl %s, 0\n", t, cbuf); break;
                            case OP_LE: fprintf(out, "\t%%t%d =l cslel %s, 0\n", t, cbuf); break;
                            case OP_GT: fprintf(out, "\t%%t%d =l csgtl %s, 0\n", t, cbuf); break;
                            case OP_GE: fprintf(out, "\t%%t%d =l csgel %s, 0\n", t, cbuf); break;
                            default: break;
                        }
                    } else {
                        switch (e->op) {
                            case OP_LT: fprintf(out, "\t%%t%d =l csltl %s, %s\n", t, l, r); break;
                            case OP_LE: fprintf(out, "\t%%t%d =l cslel %s, %s\n", t, l, r); break;
                            case OP_GT: fprintf(out, "\t%%t%d =l csgtl %s, %s\n", t, l, r); break;
                            case OP_GE: fprintf(out, "\t%%t%d =l csgel %s, %s\n", t, l, r); break;
                            default: break;
                        }
                    }
                    break;
                }
                case OP_ADD: {
                    if (ltype == TYPE_PTR || rtype == TYPE_PTR) {
                        /* M26: pointer arithmetic — `ptr + int` or `int + ptr`.
                           Scales the integer operand by the pointed-to type's
                           byte size so `p + 1` steps one element, not one byte,
                           matching C's PTRDIFF semantics (previously this was a
                           raw byte add, so `*(p+1)` read garbage). */
                        const char *ptr_val = (ltype == TYPE_PTR) ? l : r;
                        const char *off_val = (ltype == TYPE_PTR) ? r : l;
                        TypeKind pelem = static_elem_type_of(lt, (ltype == TYPE_PTR) ? e->lhs : e->rhs);
                        int sz = type_size(pelem);
                        char offbuf[32];
                        if (sz == 1) {
                            snprintf(offbuf, sizeof(offbuf), "%s", off_val);
                        } else {
                            int s = new_tmp();
                            fprintf(out, "\t%%t%d =l mul %s, %d\n", s, off_val, sz);
                            snprintf(offbuf, sizeof(offbuf), "%%t%d", s);
                        }
                        fprintf(out, "\t%%t%d =l add %s, %s\n", t, ptr_val, offbuf);
                    } else if (ltype == TYPE_STRING) {
                        /* string + string = concatenation */
                        fprintf(out, "\t%%t%d =l call $rapidc_strcat(l %s, l %s)\n", t, l, r);
                        /* BUG-4 fix: track this heap allocation so the
                           end-of-statement cleanup can free it if it turns
                           out to be an intermediate (consumed by an outer
                           `+` rather than the statement's final value). */
                        strcat_tmp_record(t);
                    } else {
                        /* int+int, int*+int (byte offset), byte+int — all plain add */
                        fprintf(out, "\t%%t%d =l add %s, %s\n", t, l, r);
                    }
                    break;
                }
                case OP_SUB: {
                    if (ltype == TYPE_PTR && rtype == TYPE_PTR) {
                        /* M26: T* - T* → element difference (not byte
                           difference), matching C. Both sides share the same
                           pointee type (semantic.c already rejected a mismatch). */
                        TypeKind pelem = static_elem_type_of(lt, e->lhs);
                        int sz = type_size(pelem);
                        int d = new_tmp();
                        fprintf(out, "\t%%t%d =l sub %s, %s\n", d, l, r);
                        char dbuf[32];
                        snprintf(dbuf, sizeof(dbuf), "%%t%d", d);
                        if (sz == 1) {
                            snprintf(buf, bufsz, "%s", dbuf);
                            return buf;
                        }
                        int qt = new_tmp();
                        fprintf(out, "\t%%t%d =l copy %d\n", qt, sz);
                        int q = new_tmp();
                        fprintf(out, "\t%%t%d =l div %s, %%t%d\n", q, dbuf, qt);
                        snprintf(buf, bufsz, "%%t%d", q);
                        return buf;
                    }
                    if (ltype == TYPE_PTR && type_is_integer(rtype)) {
                        /* M26: T* - int → T*, scaled by the element size. */
                        TypeKind pelem = static_elem_type_of(lt, e->lhs);
                        int sz = type_size(pelem);
                        char offbuf[32];
                        if (sz == 1) {
                            snprintf(offbuf, sizeof(offbuf), "%s", r);
                        } else {
                            int s = new_tmp();
                            fprintf(out, "\t%%t%d =l mul %s, %d\n", s, r, sz);
                            snprintf(offbuf, sizeof(offbuf), "%%t%d", s);
                        }
                        fprintf(out, "\t%%t%d =l sub %s, %s\n", t, l, offbuf);
                    } else {
                        /* plain integer subtraction */
                        fprintf(out, "\t%%t%d =l sub %s, %s\n", t, l, r);
                    }
                    break;
                }
                case OP_MUL: fprintf(out, "\t%%t%d =l mul %s, %s\n", t, l, r); break;
                case OP_DIV: fprintf(out, "\t%%t%d =l div %s, %s\n", t, l, r); break;
                case OP_MOD: fprintf(out, "\t%%t%d =l rem %s, %s\n", t, l, r); break;
                case OP_BAND: fprintf(out, "\t%%t%d =l and %s, %s\n", t, l, r); break;
                case OP_BOR: fprintf(out, "\t%%t%d =l or %s, %s\n", t, l, r); break;
                case OP_BXOR: fprintf(out, "\t%%t%d =l xor %s, %s\n", t, l, r); break;
                case OP_SHL: fprintf(out, "\t%%t%d =l shl %s, %s\n", t, l, r); break;
                case OP_SHR: fprintf(out, "\t%%t%d =l sar %s, %s\n", t, l, r); break;
                case OP_POW: fprintf(out, "\t%%t%d =l call $rapidc_ipow(l %s, l %s)\n", t, l, r); break;
            }
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_ERROR_LIT: { /* M43 */
            int t = new_tmp();
            fprintf(out, "\t%%t%d =l copy %ld\n", t, e->num);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_TRY: {
            /* M43: cagriyi yap; rapid_err != 0 ise ayni hata ile bu fonksiyondan cik. */
            char abuf[32];
            char *v = gen_expr(lt, e->arg, abuf, sizeof(abuf));
            TypeKind rt = static_type_of(lt, e->arg);
            int res = -1;
            if (v && rt != TYPE_VOID) {
                res = new_tmp();
                fprintf(out, "\t%%t%d =%c copy %s\n", res, type_qbe_class(rt), v);
            }
            int f = new_tmp();
            int lfail = new_label(), lok = new_label();
            fprintf(out, "\t%%t%d =l loadl $rapid_err\n", f);
            fprintf(out, "\tjnz %%t%d, @L%d, @L%d\n", f, lfail, lok);
            fprintf(out, "@L%d\n", lfail);
            emit_zero_ret();
            fprintf(out, "@L%d\n", lok);
            if (res < 0) return NULL;
            snprintf(buf, bufsz, "%%t%d", res);
            return buf;
        }
        case EXPR_CATCH: {
            /* M43: hata varsa hatayi "son hata"ya tasi, temizle ve varsayilani degerlendir. */
            char abuf[32];
            char *v = gen_expr(lt, e->lhs, abuf, sizeof(abuf));
            TypeKind rt = static_type_of(lt, e->lhs);
            char cls = type_qbe_class(rt);
            int res = -1;
            if (v && rt != TYPE_VOID) {
                res = new_tmp();
                fprintf(out, "\t%%t%d =%c copy %s\n", res, cls, v);
            }
            int f = new_tmp();
            int lerr = new_label(), lend = new_label();
            fprintf(out, "\t%%t%d =l loadl $rapid_err\n", f);
            fprintf(out, "\tjnz %%t%d, @L%d, @L%d\n", f, lerr, lend);
            fprintf(out, "@L%d\n", lerr);
            fprintf(out, "\tstorel %%t%d, $rapid_last\n", f);
            fprintf(out, "\tstorel 0, $rapid_err\n");
            char dbuf[32];
            char *d = gen_expr(lt, e->rhs, dbuf, sizeof(dbuf));
            if (res >= 0 && d) fprintf(out, "\t%%t%d =%c copy %s\n", res, cls, d);
            fprintf(out, "\tjmp @L%d\n", lend);
            fprintf(out, "@L%d\n", lend);
            if (res < 0) return NULL;
            snprintf(buf, bufsz, "%%t%d", res);
            return buf;
        }
        case EXPR_NOT: {
            /* !b : bool -> bool. QBE has no dedicated logical-not, so this
               lowers to `ceql b, 0` (bools are already a plain 0/1 `l`
               value, same representation the comparison ops above already
               produce). */
            char argbuf[32];
            char *v = gen_expr(lt, e->arg, argbuf, sizeof(argbuf));
            int t = new_tmp();
            fprintf(out, "\t%%t%d =l ceql %s, 0\n", t, v);
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_LOGAND: {
            /* a && b, short-circuit: if a is false, skip evaluating b
               entirely and the result is false. Lowered with explicit
               branches (same @L/jnz/jmp vocabulary STMT_IF/STMT_WHILE
               already use below) rather than an eager `and`, so a's
               side effects run but b's don't when a is already false. */
            char lbuf[32];
            char *l = gen_expr(lt, e->lhs, lbuf, sizeof(lbuf));
            int result = new_tmp();
            int lrhs = new_label(), lfalse = new_label(), lend = new_label();
            fprintf(out, "\tjnz %s, @L%d, @L%d\n", l, lrhs, lfalse);
            fprintf(out, "@L%d\n", lrhs);
            char rbuf[32];
            char *r = gen_expr(lt, e->rhs, rbuf, sizeof(rbuf));
            fprintf(out, "\t%%t%d =l copy %s\n", result, r);
            fprintf(out, "\tjmp @L%d\n", lend);
            fprintf(out, "@L%d\n", lfalse);
            fprintf(out, "\t%%t%d =l copy 0\n", result);
            fprintf(out, "\tjmp @L%d\n", lend);
            fprintf(out, "@L%d\n", lend);
            snprintf(buf, bufsz, "%%t%d", result);
            return buf;
        }
        case EXPR_LOGOR: {
            /* a || b, short-circuit: if a is true, skip evaluating b
               entirely and the result is true. Mirrors EXPR_LOGAND above
               with the branch sense flipped. */
            char lbuf[32];
            char *l = gen_expr(lt, e->lhs, lbuf, sizeof(lbuf));
            int result = new_tmp();
            int ltrue = new_label(), lrhs = new_label(), lend = new_label();
            fprintf(out, "\tjnz %s, @L%d, @L%d\n", l, ltrue, lrhs);
            fprintf(out, "@L%d\n", lrhs);
            char rbuf[32];
            char *r = gen_expr(lt, e->rhs, rbuf, sizeof(rbuf));
            fprintf(out, "\t%%t%d =l copy %s\n", result, r);
            fprintf(out, "\tjmp @L%d\n", lend);
            fprintf(out, "@L%d\n", ltrue);
            fprintf(out, "\t%%t%d =l copy 1\n", result);
            fprintf(out, "\tjmp @L%d\n", lend);
            fprintf(out, "@L%d\n", lend);
            snprintf(buf, bufsz, "%%t%d", result);
            return buf;
        }
        case EXPR_NEG: {
            /* M29: unary minus. QBE has no dedicated `neg` instruction, so
               this lowers to `sub` from a same-class zero — `0 - x` for
               int, `d_0 - x` / `s_0 - x` for double/float (QBE requires an
               explicit class suffix on a float-typed immediate, same
               convention codegen already uses elsewhere for float
               constants/returns, e.g. the `ret d_0`/`ret s_0` case). */
            char argbuf[32];
            char *v = gen_expr(lt, e->arg, argbuf, sizeof(argbuf));
            TypeKind at = static_type_of(lt, e->arg);
            int t = new_tmp();
            if (at == TYPE_DOUBLE) {
                fprintf(out, "\t%%t%d =d sub d_0, %s\n", t, v);
            } else if (at == TYPE_FLOAT) {
                fprintf(out, "\t%%t%d =s sub s_0, %s\n", t, v);
            } else {
                fprintf(out, "\t%%t%d =l sub 0, %s\n", t, v);
            }
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
    }
    return NULL;
}

/* Returns 1 if this statement terminates the block (return), so caller can stop emitting further code. */
static int gen_stmt(LocalTable *lt, Stmt *s) {
    /* String-concat leak fix: each statement starts with a clean slate of
       tracked strcat temps. gen_stmt is never re-entered mid-statement (the
       recursive calls it makes are for nested statements, e.g. an if-body,
       which get their own top-level gen_stmt call and their own reset), so
       resetting here cannot lose temps that belong to an enclosing
       statement still in progress. */
    strcat_tmp_reset();
    switch (s->kind) {
        case STMT_VAR_DECL:
        case STMT_CONST_DECL: {
            /* M8: struct-typed variables live on the stack; %v_<name> holds
               the base pointer (their "value"), fields are zero-initialized
               and then filled from the struct literal if one is given. */
            if (s->type == TYPE_STRUCT) {
                /* BUG-3 fix: reuse the slot hoist_struct_locals already
                   alloc8'd once in the prologue for this exact declaration
                   site, instead of alloc8'ing a brand-new block every time
                   this statement is reached (which is what made loops over
                   a struct var-decl grow the stack without bound). */
                int hoisted = hoisted_struct_find(s);
                char basebuf[32];
                if (hoisted >= 0) {
                    snprintf(basebuf, sizeof(basebuf), "%%t%d", hoisted);
                } else {
                    /* Fallback (shouldn't happen: hoist_struct_locals walks
                       every var-decl in the function) — allocate normally. */
                    StructDef *sd0 = s->struct_name ? struct_find(s->struct_name) : struct_of_var(s->name);
                    int base0 = new_tmp();
                    fprintf(out, "\t%%t%d =l alloc8 %d\n", base0, sd0 && sd0->size > 0 ? sd0->size : 8);
                    snprintf(basebuf, sizeof(basebuf), "%%t%d", base0);
                }
                if (s->expr && s->expr->kind == EXPR_STRUCT_LITERAL) {
                    /* BUG-3 fix (cont.): when the initializer is a struct
                       literal directly (the common `var p: T = T{...};`
                       loop-body pattern from the bug report), generate its
                       fields straight into our hoisted slot instead of
                       going through gen_expr's own alloc8'd temp block —
                       that intermediate block would itself be a fresh,
                       never-freed allocation on every iteration even
                       though we then copy out of it into basebuf. */
                    StructDef *sd = s->struct_name ? struct_find(s->struct_name)
                                   : (s->expr->name ? struct_find(s->expr->name) : struct_of_var(s->name));
                    gen_struct_literal_into(lt, s->expr, sd, basebuf);
                    fprintf(out, "\t%%v_%s =l copy %s\n", s->name, basebuf);
                } else if (s->expr) {
                    char argbuf[32];
                    char *val = gen_expr(lt, s->expr, argbuf, sizeof(argbuf));
                    /* BUG-01 fix: `val` is a base pointer (either another
                       struct variable's, a struct-literal's fresh block, or
                       — since the EXPR_FIELD fix above — the address of a
                       nested struct field inside its parent). Assigning it
                       straight to %v_<name> would alias that storage rather
                       than giving this new variable independent value
                       semantics, so deep-copy into our (now reused) slot. */
                    StructDef *sd = s->struct_name ? struct_find(s->struct_name) : struct_of_var(s->name);
                    if (!sd && s->expr->kind == EXPR_IDENT) sd = struct_of_var(s->expr->name);
                    emit_struct_copy_into(sd, basebuf, val);
                    fprintf(out, "\t%%v_%s =l copy %s\n", s->name, basebuf);
                } else {
                    StructDef *sd = s->struct_name ? struct_find(s->struct_name) : struct_of_var(s->name);
                    emit_struct_zero(sd, basebuf);
                    fprintf(out, "\t%%v_%s =l copy %s\n", s->name, basebuf);
                }
                return 0;
            }
            char argbuf[32];
            char *val = s->expr
                ? gen_expr(lt, s->expr, argbuf, sizeof(argbuf))
                : NULL; /* unreachable for non-structs (semantic rejects) */
            if (!val) {
                fprintf(out, "\t%%v_%s =l copy 0\n", s->name);
                return 0;
            }
            /* M25: float/double declarations. semantic.c allows a float
               initializer for a double variable and vice versa (implicit
               widen/narrow, mirroring the int family) — but QBE itself has
               no implicit conversion between its `s` and `d` classes, so a
               width mismatch between the declared type and the initializer
               expression's actual type needs an explicit exts/truncd here. */
            if (s->type == TYPE_FLOAT || s->type == TYPE_DOUBLE) {
                TypeKind init_t = static_type_of(lt, s->expr);
                if (s->type == TYPE_DOUBLE && init_t == TYPE_FLOAT) {
                    int w = new_tmp();
                    fprintf(out, "\t%%t%d =d exts %s\n", w, val);
                    snprintf(argbuf, sizeof(argbuf), "%%t%d", w);
                    val = argbuf;
                } else if (s->type == TYPE_FLOAT && init_t == TYPE_DOUBLE) {
                    int w = new_tmp();
                    fprintf(out, "\t%%t%d =s truncd %s\n", w, val);
                    snprintf(argbuf, sizeof(argbuf), "%%t%d", w);
                    val = argbuf;
                }
                char cls = type_qbe_class(s->type);
                int idxf = local_find(lt, s->name);
                if (idxf != -1 && lt->addr_taken[idxf]) {
                    int addr = new_tmp();
                    fprintf(out, "\t%%t%d =l alloc8 8\n", addr);
                    fprintf(out, "\t%s %s, %%t%d\n", (s->type == TYPE_FLOAT) ? "stores" : "stored", val, addr);
                    fprintf(out, "\t%%v_%s =l copy %%t%d\n", s->name, addr);
                } else {
                    fprintf(out, "\t%%v_%s =%c copy %s\n", s->name, cls, val);
                }
                return 0;
            }
            /* M9: `list<int> xs = [1, 2, 3];` — the initializer codegen'd as
               a fixed array-literal stack block (see EXPR_ARRAY_LITERAL);
               wrap it into a heap-backed, growable list header before it
               becomes the variable's value. `list<int> xs = ys;` /
               `= listCopy(ys);` already produce a list header, so only the
               array-literal case needs this extra step. */
            if (s->type == TYPE_INT_LIST && s->expr->kind == EXPR_ARRAY_LITERAL) {
                int n = s->expr->elem_count;
                int hdr = new_tmp();
                fprintf(out, "\t%%t%d =l call $rapidc_list_new(l %s, l %d)\n", hdr, val, n);
                fprintf(out, "\t%%v_%s =l copy %%t%d\n", s->name, hdr);
                return 0;
            }
            /* M20: `list<byte> bs = [];` — semantic.c only allows this with
               an empty literal (no byte-array-literal syntax exists yet),
               so this always builds a fresh empty byte list. */
            if (s->type == TYPE_BYTE_LIST && s->expr->kind == EXPR_ARRAY_LITERAL) {
                int hdr = new_tmp();
                fprintf(out, "\t%%t%d =l call $rapidc_blist_new(l %s, l 0)\n", hdr, val);
                fprintf(out, "\t%%v_%s =l copy %%t%d\n", s->name, hdr);
                return 0;
            }
            int idx = local_find(lt, s->name);
            if (idx != -1 && lt->addr_taken[idx]) {
                /* Address-taken variable: allocate on stack */
                int addr = new_tmp();
                fprintf(out, "\t%%t%d =l alloc8 8\n", addr);
                fprintf(out, "\tstorel %s, %%t%d\n", val, addr);
                /* Store the address as the variable's "value" for address-of */
                fprintf(out, "\t%%v_%s =l copy %%t%d\n", s->name, addr);
            } else {
                /* Normal variable: keep in SSA temp */
                fprintf(out, "\t%%v_%s =l copy %s\n", s->name, val);
            }
            /* String-concat leak fix: `val` (now also owned by %v_<name>)
               survives; any other strcat intermediate from this
               initializer expression is unreachable and must be freed.
               Must run AFTER %v_<name> is bound to val, not before --
               val's underlying allocation must NOT be in the freed set. */
            strcat_tmp_free_all_except(val);
            return 0;
        }
        case STMT_FAIL: { /* M43 */
            char argbuf[32];
            char *val = gen_expr(lt, s->expr, argbuf, sizeof(argbuf));
            fprintf(out, "\tcall $rapidc_fail(l %s)\n", val ? val : "1");
            emit_zero_ret();
            return 1;
        }
        case STMT_RETURN: {
            if (s->expr) {
                char argbuf[32];
                char *val = gen_expr(lt, s->expr, argbuf, sizeof(argbuf));
                /* The returned value escapes the function; free every
                   other tracked strcat intermediate, keeping val. */
                strcat_tmp_free_all_except(val);
                /* M29 fix: an `efn` wrapping a void-returning call (e.g. a
                   keyword-rename forwarding wrapper over a void extern fn)
                   desugars to `return <void call expr>;` (see efn_new in
                   ast.c, which always builds a `return expr` node
                   regardless of the expr's inferred type). gen_expr returns
                   NULL for a void-typed expression (its value is not
                   meaningful/usable), so without this check the call below
                   printed the literal string "(null)" as the return value
                   ("ret (null)"), which QBE rejects as invalid IL. A NULL
                   value here always means the expression's type is void,
                   so the correct QBE return is the bare `ret` form (same
                   as the `!s->expr` branch), matching the void-return case
                   normal `fn` bodies already emit correctly on line 1144. */
                if (val) {
                    fprintf(out, "\tret %s\n", val);
                } else {
                    fprintf(out, "\tret\n");
                }
            } else {
                fprintf(out, "\tret\n");
            }
            return 1;
        }
        case STMT_EXPR: {
            char argbuf[32];
            gen_expr(lt, s->expr, argbuf, sizeof(argbuf));
            /* A bare expression statement's value, if any, is always
               discarded -- nothing escapes this statement, so every
               tracked strcat result (including the outermost, top-level
               one) is dead. E.g. a lone `a + b;` statement, or a call
               whose argument expression built a throwaway concatenation
               that the call itself copies rather than takes ownership of. */
            strcat_tmp_free_all_except(NULL);
            return 0;
        }
        case STMT_IF: {
            char condbuf[32];
            char *cond = gen_expr(lt, s->expr, condbuf, sizeof(condbuf));
            /* The condition result (always a bool/int comparison, never
               itself a bare strcat result) is used below by `jnz`; any
               strcat intermediate that fed into computing it is now dead. */
            strcat_tmp_free_all_except(cond);

            int lthen = new_label();
            int lelse = new_label();
            int lend = new_label();

            fprintf(out, "\tjnz %s, @L%d, @L%d\n", cond, lthen, lelse);

            fprintf(out, "@L%d\n", lthen);
            int then_term = gen_block(lt, s->then_body);
            if (!then_term) fprintf(out, "\tjmp @L%d\n", lend);

            fprintf(out, "@L%d\n", lelse);
            int else_term = 0;
            if (s->else_body) {
                if (s->else_body->kind == STMT_IF && s->else_body->next == NULL) {
                    else_term = gen_stmt(lt, s->else_body);
                } else {
                    else_term = gen_block(lt, s->else_body);
                }
            }
            if (!else_term) fprintf(out, "\tjmp @L%d\n", lend);

            if (then_term && else_term) {
                /* both branches return; @L%d is unreachable, but QBE requires
                   every block to end in a jump/return, and a label must start
                   a new block. Emit a dead block so @Lend is well-formed. */
                fprintf(out, "@L%d\n\thlt\n", lend);
                return 1;
            }

            fprintf(out, "@L%d\n", lend);
            return 0;
        }
        case STMT_ASSIGN: {
            int idx_pre = local_find(lt, s->name);
            TypeKind vt_pre = (idx_pre != -1) ? lt->types[idx_pre] : TYPE_INT;
            if (vt_pre == TYPE_STRUCT && s->expr->kind == EXPR_STRUCT_LITERAL) {
                /* BUG-5 fix: like the BUG-3 fix in STMT_VAR_DECL, a struct
                   literal assigned to an *existing* variable (`tmp = T{...};`,
                   as opposed to `var tmp: T = T{...};`) inside a loop body
                   still went through gen_expr's general EXPR_STRUCT_LITERAL
                   path, which alloc8's its own fresh block every time it's
                   evaluated — QBE never frees it, so a loop doing `tmp =
                   T{...};` every iteration grew the stack without bound
                   exactly like the original BUG-3, just reached through
                   plain assignment instead of a var-decl. Generate the
                   literal's fields directly into tmp's own existing slot
                   (%v_tmp, stable since tmp's declaration) instead of
                   through an intermediate temp block. */
                StructDef *sd = struct_of_var(s->name);
                if (!sd && s->expr->name) sd = struct_find(s->expr->name);
                char dstbuf[32];
                snprintf(dstbuf, sizeof(dstbuf), "%%v_%s", s->name);
                gen_struct_literal_into(lt, s->expr, sd, dstbuf);
                return 0;
            }
            char valbuf[32];
            char *val = gen_expr(lt, s->expr, valbuf, sizeof(valbuf));
            /* String-concat leak fix: val (about to be stored into
               %v_<name>, replacing whatever that variable held before)
               survives; every other tracked strcat intermediate from this
               RHS expression is dead and must be freed. Note: this does
               NOT free the variable's *previous* value (`x = x + y;`
               overwrites %v_x without freeing the old string it pointed
               to) -- that is a separate, pre-existing use-after-assign
               leak class (the old value, not an unreachable temp), not
               part of this fix. */
            strcat_tmp_free_all_except(val);
            int idx = local_find(lt, s->name);
            TypeKind vt = (idx != -1) ? lt->types[idx] : TYPE_INT;
            if (vt == TYPE_STRUCT) {
                /* BUG-4 fix: `b = a;` (plain re-assignment of an existing
                   struct variable, as opposed to `var b: T = a;`) used to
                   emit a raw `%v_b =l copy val`, repointing b's "value" at
                   a's storage instead of copying into b's own existing
                   block — after that, b and a alias the same memory, so
                   mutating one silently corrupts the other. b's `%v_b`
                   already holds a stable base pointer from its original
                   declaration; byte-copy the RHS's fields into that
                   existing block (same convention STMT_VAR_DECL and
                   STMT_FIELD_ASSIGN already use for struct-by-value
                   semantics) instead of repointing it. */
                StructDef *sd = struct_of_var(s->name);
                if (!sd && s->expr->kind == EXPR_IDENT) sd = struct_of_var(s->expr->name);
                char dstbuf[32];
                snprintf(dstbuf, sizeof(dstbuf), "%%v_%s", s->name);
                emit_struct_copy_into(sd, dstbuf, val);
                return 0;
            }
            if (vt == TYPE_FLOAT || vt == TYPE_DOUBLE) {
                /* M25: same exts/truncd width-matching as STMT_VAR_DECL. */
                TypeKind rhs_t = static_type_of(lt, s->expr);
                if (vt == TYPE_DOUBLE && rhs_t == TYPE_FLOAT) {
                    int w = new_tmp();
                    fprintf(out, "\t%%t%d =d exts %s\n", w, val);
                    snprintf(valbuf, sizeof(valbuf), "%%t%d", w);
                    val = valbuf;
                } else if (vt == TYPE_FLOAT && rhs_t == TYPE_DOUBLE) {
                    int w = new_tmp();
                    fprintf(out, "\t%%t%d =s truncd %s\n", w, val);
                    snprintf(valbuf, sizeof(valbuf), "%%t%d", w);
                    val = valbuf;
                }
                if (idx != -1 && lt->addr_taken[idx]) {
                    fprintf(out, "\t%s %s, %%v_%s\n", (vt == TYPE_FLOAT) ? "stores" : "stored", val, s->name);
                } else {
                    fprintf(out, "\t%%v_%s =%c copy %s\n", s->name, type_qbe_class(vt), val);
                }
                return 0;
            }
            if (idx != -1 && lt->addr_taken[idx]) {
                /* Address-taken variable: store to stack address */
                fprintf(out, "\tstorel %s, %%v_%s\n", val, s->name);
            } else {
                fprintf(out, "\t%%v_%s =l copy %s\n", s->name, val);
            }
            return 0;
        }
        case STMT_INDEX_ASSIGN: {
            char arrbuf[32], idxbuf[32], valbuf[32];
            char *arr = gen_expr(lt, s->target, arrbuf, sizeof(arrbuf));
            char *idx = gen_expr(lt, s->index, idxbuf, sizeof(idxbuf));
            char *val = gen_expr(lt, s->value, valbuf, sizeof(valbuf));
            /* val is stored in every branch below (never discarded), so
               one cleanup here (keeping val) covers them all. */
            strcat_tmp_free_all_except(val);
            TypeKind container_type = static_type_of(lt, s->target);
            if (container_type == TYPE_STRING) {
                /* M16: s[i] = b — bounds-check via strlen, then same
                   1-byte pointer arithmetic as the EXPR_INDEX read path,
                   but storeb instead of loadub. */
                gen_string_bounds_check(arr, idx);
                int addr = new_tmp();
                fprintf(out, "\t%%t%d =l add %s, %s\n", addr, arr, idx);
                fprintf(out, "\tstoreb %s, %%t%d\n", val, addr);
                return 0;
            }
            if (container_type == TYPE_BYTE_LIST) {
                /* M20: bs[i] = b — header + packed 1-byte elements */
                char addrbuf[32];
                char *addr = gen_elem_addr(container_type, arr, idx, addrbuf, sizeof(addrbuf));
                fprintf(out, "\tstoreb %s, %s\n", val, addr);
                return 0;
            }
            if (container_type == TYPE_PTR) {
                /* M26-add: p[i] = v — address = base + i*sizeof(T),
                   stored with the element's width, mirroring EXPR_INDEX's read
                   path (gen_scaled_idx_addr). (M35: the a[i] = v fixed-size-
                   array half of this used to share this branch.) */
                TypeKind elem = static_elem_type_of(lt, s->target);
                int sz = type_size(elem);
                char offbuf[32];
                if (sz == 1) {
                    snprintf(offbuf, sizeof(offbuf), "%s", idx);
                } else {
                    int s = new_tmp();
                    fprintf(out, "\t%%t%d =l mul %s, %d\n", s, idx, sz);
                    snprintf(offbuf, sizeof(offbuf), "%%t%d", s);
                }
                int addr = new_tmp();
                fprintf(out, "\t%%t%d =l add %s, %s\n", addr, arr, offbuf);
                char addrbuf[32];
                snprintf(addrbuf, sizeof(addrbuf), "%%t%d", addr);
                const char *storeop;
                switch (elem) {
                    case TYPE_INT8: case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL:
                        storeop = "storeb"; break;
                    case TYPE_INT16: case TYPE_UINT16:
                        storeop = "storeh"; break;
                    case TYPE_INT32: case TYPE_UINT32:
                        storeop = "storew"; break;
                    case TYPE_FLOAT:
                        storeop = "stores"; break;
                    case TYPE_DOUBLE:
                        storeop = "stored"; break;
                    default:
                        storeop = "storel"; break; /* TYPE_INT/TYPE_STRUCT/TYPE_PTR */
                }
                fprintf(out, "\t%s %s, %s\n", storeop, val, addrbuf);
                return 0;
            }
            char addrbuf[32];
            char *addr = gen_elem_addr(container_type, arr, idx, addrbuf, sizeof(addrbuf));
            fprintf(out, "\tstorel %s, %s\n", val, addr);
            return 0;
        }
        case STMT_DEREF_ASSIGN: {
            /* M26: *p = x — store width/opcode picked from p's pointee
               elem_type (storeb/storeh/storew for the narrow integer
               widths — QBE has no signed/unsigned distinction on stores,
               only on loads — storel for 64-bit int/uint/struct/ptr, and
               stores/stored for float/double). */
            char targbuf[32], valbuf[32];
            char *target = gen_expr(lt, s->target, targbuf, sizeof(targbuf));
            char *val = gen_expr(lt, s->value, valbuf, sizeof(valbuf));
            TypeKind elem = static_elem_type_of(lt, s->target);
            /* M26: same exts/truncd width-matching as STMT_VAR_DECL — the
               value expression's own QBE class (s or d) may not match the
               pointee's class (e.g. a bare float-literal RHS is always
               generated as TYPE_DOUBLE by static_type_of/gen_expr, per
               M25's documented default, but the pointee here is float*). */
            if (elem == TYPE_FLOAT || elem == TYPE_DOUBLE) {
                TypeKind val_t = static_type_of(lt, s->value);
                if (elem == TYPE_DOUBLE && val_t == TYPE_FLOAT) {
                    int w = new_tmp();
                    fprintf(out, "\t%%t%d =d exts %s\n", w, val);
                    snprintf(valbuf, sizeof(valbuf), "%%t%d", w);
                    val = valbuf;
                } else if (elem == TYPE_FLOAT && val_t == TYPE_DOUBLE) {
                    int w = new_tmp();
                    fprintf(out, "\t%%t%d =s truncd %s\n", w, val);
                    snprintf(valbuf, sizeof(valbuf), "%%t%d", w);
                    val = valbuf;
                }
            }
            const char *storeop;
            switch (elem) {
                case TYPE_INT8: case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL:
                    storeop = "storeb"; break;
                case TYPE_INT16: case TYPE_UINT16:
                    storeop = "storeh"; break;
                case TYPE_INT32: case TYPE_UINT32:
                    storeop = "storew"; break;
                case TYPE_FLOAT:
                    storeop = "stores"; break;
                case TYPE_DOUBLE:
                    storeop = "stored"; break;
                default:
                    storeop = "storel"; break; /* TYPE_INT/TYPE_INT64/TYPE_UINT64/TYPE_STRUCT/TYPE_PTR */
            }
            fprintf(out, "\t%s %s, %s\n", storeop, val, target);
            /* val now lives in *target and survives; free any other
               tracked strcat intermediate from this statement's exprs. */
            strcat_tmp_free_all_except(val);
            return 0;
        }
        case STMT_FIELD_ASSIGN: {
            /* M8: `p.f = expr;` — store through the struct's base pointer
               at the field's offset.
               BUG-03 fix: target used to be required to be a bare
               EXPR_IDENT (`return 0` no-op'd anything else away, so
               `o.inner.a = 7;` silently compiled to nothing); target may
               now be any struct-typed expression (e.g. the EXPR_FIELD
               `o.inner`), whose address gen_expr already computes correctly
               (see EXPR_FIELD above), and struct_def_of_expr resolves its
               struct definition the same way.
               Also BUG-03: when the field itself is struct-typed (`o.inner
               = i;`), `val` from gen_expr(s->value) is i's base *pointer*
               (BUG-01's struct-by-value convention) — storing that pointer
               directly into the field slot would alias o.inner with i's
               storage instead of copying i's bytes in, so that case must
               go through emit_struct_copy_into (word-for-word copy into
               the field's own already-existing storage) instead of a
               single scalar store. */
            char bbuf[32], valbuf[32];
            char *base = gen_expr(lt, s->target, bbuf, sizeof(bbuf));
            StructDef *sd = struct_def_of_expr(s->target);
            Field *fd = struct_field_find(sd, s->field);
            if (fd && fd->type == TYPE_STRUCT) {
                int addr = new_tmp();
                fprintf(out, "\t%%t%d =l add %s, %d\n", addr, base, fd->offset);
                char addrbuf[32];
                snprintf(addrbuf, sizeof(addrbuf), "%%t%d", addr);
                StructDef *field_sd = struct_find(fd->struct_name);
                if (s->value->kind == EXPR_STRUCT_LITERAL) {
                    /* BUG-5-adjacent fix: same leak as STMT_ASSIGN's
                       struct-literal case — generate the literal directly
                       into the field's slot instead of through gen_expr's
                       own alloc8'd temp block, which loop bodies doing
                       `o.inner = T{...};` every iteration would otherwise
                       leak one block of per iteration. */
                    StructDef *lit_sd = field_sd ? field_sd : (s->value->name ? struct_find(s->value->name) : NULL);
                    gen_struct_literal_into(lt, s->value, lit_sd, addrbuf);
                } else {
                    char *val = gen_expr(lt, s->value, valbuf, sizeof(valbuf));
                    emit_struct_copy_into(field_sd, addrbuf, val);
                    strcat_tmp_free_all_except(val);
                }
                return 0;
            }
            char *val = gen_expr(lt, s->value, valbuf, sizeof(valbuf));
            emit_store_at(base, fd ? fd->offset : 0, val);
            strcat_tmp_free_all_except(val);
            return 0;
        }
        case STMT_INC: {
            int idx = local_find(lt, s->name);
            if (idx != -1 && lt->addr_taken[idx]) {
                /* Address-taken variable: load, add, store */
                TypeKind tkind = lt->types[idx];
                /* M26-add: pointer ++ advances by the pointed-to type's size
                   (1 for byte*), not by 1 byte. */
                int step = (tkind == TYPE_PTR) ? type_size(lt->elem_types[idx]) : 1;
                int t = new_tmp();
                fprintf(out, "\t%%t%d =l loadl %%v_%s\n", t, s->name);
                fprintf(out, "\t%%t%d =l add %%t%d, %d\n", t, t, step);
                fprintf(out, "\tstorel %%t%d, %%v_%s\n", t, s->name);
            } else {
                TypeKind tkind = (idx != -1) ? lt->types[idx] : TYPE_INT;
                int step = (tkind == TYPE_PTR) ? type_size(lt->elem_types[idx]) : 1;
                fprintf(out, "\t%%v_%s =l add %%v_%s, %d\n", s->name, s->name, step);
            }
            return 0;
        }
        case STMT_DEC: {
            int idx = local_find(lt, s->name);
            if (idx != -1 && lt->addr_taken[idx]) {
                TypeKind tkind = lt->types[idx];
                int step = (tkind == TYPE_PTR) ? type_size(lt->elem_types[idx]) : 1;
                int t = new_tmp();
                fprintf(out, "\t%%t%d =l loadl %%v_%s\n", t, s->name);
                fprintf(out, "\t%%t%d =l sub %%t%d, %d\n", t, t, step);
                fprintf(out, "\tstorel %%t%d, %%v_%s\n", t, s->name);
            } else {
                TypeKind tkind = (idx != -1) ? lt->types[idx] : TYPE_INT;
                int step = (tkind == TYPE_PTR) ? type_size(lt->elem_types[idx]) : 1;
                fprintf(out, "\t%%v_%s =l sub %%v_%s, %d\n", s->name, s->name, step);
            }
            return 0;
        }
        case STMT_WHILE: {
            int lcond = new_label();
            int lbody = new_label();
            int lend = new_label();

            fprintf(out, "\tjmp @L%d\n", lcond);
            fprintf(out, "@L%d\n", lcond);
            char condbuf[32];
            char *cond = gen_expr(lt, s->expr, condbuf, sizeof(condbuf));
            /* Re-executed every iteration (this IL sits at @Lcond, which
               the loop jumps back to) -- must free any strcat
               intermediate the condition built, every time, not just once. */
            strcat_tmp_free_all_except(cond);
            fprintf(out, "\tjnz %s, @L%d, @L%d\n", cond, lbody, lend);

            fprintf(out, "@L%d\n", lbody);
            int ok_while = loop_depth_push_ok();
            if (ok_while) {
                loop_continue_label[loop_depth] = lcond;
                loop_break_label[loop_depth] = lend;
                loop_is_for_in[loop_depth] = 0;
                loop_depth++;
            }
            int term = gen_block(lt, s->body);
            if (ok_while) loop_depth--;
            if (!term) fprintf(out, "\tjmp @L%d\n", lcond);

            fprintf(out, "@L%d\n", lend);
            return 0;
        }
        case STMT_DO_WHILE: {
            /* do { body } while (cond); — body runs once unconditionally
               before the first condition check, then loops back to the
               body (not to a separate cond-check-first block) as long as
               cond holds. Structurally the same three-label shape as
               STMT_WHILE, just with the unconditional entry jump landing
               on lbody instead of lcond, and lcond placed after the body
               so `continue` still lands on the condition check rather than
               skipping it. */
            int lbody = new_label();
            int lcond = new_label();
            int lend = new_label();

            fprintf(out, "\tjmp @L%d\n", lbody);
            fprintf(out, "@L%d\n", lbody);
            int ok_dowhile = loop_depth_push_ok();
            if (ok_dowhile) {
                loop_continue_label[loop_depth] = lcond;
                loop_break_label[loop_depth] = lend;
                loop_is_for_in[loop_depth] = 0;
                loop_depth++;
            }
            int term = gen_block(lt, s->body);
            if (ok_dowhile) loop_depth--;
            if (!term) fprintf(out, "\tjmp @L%d\n", lcond);

            fprintf(out, "@L%d\n", lcond);
            char condbuf[32];
            char *cond = gen_expr(lt, s->expr, condbuf, sizeof(condbuf));
            strcat_tmp_free_all_except(cond);
            fprintf(out, "\tjnz %s, @L%d, @L%d\n", cond, lbody, lend);

            fprintf(out, "@L%d\n", lend);
            return 0;
        }
        case STMT_FOR: {
            int lcond = new_label();
            int lbody = new_label();
            int lpost = new_label();
            int lend = new_label();

            gen_stmt(lt, s->init);

            fprintf(out, "\tjmp @L%d\n", lcond);
            fprintf(out, "@L%d\n", lcond);
            char condbuf[32];
            char *cond = gen_expr(lt, s->expr, condbuf, sizeof(condbuf));
            strcat_tmp_free_all_except(cond);
            fprintf(out, "\tjnz %s, @L%d, @L%d\n", cond, lbody, lend);

            fprintf(out, "@L%d\n", lbody);
            int ok_for = loop_depth_push_ok();
            if (ok_for) {
                loop_continue_label[loop_depth] = lpost;
                loop_break_label[loop_depth] = lend;
                loop_is_for_in[loop_depth] = 0;
                loop_depth++;
            }
            int term = gen_block(lt, s->body);
            if (ok_for) loop_depth--;
            if (!term) fprintf(out, "\tjmp @L%d\n", lpost);

            fprintf(out, "@L%d\n", lpost);
            gen_stmt(lt, s->post);
            fprintf(out, "\tjmp @L%d\n", lcond);

            fprintf(out, "@L%d\n", lend);
            return 0;
        }
        case STMT_FOR_IN: {
            /* Desugars to: i = 0; while (i < len) { x = arr[i]; body; i++; } */
            char arrbuf[32];
            char *arr = gen_expr(lt, s->expr, arrbuf, sizeof(arrbuf));
            int arr_t = new_tmp();
            fprintf(out, "\t%%t%d =l copy %s\n", arr_t, arr);

            TypeKind container_type = static_type_of(lt, s->expr);

            /* M9: dynamic lists don't have a compile-time-known length —
               query it once via the runtime at loop entry. Fixed arrays
               keep the old compile-time-constant path. */
            char lenbuf[32];
            if (container_type == TYPE_INT_LIST) {
                int lt_tmp = new_tmp();
                fprintf(out, "\t%%t%d =l call $rapidc_list_len(l %%t%d)\n", lt_tmp, arr_t);
                snprintf(lenbuf, sizeof(lenbuf), "%%t%d", lt_tmp);
            } else if (container_type == TYPE_BYTE_LIST) {
                int lt_tmp = new_tmp();
                fprintf(out, "\t%%t%d =l call $rapidc_blist_len(l %%t%d)\n", lt_tmp, arr_t);
                snprintf(lenbuf, sizeof(lenbuf), "%%t%d", lt_tmp);
            } else {
                int len = 0;
                if (s->expr->kind == EXPR_IDENT) {
                    int idx = local_find(lt, s->expr->name);
                    if (idx != -1) len = lt->array_len[idx];
                } else if (s->expr->kind == EXPR_ARRAY_LITERAL) {
                    len = s->expr->elem_count;
                }
                snprintf(lenbuf, sizeof(lenbuf), "%d", len);
            }

            int idx_local = new_tmp(); /* hidden index counter, not a named local */
            fprintf(out, "\t%%hi%d =l copy 0\n", idx_local);

            int lcond = new_label();
            int lbody = new_label();
            int lend = new_label();

            fprintf(out, "\tjmp @L%d\n", lcond);
            fprintf(out, "@L%d\n", lcond);
            int cmpt = new_tmp();
            fprintf(out, "\t%%t%d =l csltl %%hi%d, %s\n", cmpt, idx_local, lenbuf);
            fprintf(out, "\tjnz %%t%d, @L%d, @L%d\n", cmpt, lbody, lend);

            fprintf(out, "@L%d\n", lbody);
            /* Scope: the loop variable is visible only inside this loop
               (semantic.c does the same with saved_count). Without popping
               it below, every for-in leaked one LocalTable slot for the rest
               of the function, and local_find() kept returning the stale
               earlier entry for a re-used name (wrong type). */
            int for_in_saved_count = lt->count;
            char idxvalbuf[32], addrbuf[32];
            snprintf(idxvalbuf, sizeof(idxvalbuf), "%%hi%d", idx_local);
            char arrvalbuf[32];
            snprintf(arrvalbuf, sizeof(arrvalbuf), "%%t%d", arr_t);
            char *addr = gen_scaled_idx_addr(lt, container_type, s->expr, arrvalbuf, idxvalbuf, addrbuf, sizeof(addrbuf));
            if (container_type == TYPE_BYTE_LIST || container_type == TYPE_STRING) {
                fprintf(out, "\t%%v_%s =l loadub %s\n", s->name, addr);
                local_add(lt, s->name, TYPE_BYTE, 0, TYPE_VOID, TYPE_VOID);
            } else if (container_type == TYPE_INT_LIST) {
                fprintf(out, "\t%%v_%s =l loadl %s\n", s->name, addr);
                local_add(lt, s->name, TYPE_INT, 0, TYPE_VOID, TYPE_VOID);
            } else if (container_type == TYPE_PTR) {
                /* M35: semantic.c's STMT_FOR_IN check now only accepts
                   list<int>/list<byte>, so TYPE_PTR (and the old TYPE_ARRAY)
                   are unreachable here post-semantic-check; branch kept,
                   now PTR-only, as a defensive fallback rather than deleted
                   outright, mirroring the same conservative treatment as
                   the other TYPE_ARRAY sites in this file. */
                TypeKind elem = static_elem_type_of(lt, s->expr);
                char cls = type_qbe_class(elem);
                const char *loadop;
                switch (elem) {
                    case TYPE_INT8:  loadop = "loadsb"; break;
                    case TYPE_UINT8: case TYPE_BYTE: case TYPE_BOOL: loadop = "loadub"; break;
                    case TYPE_INT16:  loadop = "loadsh"; break;
                    case TYPE_UINT16: loadop = "loaduh"; break;
                    case TYPE_INT32:  loadop = "loadsw"; break;
                    case TYPE_UINT32: loadop = "loaduw"; break;
                    default: loadop = "loadl"; break;
                }
                fprintf(out, "\t%%v_%s =%c %s %s\n", s->name, cls, loadop, addr);
                local_add(lt, s->name, elem, 0, TYPE_VOID, TYPE_VOID);
            } else {
                fprintf(out, "\t%%v_%s =l loadl %s\n", s->name, addr);
                local_add(lt, s->name, TYPE_INT, 0, TYPE_VOID, TYPE_VOID);
            }

            int ok_forin = loop_depth_push_ok();
            if (ok_forin) {
                loop_continue_label[loop_depth] = lcond;
                loop_break_label[loop_depth] = lend;
                loop_is_for_in[loop_depth] = 1;
                loop_for_in_idx_tmp[loop_depth] = idx_local;
                loop_depth++;
            }
            int term = gen_block(lt, s->body);
            if (ok_forin) loop_depth--;
            if (!term) {
                fprintf(out, "\t%%hi%d =l add %%hi%d, 1\n", idx_local, idx_local);
                fprintf(out, "\tjmp @L%d\n", lcond);
            }

            fprintf(out, "@L%d\n", lend);
            lt->count = for_in_saved_count;
            return 0;
        }
        case STMT_BREAK: {
            fprintf(out, "\tjmp @L%d\n", loop_break_label[loop_depth - 1]);
            return 1; /* rest of the current block is unreachable */
        }
        case STMT_CONTINUE: {
            int d = loop_depth - 1;
            if (loop_is_for_in[d]) {
                /* for-in: bump the hidden index before re-checking the condition */
                int idx_local = loop_for_in_idx_tmp[d];
                fprintf(out, "\t%%hi%d =l add %%hi%d, 1\n", idx_local, idx_local);
            }
            fprintf(out, "\tjmp @L%d\n", loop_continue_label[d]);
            return 1; /* rest of the current block is unreachable */
        }
        case STMT_SWITCH: {
            char subjbuf[32];
            char *subj = gen_expr(lt, s->expr, subjbuf, sizeof(subjbuf));
            int subj_t = new_tmp();
            fprintf(out, "\t%%t%d =l copy %s\n", subj_t, subj);
            strcat_tmp_free_all_except(subj);

            int n = 0;
            for (CaseClause *c = s->cases; c; c = c->next) n++;
            int *case_labels = malloc(sizeof(int) * (n + 1));
            int lend = new_label();
            int default_idx = -1;

            /* First pass: allocate a label per clause and find the chain of
               comparisons needed to dispatch (case values only). */
            CaseClause *c = s->cases;
            for (int i = 0; i < n; i++, c = c->next) {
                case_labels[i] = new_label();
                if (!c->value) default_idx = i;
            }

            /* Dispatch: chain of compare+jnz, in source order, falling to
               the next comparison on mismatch; default (if any) is the
               final fallback target, otherwise skip straight to end. */
            c = s->cases;
            for (int i = 0; i < n; i++, c = c->next) {
                if (!c->value) continue; /* default handled via fallback */
                char valbuf[32];
                char *val = gen_expr(lt, c->value, valbuf, sizeof(valbuf));
                int cmpt = new_tmp();
                fprintf(out, "\t%%t%d =l ceql %%t%d, %s\n", cmpt, subj_t, val);
                int lnext = new_label();
                fprintf(out, "\tjnz %%t%d, @L%d, @L%d\n", cmpt, case_labels[i], lnext);
                fprintf(out, "@L%d\n", lnext);
            }
            if (default_idx != -1) {
                fprintf(out, "\tjmp @L%d\n", case_labels[default_idx]);
            } else {
                fprintf(out, "\tjmp @L%d\n", lend);
            }

            /* Bodies, in source order, falling through to the next clause's
               body when a case has no `break` (C semantics). */
            c = s->cases;
            int ok_switch = loop_depth_push_ok();
            if (ok_switch) {
                loop_break_label[loop_depth] = lend; /* break exits the switch */
                loop_depth++;
            }
            for (int i = 0; i < n; i++, c = c->next) {
                fprintf(out, "@L%d\n", case_labels[i]);
                int term = gen_block(lt, c->body);
                if (!term) {
                    int target = (i + 1 < n) ? case_labels[i + 1] : lend;
                    fprintf(out, "\tjmp @L%d\n", target);
                }
            }
            if (ok_switch) loop_depth--;

            fprintf(out, "@L%d\n", lend);
            free(case_labels);
            return 0;
        }
    }
    return 0;
}

static void gen_function(Function *f) {
    LocalTable lt;
    lt.count = 0;

    /* M25: return/param QBE class must match the type (s/d for float/
       double, l for everything else — the historical default). */
    g_cur_ret_type = f->ret_type; /* M43 */
    char retcls = type_qbe_class(f->ret_type);
    const char *qret = (f->ret_type == TYPE_VOID) ? "" : (retcls == 's') ? "s " : (retcls == 'd') ? "d " : "l ";
    fprintf(out, "export function %s$f_%s(", qret, f->name);
    for (Param *p = f->params; p; p = p->next) {
        fprintf(out, "%c %%v_%s%s", type_qbe_class(p->type), p->name, p->next ? ", " : "");
    }
    fprintf(out, ") {\n@start\n");

    /* M36: struct haritasi artik fonksiyon-yerel. semantic.c her govdeyi
       kontrol ederken haritayi doldurup bir sonrakinde temizliyor, yani
       codegen calisirken harita SON fonksiyonun durumunu tasir. Burada bu
       fonksiyonun parametrelerini yeniden kaydediyoruz; yerel `var`'lar
       asagidaki ilk gecisten once yeniden kaydedilir (bkz. asagi). */
    var_struct_map_clear();
    for (Param *p = f->params; p; p = p->next) {
        local_add(&lt, p->name, p->type, 0, p->elem_type, p->elem_elem_type);
        if (p->struct_name) var_struct_map_set(p->name, p->struct_name);
    }

    /* First pass: collect local variable declarations and mark address-taken */
    for (Stmt *s = f->body; s; s = s->next) {
        if (s->kind == STMT_VAR_DECL || s->kind == STMT_CONST_DECL) {
            /* guard: initializer-less declarations (M8 structs) have no expr.
               M35: TYPE_ARRAY is gone (no grammar rule produces it as a
               declared var type anymore), so `len` (the old array_len
               local-table slot) is always 0 now — kept as a parameter to
               local_add() rather than restructuring that signature. */
            int len = 0;
            local_add(&lt, s->name, s->type, len, s->elem_type, s->elem_elem_type);
        }
    }
    mark_addr_taken(&lt, f->body);

    /* BUG-3 fix: pre-allocate stable stack slots (once) for every
       struct-typed var-decl anywhere in the function, including inside
       loop bodies — see hoist_struct_locals_stmt for why. */
    hoisted_struct_count = 0;
    hoist_struct_locals(f->body);

    /* Second pass: emit code with proper stack allocation for address-taken variables */
    int terminated = 0;
    for (Stmt *s = f->body; s; s = s->next) {
        if (gen_stmt(&lt, s)) {
            terminated = 1;
            break;
        }
    }
    /* A function whose body falls off the end (e.g. a void fn without a
       trailing `return`) still needs its last block to end in a ret —
       otherwise qbe rejects the IL ("last block misses jump"). */
    if (!terminated) {
        if (f->ret_type == TYPE_VOID) {
            fprintf(out, "\tret\n");
        } else if (f->ret_type == TYPE_FLOAT) {
            /* M25: QBE floating constants need the s_/d_ prefix even for a
               plain 0 — "ret 0" would be parsed as an integer literal in an
               `s`-typed function and rejected. */
            fprintf(out, "\tret s_0\n");
        } else if (f->ret_type == TYPE_DOUBLE) {
            fprintf(out, "\tret d_0\n");
        } else {
            fprintf(out, "\tret 0\n");
        }
    }
    fprintf(out, "}\n");
}

/* Resets all codegen global/static state to its initial (pre-main) values,
   so codegen_emit() can be called repeatedly from the same process (e.g. an
   embedding library, LSP, watch-mode or build-server) without state leaking
   or leaking memory between runs. Must run before any other codegen call. */
void codegen_reset(void) {
    /* g_strings[] entries are heap-owned (strdup'd in string_label()); free
       them before forgetting the count, or repeated runs leak one string
       table's worth of memory per call. */
    for (int i = 0; i < g_string_count; i++) {
        free(g_strings[i]);
        g_strings[i] = NULL;
    }

    out = NULL;
    tmp_counter = 0;
    label_counter = 0;
    had_fmt_int = 0;
    had_fmt_str = 0;
    had_fmt_byte = 0;
    had_fmt_double = 0;
    g_string_count = 0;
    g_func_count = 0;
    loop_depth = 0;
    g_codegen_error = 0;
    hoisted_struct_count = 0;
    g_cur_ret_type = TYPE_VOID;
    g_expr_depth = 0;
}

int codegen_emit(Program *prog, const char *out_path) {
    codegen_reset();
    out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "codegen: cannot open output file %s\n", out_path);
        return 0;
    }

    for (Function *f = prog->functions; f; f = f->next) {
        if (g_func_count >= MAX_FUNCS) {
            /* M36: semantic.c ayni siniri kontrol eder, ama iki sabit ayri tanimli;
               biri digerinden geride kalirsa tasma yerine temiz hata versin. */
            fprintf(stderr, "rapidc: too many functions (limit %d)\n", MAX_FUNCS);
            exit(1);
        }
        FuncInfo *fi = &g_funcs[g_func_count++];
        fi->name = f->name;
        fi->ret_type = f->ret_type;
        fi->ret_elem_type = f->ret_elem_type; /* M26 */
        fi->ret_elem_elem_type = f->ret_elem_elem_type; /* M32 */
        fi->param_count = f->param_count;
        fi->is_extern = f->is_extern;     /* M27 */
        fi->is_variadic = f->is_variadic; /* M27 */
    }

    for (Function *f = prog->functions; f; f = f->next) {
        /* M27: extern functions have no body — nothing to emit, they're
           resolved at link time against libc (or whatever the linker sees). */
        if (f->is_extern) continue;
        gen_function(f);
    }

    /* Real process entry point: every user function is emitted as $f_<name>
       (so user code can freely call a function named e.g. "main" as a value
       without colliding with C's `main`); this wrapper is the actual `main`
       the linker looks for, and just forwards to $f_main. */
    fprintf(out, "export function w $main() {\n@start\n\tcall $f_main()\n\tret 0\n}\n");

    if (had_fmt_int) {
        fprintf(out, "data $fmt_int = { b \"%%ld\\n\", b 0 }\n");
    }
    if (had_fmt_str) {
        fprintf(out, "data $fmt_str = { b \"%%s\\n\", b 0 }\n");
    }
    if (had_fmt_byte) {
        fprintf(out, "data $fmt_byte = { b \"%%c\\n\", b 0 }\n");
    }
    if (had_fmt_double) {
        /* M25: %g prints float/double without a fixed decimal-place count
           (trims trailing zeros), matching typical "print this number"
           expectations better than a fixed %f width would. */
        fprintf(out, "data $fmt_double = { b \"%%g\\n\", b 0 }\n");
    }
    /* M43: hata adi tablosu (rapidc_errname icin). Kod 1 = Unknown her zaman var. */
    {
        int n = error_count();
        for (int i = 1; i <= n; i++)
            fprintf(out, "data $rapid_en%d = { b \"%s\", b 0 }\n", i, error_name_of(i));
        fprintf(out, "export data $rapid_errcount = { l %d }\n", n);
        fprintf(out, "export data $rapid_errnames = { l 0");
        for (int i = 1; i <= n; i++) fprintf(out, ", l $rapid_en%d", i);
        fprintf(out, " }\n");
    }
    for (int i = 0; i < g_string_count; i++) {
        fprintf(out, "data $str%d = { b \"", i);
        for (const char *p = g_strings[i]; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', out);
            if (*p == '\n') { fprintf(out, "\\n"); continue; }
            fputc(*p, out);
        }
        fprintf(out, "\", b 0 }\n");
    }

    fclose(out);
    return g_codegen_error ? 0 : 1;
}
