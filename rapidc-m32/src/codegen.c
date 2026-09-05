#include "codegen.h"
#include "var_struct_map.h"
#include <string.h>

#define MAX_LOCALS 64
#define MAX_STRINGS 256
#define MAX_FUNCS 64

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

#define MAX_LOOP_DEPTH 32
static int loop_continue_label[MAX_LOOP_DEPTH]; /* label to jump to on `continue` */
static int loop_break_label[MAX_LOOP_DEPTH];    /* label to jump to on `break` (also used for switch) */
static int loop_is_for_in[MAX_LOOP_DEPTH];      /* 1 if this loop level is a for-in (continue must also bump the hidden index) */
static int loop_for_in_idx_tmp[MAX_LOOP_DEPTH]; /* hidden index temp number, for for-in levels */
static int loop_depth = 0;

static int new_tmp(void) {
    return tmp_counter++;
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

static void local_add(LocalTable *lt, char *name, TypeKind type, int array_len, TypeKind elem_type, TypeKind elem_elem_type) {
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
            if (e->lhs && e->lhs->kind == EXPR_IDENT) {
                StructDef *sd = struct_of_var(e->lhs->name);
                Field *fd = struct_field_find(sd, e->name);
                if (fd) return fd->elem_type;
            }
            return TYPE_INT;
        }
        case EXPR_BINOP:
            /* T* +/- int → T*, same elem_type as the pointer side. */
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
            if (e->lhs && e->lhs->kind == EXPR_IDENT) {
                StructDef *sd = struct_of_var(e->lhs->name);
                Field *fd = struct_field_find(sd, e->name);
                if (fd) return fd->elem_elem_type;
            }
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
        case EXPR_INT_LITERAL: return TYPE_INT;
        case EXPR_FLOAT_LITERAL: return TYPE_DOUBLE; /* M25: matches semantic.c's default */
        case EXPR_CHAR_LITERAL: return TYPE_BYTE;
        case EXPR_STRING_LITERAL: return TYPE_STRING;
        case EXPR_IDENT: {
            int idx = local_find(lt, e->name);
            return idx == -1 ? TYPE_INT : lt->types[idx];
        }
        case EXPR_IO_OUT_CALL: return TYPE_VOID;
        case EXPR_IO_IN_CALL: return TYPE_STRING;
        case EXPR_IO_FORMAT_CALL: return TYPE_VOID;
        case EXPR_IO_SYSCALL_CALL: return TYPE_INT;
        case EXPR_ARRAY_LITERAL: return TYPE_INT_ARRAY;
        case EXPR_INDEX: {
            TypeKind ct = static_type_of(lt, e->lhs);
            if (ct == TYPE_STRING || ct == TYPE_BYTE_LIST) return TYPE_BYTE;
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
            if (strcmp(e->name, "listCopy") == 0) {
                /* M20: result kind mirrors the argument (list<int> in ->
                   list<int> out, list<byte> in -> list<byte> out). */
                TypeKind arg_t = e->args ? static_type_of(lt, e->args->expr) : TYPE_INT_LIST;
                return (arg_t == TYPE_BYTE_LIST) ? TYPE_BYTE_LIST : TYPE_INT_LIST;
            }
            if (strcmp(e->name, "len") == 0) return TYPE_INT;
            if (strcmp(e->name, "push") == 0) return TYPE_VOID;
            if (strcmp(e->name, "alloc") == 0) return TYPE_PTR;
            if (strcmp(e->name, "free") == 0) return TYPE_VOID;
            if (strcmp(e->name, "syscall") == 0) return TYPE_INT;
            if (strcmp(e->name, "toInt") == 0) return TYPE_INT;
            if (strcmp(e->name, "toString") == 0) return TYPE_STRING;
            FuncInfo *fi = func_info_find(e->name);
            return fi ? fi->ret_type : TYPE_VOID;
        }
        case EXPR_FIELD: {
            /* M8: base must be a struct variable (guaranteed by semantic). */
            if (e->lhs && e->lhs->kind == EXPR_IDENT) {
                StructDef *sd = struct_of_var(e->lhs->name);
                Field *fd = struct_field_find(sd, e->name);
                if (fd) return fd->type;
            }
            return TYPE_INT;
        }
        case EXPR_ENUM_LITERAL: return TYPE_ENUM;
        case EXPR_STRUCT_LITERAL: return TYPE_STRUCT;
        case EXPR_NEG: return static_type_of(lt, e->arg); /* M29: same type as operand */
        case EXPR_FUNC_REF: return TYPE_FN; /* M11 */
        case EXPR_CALL_INDIRECT: return TYPE_INT; /* M11: unknown static return type */
        case EXPR_BINOP:
            switch (e->op) {
                case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
                    return TYPE_BOOL;
                case OP_ADD: case OP_SUB: {
                    TypeKind ltype = static_type_of(lt, e->lhs);
                    TypeKind rtype = static_type_of(lt, e->rhs);
                    /* M26: T* +/- T* → int (pointer difference);
                       T* +/- int → T* (generalized off TYPE_PTR). */
                    if (ltype == TYPE_PTR && rtype == TYPE_PTR) return TYPE_INT;
                    if (ltype == TYPE_PTR) return TYPE_PTR;
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

/* Emits the address of element `idx_val` of `base_val` (a QBE temp/value
   holding the container's base pointer) into a fresh temp, returned as a
   "%tN" string in buf. Shared by EXPR_INDEX, &arr[i], and indexed-assign. */
static char *gen_elem_addr(TypeKind container_type, const char *base_val, const char *idx_val, char *buf, size_t bufsz) {
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
        default:
            break;
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
static char *gen_expr(LocalTable *lt, Expr *e, char *buf, size_t bufsz) {
    switch (e->kind) {
        case EXPR_INT_LITERAL:
        case EXPR_CHAR_LITERAL: {
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
                had_fmt_str = 1;
                fprintf(out, "\tcall $printf(l $fmt_str, l %s, ...)\n", arg);
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
            /* Allocate a fresh stack block, store each element at base+8*i,
               return the (stable) base pointer as the array "value". */
            int n = e->elem_count;
            int base = new_tmp();
            fprintf(out, "\t%%t%d =l alloc8 %d\n", base, n > 0 ? n * 8 : 8);
            int i = 0;
            for (ExprList *l = e->elements; l; l = l->next, i++) {
                char elbuf[32];
                char *el = gen_expr(lt, l->expr, elbuf, sizeof(elbuf));
                if (i == 0) {
                    fprintf(out, "\tstorel %s, %%t%d\n", el, base);
                } else {
                    int addr = new_tmp();
                    fprintf(out, "\t%%t%d =l add %%t%d, %d\n", addr, base, i * 8);
                    fprintf(out, "\tstorel %s, %%t%d\n", el, addr);
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
                /* string[i]: pointer arithmetic by 1 byte, then load a single byte */
                int addr = new_tmp();
                fprintf(out, "\t%%t%d =l add %s, %s\n", addr, arr, idx);
                fprintf(out, "\t%%t%d =l loadub %%t%d\n", t, addr);
            } else if (container_type == TYPE_BYTE_LIST) {
                /* M20: list<byte>[i]: header + packed 1-byte elements */
                char addrbuf[32];
                char *addr = gen_elem_addr(container_type, arr, idx, addrbuf, sizeof(addrbuf));
                fprintf(out, "\t%%t%d =l loadub %s\n", t, addr);
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
                /* M15: address of array/list element is unchanged for byte* —
                   handled by the existing EXPR_INDEX branch below regardless
                   of pointee width. */
                /* Address of array/list element: &arr[i] */
                char arrbuf[32], idxbuf[32], addrbuf[32];
                char *arr = gen_expr(lt, e->arg->lhs, arrbuf, sizeof(arrbuf));
                char *idx = gen_expr(lt, e->arg->rhs, idxbuf, sizeof(idxbuf));
                TypeKind container_type = static_type_of(lt, e->arg->lhs);
                char *addr = gen_elem_addr(container_type, arr, idx, addrbuf, sizeof(addrbuf));
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
            if (strcmp(e->name, "syscall") == 0) {
                char argbufs[7][32];
                char *argvals[7];
                int n = 0;
                for (ExprList *l = e->args; l; l = l->next, n++) {
                    argvals[n] = gen_expr(lt, l->expr, argbufs[n], sizeof(argbufs[n]));
                }
                int t = new_tmp();
                fprintf(out, "\t%%t%d =l call $rapidc_syscall6(", t);
                for (int i = 0; i < 7; i++) {
                    if (i > 0) fprintf(out, ", ");
                    if (i < n) fprintf(out, "l %s", argvals[i]);
                    else fprintf(out, "l 0");
                }
                fprintf(out, ")\n");
                snprintf(buf, bufsz, "%%t%d", t);
                return buf;
            }
            /* M20: toInt(str)/toString(int) - thin wrappers over
               atol/sprintf in runtime.c (rapidc_str_to_int/rapidc_int_to_str),
               same call-a-runtime-helper pattern as the sysprog builtins
               above. */
            if (strcmp(e->name, "toInt") == 0) {
                char abuf[32];
                char *s = gen_expr(lt, e->args->expr, abuf, sizeof(abuf));
                int t = new_tmp();
                fprintf(out, "\t%%t%d =l call $rapidc_str_to_int(l %s)\n", t, s);
                snprintf(buf, bufsz, "%%t%d", t);
                return buf;
            }
            if (strcmp(e->name, "toString") == 0) {
                char abuf[32];
                char *n = gen_expr(lt, e->args->expr, abuf, sizeof(abuf));
                int t = new_tmp();
                fprintf(out, "\t%%t%d =l call $rapidc_int_to_str(l %s)\n", t, n);
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
            if (strcmp(e->name, "listCopy") == 0) {
                char lbuf[32];
                char *listval = gen_expr(lt, e->args->expr, lbuf, sizeof(lbuf));
                TypeKind list_type = static_type_of(lt, e->args->expr);
                int t = new_tmp();
                if (list_type == TYPE_BYTE_LIST) {
                    fprintf(out, "\t%%t%d =l call $rapidc_blist_copy(l %s)\n", t, listval);
                } else {
                    fprintf(out, "\t%%t%d =l call $rapidc_list_copy(l %s)\n", t, listval);
                }
                snprintf(buf, bufsz, "%%t%d", t);
                return buf;
            }
            if (strcmp(e->name, "len") == 0) {
                char lbuf[32];
                char *arg = gen_expr(lt, e->args->expr, lbuf, sizeof(lbuf));
                TypeKind arg_type = static_type_of(lt, e->args->expr);
                int t = new_tmp();
                if (arg_type == TYPE_INT_LIST) {
                    fprintf(out, "\t%%t%d =l call $rapidc_list_len(l %s)\n", t, arg);
                } else if (arg_type == TYPE_BYTE_LIST) {
                    fprintf(out, "\t%%t%d =l call $rapidc_blist_len(l %s)\n", t, arg);
                } else if (arg_type == TYPE_STRING) {
                    fprintf(out, "\t%%t%d =l call $rapidc_strlen(l %s)\n", t, arg);
                } else {
                    /* fixed array: length is compile-time-known */
                    int n = 0;
                    if (e->args->expr->kind == EXPR_IDENT) {
                        int idx = local_find(lt, e->args->expr->name);
                        if (idx != -1) n = lt->array_len[idx];
                    } else if (e->args->expr->kind == EXPR_ARRAY_LITERAL) {
                        n = e->args->expr->elem_count;
                    }
                    fprintf(out, "\t%%t%d =l copy %d\n", t, n);
                }
                snprintf(buf, bufsz, "%%t%d", t);
                return buf;
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
            int n = 0;
            for (ExprList *l = e->args; l; l = l->next, n++) {
                argvals[n] = gen_expr(lt, l->expr, argbufs[n], sizeof(argbufs[n]));
            }
            int t = new_tmp();
            fprintf(out, "\t%%t%d =l call %s(", t, callee);
            for (int i = 0; i < n; i++) {
                fprintf(out, "l %s%s", argvals[i], i + 1 < n ? ", " : "");
            }
            fprintf(out, ")\n");
            snprintf(buf, bufsz, "%%t%d", t);
            return buf;
        }
        case EXPR_FIELD: {
            /* M8: `p.f` — struct var's %v_<name> holds the stack base
               pointer; load the field from base + offset. */
            char bbuf[32];
            char *base = gen_expr(lt, e->lhs, bbuf, sizeof(bbuf));
            StructDef *sd = (e->lhs->kind == EXPR_IDENT) ? struct_of_var(e->lhs->name) : NULL;
            Field *fd = struct_field_find(sd, e->name);
            int addr = new_tmp();
            fprintf(out, "\t%%t%d =l add %s, %d\n", addr, base, fd ? fd->offset : 0);
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
            StructDef *sd = e->name ? struct_find(e->name) : NULL;
            int base = new_tmp();
            fprintf(out, "\t%%t%d =l alloc8 %d\n", base, sd && sd->size > 0 ? sd->size : 8);
            char basebuf[32];
            snprintf(basebuf, sizeof(basebuf), "%%t%d", base);
            emit_struct_zero(sd, basebuf);
            for (StructLiteralField *fl = e->struct_fields; fl; fl = fl->next) {
                Field *fd = struct_field_find(sd, fl->field);
                char vbuf[32];
                char *v = gen_expr(lt, fl->value, vbuf, sizeof(vbuf));
                emit_store_at(basebuf, fd ? fd->offset : 0, v);
            }
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
                }
                snprintf(buf, bufsz, "%%t%d", t);
                return buf;
            }
            switch (e->op) {
                case OP_EQ: fprintf(out, "\t%%t%d =l ceql %s, %s\n", t, l, r); break;
                case OP_NE: fprintf(out, "\t%%t%d =l cnel %s, %s\n", t, l, r); break;
                case OP_LT: fprintf(out, "\t%%t%d =l csltl %s, %s\n", t, l, r); break;
                case OP_LE: fprintf(out, "\t%%t%d =l cslel %s, %s\n", t, l, r); break;
                case OP_GT: fprintf(out, "\t%%t%d =l csgtl %s, %s\n", t, l, r); break;
                case OP_GE: fprintf(out, "\t%%t%d =l csgel %s, %s\n", t, l, r); break;
                case OP_ADD: {
                    if (ltype == TYPE_STRING) {
                        /* string + string = concatenation */
                        fprintf(out, "\t%%t%d =l call $rapidc_strcat(l %s, l %s)\n", t, l, r);
                    } else {
                        /* int+int, int*+int (byte offset), byte+int — all plain add */
                        fprintf(out, "\t%%t%d =l add %s, %s\n", t, l, r);
                    }
                    break;
                }
                case OP_SUB: fprintf(out, "\t%%t%d =l sub %s, %s\n", t, l, r); break;
                case OP_MUL: fprintf(out, "\t%%t%d =l mul %s, %s\n", t, l, r); break;
                case OP_DIV: fprintf(out, "\t%%t%d =l div %s, %s\n", t, l, r); break;
            }
            snprintf(buf, bufsz, "%%t%d", t);
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
    switch (s->kind) {
        case STMT_VAR_DECL:
        case STMT_CONST_DECL: {
            /* M8: struct-typed variables live on the stack; %v_<name> holds
               the base pointer (their "value"), fields are zero-initialized
               and then filled from the struct literal if one is given. */
            if (s->type == TYPE_STRUCT) {
                if (s->expr) {
                    char argbuf[32];
                    char *val = gen_expr(lt, s->expr, argbuf, sizeof(argbuf));
                    fprintf(out, "\t%%v_%s =l copy %s\n", s->name, val);
                } else {
                    StructDef *sd = s->struct_name ? struct_find(s->struct_name) : struct_of_var(s->name);
                    int base = new_tmp();
                    fprintf(out, "\t%%t%d =l alloc8 %d\n", base, sd && sd->size > 0 ? sd->size : 8);
                    char basebuf[32];
                    snprintf(basebuf, sizeof(basebuf), "%%t%d", base);
                    emit_struct_zero(sd, basebuf);
                    fprintf(out, "\t%%v_%s =l copy %%t%d\n", s->name, base);
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
            return 0;
        }
        case STMT_RETURN: {
            if (s->expr) {
                char argbuf[32];
                char *val = gen_expr(lt, s->expr, argbuf, sizeof(argbuf));
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
            return 0;
        }
        case STMT_IF: {
            char condbuf[32];
            char *cond = gen_expr(lt, s->expr, condbuf, sizeof(condbuf));

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
            char valbuf[32];
            char *val = gen_expr(lt, s->expr, valbuf, sizeof(valbuf));
            int idx = local_find(lt, s->name);
            TypeKind vt = (idx != -1) ? lt->types[idx] : TYPE_INT;
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
            char arrbuf[32], idxbuf[32], valbuf[32], addrbuf[32];
            char *arr = gen_expr(lt, s->target, arrbuf, sizeof(arrbuf));
            char *idx = gen_expr(lt, s->index, idxbuf, sizeof(idxbuf));
            char *val = gen_expr(lt, s->value, valbuf, sizeof(valbuf));
            TypeKind container_type = static_type_of(lt, s->target);
            if (container_type == TYPE_STRING) {
                /* M16: s[i] = b — same 1-byte pointer arithmetic as the
                   EXPR_INDEX read path, but storeb instead of loadub. */
                int addr = new_tmp();
                fprintf(out, "\t%%t%d =l add %s, %s\n", addr, arr, idx);
                fprintf(out, "\tstoreb %s, %%t%d\n", val, addr);
                return 0;
            }
            if (container_type == TYPE_BYTE_LIST) {
                /* M20: bs[i] = b — header + packed 1-byte elements */
                char *addr = gen_elem_addr(container_type, arr, idx, addrbuf, sizeof(addrbuf));
                fprintf(out, "\tstoreb %s, %s\n", val, addr);
                return 0;
            }
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
            return 0;
        }
        case STMT_FIELD_ASSIGN: {
            /* M8: `p.f = expr;` — store through the struct's base pointer
               at the field's offset. */
            if (s->target->kind != EXPR_IDENT) return 0;
            char bbuf[32], valbuf[32];
            char *val = gen_expr(lt, s->value, valbuf, sizeof(valbuf));
            char *base = gen_expr(lt, s->target, bbuf, sizeof(bbuf));
            StructDef *sd = struct_of_var(s->target->name);
            Field *fd = struct_field_find(sd, s->field);
            emit_store_at(base, fd ? fd->offset : 0, val);
            return 0;
        }
        case STMT_INC: {
            int idx = local_find(lt, s->name);
            if (idx != -1 && lt->addr_taken[idx]) {
                /* Address-taken variable: load, add, store */
                int t = new_tmp();
                fprintf(out, "\t%%t%d =l loadl %%v_%s\n", t, s->name);
                fprintf(out, "\t%%t%d =l add %%t%d, 1\n", t, t);
                fprintf(out, "\tstorel %%t%d, %%v_%s\n", t, s->name);
            } else {
                fprintf(out, "\t%%v_%s =l add %%v_%s, 1\n", s->name, s->name);
            }
            return 0;
        }
        case STMT_DEC: {
            int idx = local_find(lt, s->name);
            if (idx != -1 && lt->addr_taken[idx]) {
                int t = new_tmp();
                fprintf(out, "\t%%t%d =l loadl %%v_%s\n", t, s->name);
                fprintf(out, "\t%%t%d =l sub %%t%d, 1\n", t, t);
                fprintf(out, "\tstorel %%t%d, %%v_%s\n", t, s->name);
            } else {
                fprintf(out, "\t%%v_%s =l sub %%v_%s, 1\n", s->name, s->name);
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
            fprintf(out, "\tjnz %s, @L%d, @L%d\n", cond, lbody, lend);

            fprintf(out, "@L%d\n", lbody);
            loop_continue_label[loop_depth] = lcond;
            loop_break_label[loop_depth] = lend;
            loop_is_for_in[loop_depth] = 0;
            loop_depth++;
            int term = gen_block(lt, s->body);
            loop_depth--;
            if (!term) fprintf(out, "\tjmp @L%d\n", lcond);

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
            fprintf(out, "\tjnz %s, @L%d, @L%d\n", cond, lbody, lend);

            fprintf(out, "@L%d\n", lbody);
            loop_continue_label[loop_depth] = lpost;
            loop_break_label[loop_depth] = lend;
            loop_is_for_in[loop_depth] = 0;
            loop_depth++;
            int term = gen_block(lt, s->body);
            loop_depth--;
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
            char idxvalbuf[32], addrbuf[32];
            snprintf(idxvalbuf, sizeof(idxvalbuf), "%%hi%d", idx_local);
            char arrvalbuf[32];
            snprintf(arrvalbuf, sizeof(arrvalbuf), "%%t%d", arr_t);
            char *addr = gen_elem_addr(container_type, arrvalbuf, idxvalbuf, addrbuf, sizeof(addrbuf));
            if (container_type == TYPE_BYTE_LIST) {
                fprintf(out, "\t%%v_%s =l loadub %s\n", s->name, addr);
                local_add(lt, s->name, TYPE_BYTE, 0, TYPE_VOID, TYPE_VOID);
            } else {
                fprintf(out, "\t%%v_%s =l loadl %s\n", s->name, addr);
                local_add(lt, s->name, TYPE_INT, 0, TYPE_VOID, TYPE_VOID);
            }

            loop_continue_label[loop_depth] = lcond;
            loop_break_label[loop_depth] = lend;
            loop_is_for_in[loop_depth] = 1;
            loop_for_in_idx_tmp[loop_depth] = idx_local;
            loop_depth++;
            int term = gen_block(lt, s->body);
            loop_depth--;
            if (!term) {
                fprintf(out, "\t%%hi%d =l add %%hi%d, 1\n", idx_local, idx_local);
                fprintf(out, "\tjmp @L%d\n", lcond);
            }

            fprintf(out, "@L%d\n", lend);
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
            loop_break_label[loop_depth] = lend; /* break exits the switch */
            loop_depth++;
            for (int i = 0; i < n; i++, c = c->next) {
                fprintf(out, "@L%d\n", case_labels[i]);
                int term = gen_block(lt, c->body);
                if (!term) {
                    int target = (i + 1 < n) ? case_labels[i + 1] : lend;
                    fprintf(out, "\tjmp @L%d\n", target);
                }
            }
            loop_depth--;

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
    char retcls = type_qbe_class(f->ret_type);
    const char *qret = (f->ret_type == TYPE_VOID) ? "" : (retcls == 's') ? "s " : (retcls == 'd') ? "d " : "l ";
    fprintf(out, "export function %s$f_%s(", qret, f->name);
    for (Param *p = f->params; p; p = p->next) {
        fprintf(out, "%c %%v_%s%s", type_qbe_class(p->type), p->name, p->next ? ", " : "");
    }
    fprintf(out, ") {\n@start\n");

    for (Param *p = f->params; p; p = p->next) {
        local_add(&lt, p->name, p->type, 0, p->elem_type, p->elem_elem_type);
    }

    /* First pass: collect local variable declarations and mark address-taken */
    for (Stmt *s = f->body; s; s = s->next) {
        if (s->kind == STMT_VAR_DECL || s->kind == STMT_CONST_DECL) {
            /* guard: initializer-less declarations (M8 structs) have no expr */
            int len = (s->type == TYPE_INT_ARRAY && s->expr) ? s->expr->elem_count : 0;
            local_add(&lt, s->name, s->type, len, s->elem_type, s->elem_elem_type);
        }
    }
    mark_addr_taken(&lt, f->body);

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

int codegen_emit(Program *prog, const char *out_path) {
    out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "codegen: cannot open output file %s\n", out_path);
        return 0;
    }
    tmp_counter = 0;
    label_counter = 0;
    had_fmt_int = 0;
    had_fmt_str = 0;
    had_fmt_byte = 0;
    had_fmt_double = 0;
    g_string_count = 0;
    g_func_count = 0;
    loop_depth = 0;

    for (Function *f = prog->functions; f; f = f->next) {
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
    return 1;
}
