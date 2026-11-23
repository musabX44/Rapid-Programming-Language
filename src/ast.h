#ifndef AST_H
#define AST_H

#include <stdio.h>
#include <stdlib.h>

typedef struct CaseClause CaseClause;
typedef struct FnType FnType;
typedef struct StructDef StructDef;
typedef struct EnumDef EnumDef;
typedef struct StructLiteralField StructLiteralField;

typedef enum {
    TYPE_VOID,
    TYPE_INT,
    TYPE_BOOL,
    TYPE_STRING,   /* M5 */
     /* M35-arraylib: TYPE_INT_ARRAY and the generalized fixed-size TYPE_ARRAY
        (`T[]` syntax, `[1,2,3]` literals, `xs[i]` on a fixed-size array) have
        been removed from the core language. Fixed-size arrays are no longer
        a builtin type — see std/array.rapid for the replacement, a
        struct-based dynamic array (Array) built entirely in RapidC on top of
        alloc()/pointers, with a real length field (fixing the M33 gap: a
        fixed-size array parameter used to carry no length at all). The two
        enum values are kept as reserved/unused placeholders rather than
        renumbering everything after them; nothing in the compiler produces
        or accepts them anymore. `list<int>`/`list<byte>` (TYPE_INT_LIST/
        TYPE_BYTE_LIST below) are a separate, still-fully-supported core
        feature and are untouched by this change. */
     TYPE_INT_ARRAY_REMOVED_RESERVED,
     TYPE_ARRAY_REMOVED_RESERVED,
    TYPE_INT_LIST,  /* M9: dynamic, growable int[] (push/listCopy/len) */
    TYPE_BYTE_LIST, /* M20: dynamic, growable byte[] (push/listCopy/len) */
    TYPE_STRUCT,    /* M8: user-defined struct */
    TYPE_ENUM,      /* M8: user-defined enum (treated as int) */
    TYPE_FN,        /* M11: first-class function value (no closures) */
    TYPE_BYTE,      /* M13: 8-bit unsigned integer (also used as char) */
    /* M23: fixed-width integers. TYPE_INT stays as the original 64-bit
       general-purpose type (kept for source compatibility); these are the
       explicit-width additions. All are still represented in a single QBE
       `l` (64-bit) register/stack slot like TYPE_INT/TYPE_BYTE always were
       — only the *load/store width* differs (loadsb/loadsh/loadsw/loadl for
       signed, loadub/loaduh/loaduw/loadl for unsigned), so no new register
       class is introduced here (that's float/double territory, M25). */
    TYPE_INT8,  TYPE_INT16,  TYPE_INT32,  TYPE_INT64,
    TYPE_UINT8, TYPE_UINT16, TYPE_UINT32, TYPE_UINT64,
    /* M25: floating point. Unlike the integer family above, these are NOT
       represented in a QBE `l` slot — they get their own QBE register class
       (`s` for TYPE_FLOAT/32-bit, `d` for TYPE_DOUBLE/64-bit) end to end:
       separate literal syntax (a decimal point), separate arithmetic
       instructions (add/sub/mul/div get an `s`/`d` suffix instead of `l`),
       separate compare instructions (ceqs/ceqd, ...), separate load/store
       (loads/stores, loadd/stored), and separate ABI class in call/param/ret
       position. codegen.c's static_type_of / gen_expr call sites that used
       to hardcode "l" now branch on type_is_float() to pick the right QBE
       class letter. */
    TYPE_FLOAT, TYPE_DOUBLE,
    /* M26: single generic pointer type, replacing the old TYPE_INT_PTR/
       TYPE_BYTE_PTR pair. TypeKind alone can no longer tell you what a
       pointer points to — every scalar site that stores a TypeKind (Stmt,
       Field, Param, Function.ret_type, FnSig, Expr) grows a matching
       `elem_type` field alongside it, valid only when `type`/`ret_type`/
       etc. == TYPE_PTR. This mirrors the existing struct_names[]-next-to-
       types[] side-channel pattern already used in semantic.c/codegen.c
       for struct type names.

       M32: TYPE_PTR is now also a valid pointee, enabling T** (pointer to
       pointer). Since `elem_type` is itself just a TypeKind, a value of
       TYPE_PTR there doesn't say what *that* pointer points to — so every
       `elem_type` site above grows one more sibling field, `elem_elem_type`,
       valid only when `elem_type == TYPE_PTR`, holding the pointee of the
       pointee (e.g. for `int**`, type=TYPE_PTR, elem_type=TYPE_PTR,
       elem_elem_type=TYPE_INT). This deliberately stops at exactly one more
       level (T**, not T***...) — the same one-level-at-a-time scoping this
       side-channel pattern already used going from flat types to T*. */
    TYPE_PTR
} TypeKind;

/* M26/M32: true for every TypeKind that may be pointed to (T* is legal for
   T). Excludes TYPE_VOID (no void*) and the composite/no-fixed-value-slot
   kinds that were never valid pointees (arrays, lists, fn, enum — enum is
   TYPE_ENUM but always treated as plain int elsewhere; not extended here
   since it wasn't asked for). M32: TYPE_PTR is now included, so T** is
   legal — pointer-to-pointer is exactly one level deep (see elem_elem_type
   above); pointer-to-pointer-to-pointer is still out of scope. */
int type_ptr_ok(TypeKind t);

/* M25: true for TYPE_FLOAT/TYPE_DOUBLE. */
int type_is_float(TypeKind t);
/* M25: QBE class letter for a value of this type when it lives in a
   register/temp: 'l' for everything integer/pointer-like (the historical
   default), 's' for TYPE_FLOAT, 'd' for TYPE_DOUBLE. */
char type_qbe_class(TypeKind t);

/* M23: true for every integer TypeKind (the original TYPE_INT/TYPE_BYTE
   plus the new fixed-width family). Centralizes what used to be scattered
   `lt == TYPE_BYTE || lt == TYPE_INT`-style pairwise checks throughout
   semantic.c — every new width added here automatically participates in
   arithmetic/comparison/assignment compatibility instead of requiring a
   growing set of hand-written OR chains. */
int type_is_integer(TypeKind t);
/* M23: bit width of an integer TypeKind (8/16/32/64). TYPE_INT/TYPE_BYTE
   keep their historical widths (64 and 8) for backward compatibility. */
int type_int_width(TypeKind t);
/* M23: true if the integer TypeKind is unsigned (TYPE_BYTE and the
   TYPE_UINT* family; TYPE_INT and TYPE_INT* are signed). */
int type_int_is_unsigned(TypeKind t);
/* M23: usual-arithmetic-conversion result type for two integer operands —
   the wider one wins; on equal width, unsigned wins (matches C's usual
   arithmetic conversions closely enough for this language's needs). Callers
   must check type_is_integer() on both operands first. */
TypeKind type_int_promote(TypeKind a, TypeKind b);
/* M23: human-readable name for error messages ("int32", "uint8", ...). */
const char *type_kind_name(TypeKind t);

typedef enum {
    EXPR_INT_LITERAL,
    EXPR_FLOAT_LITERAL,  /* M25: 3.14 → float (widened to double by context; see semantic.c) */
    EXPR_STRING_LITERAL, /* M5 */
    EXPR_CHAR_LITERAL,   /* M13: 'a' → byte */
    EXPR_NULL_LITERAL,   /* M33: `null` — untyped pointer literal, TYPE_PTR
                            with elem_type matching whatever it's compared
                            against/assigned to */
    EXPR_BOOL_LITERAL,   /* `true` / `false` — TYPE_BOOL, value 1/0 in `num` */
    EXPR_IDENT,
    EXPR_IO_OUT_CALL,
    EXPR_IO_IN_CALL,     /* M16: io::in() - read a line from stdin */
    EXPR_IO_FORMAT_CALL, /* M16: io::format(fmt, args...) - "{}"-templated print */
    EXPR_IO_SYSCALL_CALL, /* M18: io::open/read/write/close(...) - raw file syscall wrappers */
    EXPR_BINOP,
    EXPR_ARRAY_LITERAL,  /* M5: [1, 2, 3] */
    EXPR_INDEX,          /* M5: xs[i] */
    EXPR_CALL,           /* M6: f(a, b, ...) */
    EXPR_ADDR_OF,        /* M7: &x */
    EXPR_DEREF,          /* M7: *p */
    EXPR_FIELD,          /* M8: struct field access */
    EXPR_ENUM_LITERAL,   /* M8: enum member */
    EXPR_STRUCT_LITERAL, /* M8: struct literal */
    EXPR_FUNC_REF,       /* M11: bare function name used as a value (not called) */
    EXPR_CALL_INDIRECT,  /* M11: expr(args) where expr is a fn-typed value */
    EXPR_NEG,            /* M29: unary minus (-expr), e.g. -5, -5.5, -x.
                             Needed as its own node (rather than desugaring
                             to `0 - expr`) because the zero literal in that
                             desugar is always int-typed, which fails
                             semantic.c's binop check for a float operand
                             (types_float_compatible requires BOTH sides to
                             already be float-typed; it does not mix
                             int-with-float the way OP_ADD/OP_SUB do for two
                             already-float operands). A dedicated node lets
                             semantic.c type it as exactly its operand's
                             type (int stays int, float/double stays
                             float/double), matching normal unary-minus
                             semantics in any C-like language. */
    EXPR_NOT,            /* logical negation (!expr), bool -> bool only */
    EXPR_TRY,            /* M43: try <cagri>  -- hata varsa yukari yay */
    EXPR_CATCH,          /* M43: <cagri> catch <varsayilan> -- hata varsa varsayilani kullan */
    EXPR_ERROR_LIT,      /* M43: error::Ad -- hata kodu (int, >=1) */
    EXPR_LOGAND,         /* expr && expr, short-circuit */
    EXPR_LOGOR           /* expr || expr, short-circuit */
} ExprKind;

typedef enum {
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, /* M5: needed for string concat (+) and array indices/arith */
    OP_MOD,   /* % : integer-only remainder */
    OP_BAND,  /* &  : bitwise and (integer-only) */
    OP_BOR,   /* |  : bitwise or (integer-only) */
    OP_BXOR,  /* ^  : bitwise xor (integer-only) */
    OP_SHL,   /* << : left shift (integer-only) */
    OP_SHR,   /* >> : right shift (integer-only) */
    OP_POW    /* ** : power, integer or float */
} BinOpKind;

typedef struct ExprList {
    struct Expr *expr;
    struct ExprList *next;
} ExprList;

typedef struct Expr {
    ExprKind kind;
    int handled;          /* M43: hata donebilen cagri try/catch ile sarilmis mi (semantic doldurur) */
    int line;             /* M42: kaynak satiri (dosya-yerel), 0 = bilinmiyor */
    const char *file;     /* M42: kaynak dosya yolu (internlenmis) */
    FnType *fn_sig;       /* M40: EXPR_CALL_INDIRECT: cagrilan fn degerinin imzasi (biliniyorsa) */
    long num;             /* EXPR_INT_LITERAL */
    double fnum;           /* M25: EXPR_FLOAT_LITERAL */
    char *str;             /* EXPR_STRING_LITERAL (raw, unescaped-decoded text) */
    char *name;             /* EXPR_IDENT / EXPR_CALL (callee name) / EXPR_FIELD (field name)
                               / EXPR_STRUCT_LITERAL (struct name) */
    struct Expr *arg;       /* EXPR_IO_OUT_CALL / EXPR_ADDR_OF / EXPR_DEREF argument */
    /* EXPR_IO_FORMAT_CALL: format-string literal expr in `lhs`, argument list in `args`. */
    /* EXPR_IO_SYSCALL_CALL: "open"/"read"/"write"/"close" in `name`, argument list in `args`. */
    BinOpKind op;            /* EXPR_BINOP */
    struct Expr *lhs;        /* EXPR_BINOP / EXPR_INDEX (array expr) / EXPR_FIELD (base expr) */
    struct Expr *rhs;        /* EXPR_BINOP / EXPR_INDEX (index expr) */
    ExprList *elements;      /* EXPR_ARRAY_LITERAL */
    int elem_count;          /* EXPR_ARRAY_LITERAL: number of elements */
    ExprList *args;          /* EXPR_CALL: argument list */
    StructLiteralField *struct_fields; /* EXPR_STRUCT_LITERAL */
    char *enum_name;         /* EXPR_ENUM_LITERAL */
    char *member_name;       /* EXPR_ENUM_LITERAL */
    TypeKind elem_type;      /* M26: EXPR_ADDR_OF / EXPR_DEREF / EXPR_CALL
                                 ("alloc") — pointee type when this expr's
                                 static type is TYPE_PTR. Filled in by
                                 semantic.c's check_expr as it resolves each
                                 node; codegen.c's static_type_of recomputes
                                 the same thing independently (it doesn't
                                 reuse semantic's annotations), so this field
                                 is authoritative only during the semantic
                                 pass, not relied on by codegen. */
    TypeKind elem_elem_type; /* M32: pointee of the pointee, valid only when
                                 elem_type == TYPE_PTR (i.e. this expr is
                                 T**). Same fill/consume convention as
                                 elem_type above. */
    /* EXPR_FUNC_REF: function name in `name`.
       EXPR_CALL_INDIRECT: callee value expr in `lhs`, argument list in `args`. */
} Expr;

typedef enum {
    STMT_VAR_DECL,
    STMT_CONST_DECL,
    STMT_RETURN,
    STMT_EXPR,
    STMT_IF,
    STMT_ASSIGN,
    STMT_INC,
    STMT_DEC,
    STMT_WHILE,
    STMT_DO_WHILE, /* M33: do { ... } while (cond); */
    STMT_FOR,
    STMT_FOR_IN,   /* M5: for (x in xs) { ... } */
    STMT_FAIL,           /* M43: fail error::Ad;  (veya fail <int ifade>;) */
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_SWITCH,   /* M5 */
    STMT_INDEX_ASSIGN, /* M5: xs[i] = expr; */
    STMT_DEREF_ASSIGN, /* M7: *p = expr; */
    STMT_FIELD_ASSIGN  /* M8: p.f = expr; */
} StmtKind;

/* Stmt definition */
typedef struct Stmt {
    StmtKind kind;
    int line;             /* M42 */
    const char *file;     /* M42 */
    char *name;         /* VAR_DECL / CONST_DECL / ASSIGN / INC / DEC (target variable name) / FOR_IN (loop var) */
    TypeKind type;       /* VAR_DECL / CONST_DECL */
    TypeKind elem_type;   /* M26: VAR_DECL / CONST_DECL — pointee type when type == TYPE_PTR */
    TypeKind elem_elem_type; /* M32: pointee of the pointee, valid only when elem_type == TYPE_PTR */
    char *struct_name;   /* VAR_DECL / CONST_DECL: source type name when type is TYPE_STRUCT/TYPE_ENUM */
    FnType *fn_type;     /* M40: VAR_DECL / CONST_DECL: imza, type == TYPE_FN ise */
    Expr *expr;          /* init expr / return expr / expr stmt / if,while,for,switch condition / assign value */
    struct Stmt *next;
    struct Stmt *then_body; /* STMT_IF */
    struct Stmt *else_body; /* STMT_IF: NULL, or a single STMT_IF (else if), or a block (else) */
    struct Stmt *body;      /* STMT_WHILE / STMT_DO_WHILE / STMT_FOR / STMT_FOR_IN */
    struct Stmt *init;      /* STMT_FOR: init statement */
    struct Stmt *post;      /* STMT_FOR: post statement */
    CaseClause *cases;      /* STMT_SWITCH */
    Expr *target;           /* STMT_INDEX_ASSIGN / STMT_DEREF_ASSIGN / STMT_FIELD_ASSIGN (base expr) */
    Expr *index;            /* STMT_INDEX_ASSIGN */
    Expr *value;            /* STMT_INDEX_ASSIGN / STMT_DEREF_ASSIGN / STMT_FIELD_ASSIGN */
    char *field;            /* STMT_FIELD_ASSIGN: field name */
} Stmt;

/* M5: one `case` (or the trailing `default`) inside a switch */
typedef struct CaseClause {
    Expr *value;              /* NULL means `default` */
    struct Stmt *body;        /* statement list for this case (may be NULL = falls through) */
    struct CaseClause *next;
} CaseClause;

typedef struct Field {
    char *name;
    TypeKind type;
    TypeKind elem_type; /* M26: pointee type when type == TYPE_PTR */
    TypeKind elem_elem_type; /* M32: pointee of the pointee, valid only when elem_type == TYPE_PTR */
    int offset; /* byte offset within struct */
    /* BUG-02/BUG-03 fix: name of the struct this field is an instance of,
       valid only when type == TYPE_STRUCT (NULL otherwise). Needed so a
       chained field access (`o.inner.a`) can resolve what struct `o.inner`
       itself is an instance of, in order to then look up field `a` on it —
       mirrors how a struct-typed *variable*'s struct name is recorded via
       var_struct_map, but for a struct-typed *field* instead. */
    char *struct_name;
    struct Field *next;
} Field;

typedef struct StructDef {
    char *name;
    Field *fields;
    int size; /* total size in bytes */
    struct StructDef *next;
} StructDef;

typedef struct EnumMember {
    char *name;
    int value;
    struct EnumMember *next;
} EnumMember;

typedef struct EnumDef {
    char *name;
    EnumMember *members;
    struct EnumDef *next;
} EnumDef;

/* Struct literal field initializer */
typedef struct StructLiteralField {
    char *field;
    Expr *value;
    struct StructLiteralField *next;
} StructLiteralField;

/* M40: fn(T1, T2): R tipinin imzasi. Onceden TYPE_FN duz bir etiketti ve
   imza hic tutulmuyordu. En fazla 8 parametre saklanir; fazlasi varsa
   `unchecked` isaretlenir (kontrol atlanir). Pointer'larda yalniz ilk
   seviye (elem) ve struct/enum adi tutulur. */
struct FnType {
    int param_count;
    int unchecked;
    TypeKind pt[8];
    TypeKind pe[8];
    const char *pn[8];
    TypeKind ret;
    TypeKind ret_elem;
    const char *ret_name;
};

/* M6: a function parameter */
typedef struct Param {
    char *name;
    TypeKind type;
    TypeKind elem_type; /* M26: pointee type when type == TYPE_PTR */
    TypeKind elem_elem_type; /* M32: pointee of the pointee, valid only when elem_type == TYPE_PTR */
    FnType *fn_type;    /* M40: type == TYPE_FN ise imza (yoksa NULL) */
    char *struct_name;  /* M36: struct adi, type == TYPE_STRUCT ise (yoksa NULL). Onceden Param'da
                           yoktu; bu yuzden struct-tipli bir PARAMETRENIN struct'i cozulemiyordu. */
    struct Param *next;
} Param;

typedef struct Function {
    int can_fail;         /* M43: `: !T` ile bildirildi -- hata donebilir */
    int line;             /* M42 */
    const char *file;     /* M42 */
    char *name;
    TypeKind ret_type;
    TypeKind ret_elem_type; /* M26: pointee type when ret_type == TYPE_PTR */
    TypeKind ret_elem_elem_type; /* M32: pointee of the pointee, valid only when ret_elem_type == TYPE_PTR */
    char *ret_struct_name; /* M40: struct/enum adi, ret_type == TYPE_STRUCT/TYPE_ENUM ise */
    Param *params;       /* M6 */
    int param_count;      /* M6 */
    Stmt *body;
    struct Function *next; /* M6: functions are chained; program can have many */
    int is_efn;            /* M10: declared with `efn name(...) => expr` sugar.
                               ret_type is a placeholder (TYPE_VOID) until the
                               semantic pass infers it from the body expression. */
    /* M27: `extern fn name(params[, ...]): ret;` — a foreign (typically C/
       libc) function with no body. body is always NULL for these; codegen
       skips emitting a definition and calls the bare symbol name (no `$f_`
       mangling, since the symbol lives outside rapidc-generated code). */
    int is_extern;
    int is_variadic;   /* M27: true if the declaration ended in `, ...`
                           (e.g. printf) — allows extra call-site arguments
                           beyond param_count, each still type-checked but
                           not matched against a declared parameter. */
} Function;

/* M28: `link "name";` top-level directive — requests that the final `cc`
   link step add a `-l<name>` flag (e.g. `link "m";` -> `-lm`), so an
   `extern fn` declaration can actually reach symbols that live outside
   libc (libm, libsqlite3, ...). Declaration only; carries no codegen of
   its own — collected onto Program and consumed by main.c after codegen
   succeeds. Order-preserving, de-duplicated by name (see link_lib_add). */
typedef struct LinkLib {
    char *name;
    struct LinkLib *next;
} LinkLib;

/* M31: `shim "path.c";` top-level directive — requests that the given C
   source file be compiled (once, as its own translation unit, exactly
   the way `runtime.c` already is) and the resulting `.o` added to the
   final `cc` link command. This is the missing piece that makes shim-
   using FFI code (e.g. ffi/sqlite3_shim.c, std/net_shim/net_shim.c)
   buildable with a single `rapidc foo.rapid -o foo` instead of a manual
   `cc -c shim.c -o shim.o && rapidc foo.rapid -obj shim.o -l ...` two-step.
   Path is resolved relative to the .rapid file that declares it (same
   convention as `use "...";`), so it travels correctly regardless of the
   caller's current working directory. Declaration only; carries no
   codegen of its own — collected onto Program and consumed by main.c
   after codegen succeeds, same as link_libs. Order-preserving,
   de-duplicated by resolved absolute path (repeating `shim "x.c";` in
   several imported files, or by hand, must not compile/link `x.o` twice). */
typedef struct LinkObj {
    char *path;   /* absolute path to the .c shim source */
    struct LinkObj *next;
} LinkObj;

/* M6: program is now a list of functions (main must exist among them) */
typedef struct {
    Function *functions; /* linked list, in source order */
    Function *main_fn;   /* convenience pointer into `functions` */
    LinkLib *link_libs;  /* M28: `link "name";` directives, in source order, de-duplicated */
    LinkObj *link_objs;  /* M31: `shim "path.c";` directives, resolved to absolute paths, in source order, de-duplicated */
} Program;

/* M11: first-class function signature (no closures — just a bare top-level
   function's arity/param-types/ret-type, so a `fn(...)`-typed variable can be
   checked for compatibility against whatever function name is assigned to
   it). Stored globally per function name and looked up by name whenever an
   identifier is used as a value instead of being called directly. */
typedef struct FnSig {
    char *name;
    TypeKind ret_type;
    TypeKind ret_elem_type;        /* M26: pointee type when ret_type == TYPE_PTR */
    TypeKind ret_elem_elem_type;   /* M32: pointee of the pointee, valid only when ret_elem_type == TYPE_PTR */
    TypeKind param_types[16];
    TypeKind param_elem_types[16]; /* M26: pointee type per param when param_types[i] == TYPE_PTR */
    TypeKind param_elem_elem_types[16]; /* M32: pointee of the pointee per param, valid only when param_elem_types[i] == TYPE_PTR */
    int param_count;
    struct FnSig *next;
} FnSig;

Expr *expr_new_int(long v);
Expr *expr_new_float(double v); /* M25: floating point literal */
Expr *expr_new_char(long v);   /* M13: 'x' literal → byte */
Expr *expr_new_null(void);     /* M33: `null` pointer literal */
Expr *expr_new_bool(long val); /* `true` (1) / `false` (0) literal */
Expr *expr_new_string(char *s);
Expr *expr_new_ident(char *name);
Expr *expr_new_io_out(Expr *arg);
Expr *expr_new_io_in(void);                       /* M16: io::in() */
Expr *expr_new_io_format(Expr *fmt, ExprList *args); /* M16: io::format(fmt, args...) */
Expr *expr_new_io_syscall(char *name, ExprList *args); /* M18: io::open/read/write/close(...) */
Expr *expr_new_binop(BinOpKind op, Expr *lhs, Expr *rhs);
Expr *expr_new_array_literal(ExprList *elements, int count);
Expr *expr_new_index(Expr *arr, Expr *idx);
Expr *expr_new_call(char *name, ExprList *args);
Expr *expr_new_addr_of(Expr *expr);
Expr *expr_new_deref(Expr *expr);
Expr *expr_new_neg(Expr *expr); /* M29: unary minus (-expr) */
Expr *expr_new_not(Expr *expr); /* logical negation (!expr) */
Expr *expr_new_logand(Expr *lhs, Expr *rhs); /* expr && expr, short-circuit */
Expr *expr_new_logor(Expr *lhs, Expr *rhs);  /* expr || expr, short-circuit */
Expr *expr_new_field(Expr *base, char *field);
Expr *expr_new_enum_literal(char *enum_name, char *member_name);
Expr *expr_new_struct_literal(char *struct_name, StructLiteralField *fields);
Expr *expr_new_func_ref(char *name);
Expr *expr_new_call_indirect(Expr *callee, ExprList *args);
ExprList *exprlist_new(Expr *e, ExprList *next);

Stmt *stmt_new_var_decl(char *name, TypeKind type, Expr *init);
Stmt *stmt_new_const_decl(char *name, TypeKind type, Expr *init);
Stmt *stmt_new_return(Expr *expr);
Stmt *stmt_new_expr(Expr *expr);
Stmt *stmt_new_if(Expr *cond, Stmt *then_body, Stmt *else_body);
Stmt *stmt_new_assign(char *name, Expr *value);
Stmt *stmt_new_inc(char *name);
Stmt *stmt_new_dec(char *name);
Stmt *stmt_new_while(Expr *cond, Stmt *body);
Stmt *stmt_new_do_while(Stmt *body, Expr *cond);
Stmt *stmt_new_for(Stmt *init, Expr *cond, Stmt *post, Stmt *body);
Stmt *stmt_new_for_in(char *var_name, Expr *array_expr, Stmt *body);
Stmt *stmt_new_break(void);
Stmt *stmt_new_continue(void);
Stmt *stmt_new_switch(Expr *subject, CaseClause *cases);
Stmt *stmt_new_index_assign(Expr *target, Expr *index, Expr *value);
Stmt *stmt_new_deref_assign(Expr *target, Expr *value);
Stmt *stmt_new_field_assign(Expr *base, char *field, Expr *value);
CaseClause *case_clause_new(Expr *value, Stmt *body, CaseClause *next);

Param *param_new(char *name, TypeKind type, Param *next);
Function *function_new(char *name, TypeKind ret_type, Param *params, Stmt *body);
/* M10: `efn name(params) => expr` — desugars to a Function whose body is a
   single `return expr;` statement. ret_type is left as TYPE_VOID (placeholder);
   semantic.c infers and fixes it up before signatures are registered.
   Untyped efn params (bare identifiers, e.g. `efn square(x) => ...`) default
   to TYPE_INT, this language's only general-purpose numeric type. */
Function *efn_new(char *name, Param *params, Expr *body_expr);
Param *param_new_untyped(char *name, Param *next);
/* M27: `extern fn name(params[, ...]): ret;` declaration — body-less,
   is_extern=1. `is_variadic` reflects whether the param list ended in `...`. */
Function *function_new_extern(char *name, TypeKind ret_type, Param *params, int is_variadic);
/* M28: record a `link "name";` directive onto g_program.link_libs (source
   order, de-duplicated by name — repeating `link "m";` doesn't add a
   second -lm). */
void link_lib_add(const char *name);
/* M31: record a `shim "path.c";` directive onto g_program.link_objs.
   `abs_path` must already be resolved to an absolute path by the caller
   (module.c, at the same point it resolves `use "...";` relative to the
   declaring file) — de-duplicated by that absolute path so the same shim
   reached via different relative paths, or repeated across several
   `use`d files, still only compiles/links once. */
void link_obj_add(const char *abs_path);

/* M8: struct/enum definition helpers */
StructDef *struct_def_new(char *name, Field *fields);
Field *field_new(char *name, TypeKind type, Field *next);
StructDef *struct_find(const char *name);
StructLiteralField *struct_literal_field_new(char *field, Expr *value, StructLiteralField *next);
EnumDef *enum_def_new(char *name, EnumMember *members);
EnumMember *enum_member_new(char *name, int value, EnumMember *next);
EnumDef *enum_find(const char *name);
int enum_member_value(EnumDef *e, const char *member);

/* M11: first-class function signature registry. Populated by semantic.c
   (once, from the function list) before any expression referencing a bare
   function name is checked. */
void fnsig_register(const char *name, TypeKind ret_type, Param *params, int param_count);
FnSig *fnsig_find(const char *name);

extern Program g_program;

/* Resets g_program and the parser's internal struct/enum lookup tables to
   their initial empty state. Does not free the previous AST (see ast.c for
   why) -- just stops anything from seeing it. An embedder (LSP, watch-mode,
   build-server) reusing the process across multiple compiles must call
   this before each yyparse(); a normal one-shot CLI run doesn't need to,
   since g_program already starts zeroed at process start. */
void ast_reset(void);

#endif

/* M42: kaynak konumu. Lexer `//@src N dosya` isaretlerini gorunce ayarlar. */
extern const char *g_src_file;
int ast_cur_line(void);
const char *ast_cur_file(void);

/* M43: hata yonetimi */
Expr *expr_new_try(Expr *call);
Expr *expr_new_catch(Expr *call, Expr *dflt);
Expr *expr_new_error_lit(char *name);
Stmt *stmt_new_fail(Expr *code);
int error_code_intern(const char *name);   /* ad -> kod (>=1); yoksa yeni kod verir */
const char *error_name_of(int code);       /* kod -> ad (NULL: bilinmeyen) */
int error_count(void);                     /* kayitli hata sayisi (kodlar 1..N) */
