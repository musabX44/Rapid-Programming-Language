#include "semantic.h"
#include <string.h>
#include "var_struct_map.h"

#define MAX_VARS 64
#define MAX_FUNCS 64

typedef struct {
    char *names[MAX_VARS];
    TypeKind types[MAX_VARS];
    TypeKind elem_types[MAX_VARS]; /* M26: pointee type when types[i] == TYPE_PTR */
    TypeKind elem_elem_types[MAX_VARS]; /* M32: pointee of the pointee, valid only when elem_types[i] == TYPE_PTR */
    int is_const[MAX_VARS];
    char *struct_names[MAX_VARS];
    int count;
} SymTable;

/* M6: global function signature table, built once before checking bodies so
   forward references / recursion work regardless of definition order. */
typedef struct {
    char *name;
    TypeKind ret_type;
    TypeKind ret_elem_type;               /* M26 */
    TypeKind ret_elem_elem_type;          /* M32 */
    TypeKind param_types[MAX_VARS];
    TypeKind param_elem_types[MAX_VARS];  /* M26 */
    TypeKind param_elem_elem_types[MAX_VARS]; /* M32 */
    int param_count;
    int is_variadic; /* M27: extern fn declared with trailing `...` */
} FuncSig;

static FuncSig g_funcs[MAX_FUNCS];
static int g_func_count = 0;

static int had_error = 0;
static int semantic_loop_depth = 0; /* tracks nesting inside while/for/for-in, for break/continue validation */
static TypeKind current_fn_ret_type = TYPE_VOID; /* for checking `return <expr>;` matches declared type */
static TypeKind current_fn_ret_elem_type = TYPE_VOID; /* M26: pointee type when current_fn_ret_type == TYPE_PTR */
static TypeKind current_fn_ret_elem_elem_type = TYPE_VOID; /* M32: pointee of the pointee, valid only when current_fn_ret_elem_type == TYPE_PTR */

static void sem_error(const char *msg, const char *name) {
    fprintf(stderr, "semantic error: %s: %s\n", msg, name ? name : "");
    had_error = 1;
}

static int sym_find(SymTable *st, const char *name) {
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->names[i], name) == 0) return i;
    }
    return -1;
}

static void sym_add(SymTable *st, char *name, TypeKind type, int is_const, const char *struct_name, TypeKind elem_type, TypeKind elem_elem_type) {
    if (st->count >= MAX_VARS) {
        sem_error("too many variables (milestone limit)", name);
        return;
    }
    if (sym_find(st, name) != -1) {
        sem_error("redeclaration of variable", name);
        return;
    }
    st->names[st->count] = name;
    st->types[st->count] = type;
    st->elem_types[st->count] = elem_type; /* M26 */
    st->elem_elem_types[st->count] = elem_elem_type; /* M32 */
    st->is_const[st->count] = is_const;
    st->struct_names[st->count] = (char *)struct_name;
    st->count++;
    /* M8: remember var -> struct name globally so codegen can resolve field
       offsets when it later sees `var.field` expressions. */
    if (struct_name) {
        var_struct_map_set(name, struct_name);
    }
}

static FuncSig *func_find(const char *name) {
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, name) == 0) return &g_funcs[i];
    }
    return NULL;
}

/* Element type produced by indexing a value of `arr_type`. */
static TypeKind element_type_of(TypeKind arr_type) {
    if (arr_type == TYPE_INT_ARRAY) return TYPE_INT;
    if (arr_type == TYPE_INT_LIST) return TYPE_INT;
    if (arr_type == TYPE_BYTE_LIST) return TYPE_BYTE;
    return TYPE_VOID;
}

/* M26: recovers the pointee (elem_type) of an already-checked TYPE_PTR
   expression. Needed because check_expr only returns a bare TypeKind
   (TYPE_PTR), losing what it points to — this walks the same small set of
   "produces a pointer" expression shapes semantic.c already understands
   and pulls elem_type from wherever it was stashed when that expression
   was itself checked: EXPR_ADDR_OF/EXPR_DEREF/EXPR_CALL("alloc") stash it
   directly on the node (e->elem_type); EXPR_IDENT looks it up in the
   symbol table (a var/param declared as `T*` carries elem_type there);
   EXPR_FIELD looks it up on the struct field definition. Falls back to
   TYPE_INT (the historical int* default) if the shape isn't recognized —
   should not happen for a well-typed TYPE_PTR expression given the shapes
   the grammar/semantic pass actually produce. */
static TypeKind expr_elem_type_of(SymTable *st, Expr *e) {
    switch (e->kind) {
        case EXPR_ADDR_OF:
        case EXPR_DEREF:
            return e->elem_type;
        case EXPR_IDENT: {
            int idx = sym_find(st, e->name);
            if (idx != -1) return st->elem_types[idx];
            return TYPE_INT;
        }
        case EXPR_CALL:
            /* alloc(n) and any user function returning T* — elem_type was
               stashed on the call node when it was checked. */
            return e->elem_type;
        case EXPR_FIELD: {
            if (e->lhs && e->lhs->kind == EXPR_IDENT) {
                const char *sname = var_struct_map_get(e->lhs->name);
                StructDef *sd = sname ? struct_find(sname) : NULL;
                for (Field *f = sd ? sd->fields : NULL; f; f = f->next) {
                    if (strcmp(f->name, e->name) == 0) return f->elem_type;
                }
            }
            return TYPE_INT;
        }
        case EXPR_INDEX:
            /* arrays/lists of pointers aren't a supported element type
               (element_type_of only returns int/byte) — not reachable for
               a well-typed TYPE_PTR expression. */
            return TYPE_INT;
        default:
            return TYPE_INT;
    }
}

/* M32: recovers the pointee-of-the-pointee (elem_elem_type) of an already-
   checked TYPE_PTR expression whose elem_type is itself TYPE_PTR (i.e. this
   expr is T**). Mirrors expr_elem_type_of() one level down — same shapes,
   same stash-and-fallback approach. Only meaningful when the caller has
   already established expr_elem_type_of(st, e) == TYPE_PTR; callers that
   haven't should not trust the result. */
static TypeKind expr_elem_elem_type_of(SymTable *st, Expr *e) {
    switch (e->kind) {
        case EXPR_ADDR_OF:
        case EXPR_DEREF:
            return e->elem_elem_type;
        case EXPR_IDENT: {
            int idx = sym_find(st, e->name);
            if (idx != -1) return st->elem_elem_types[idx];
            return TYPE_INT;
        }
        case EXPR_CALL:
            return e->elem_elem_type;
        case EXPR_FIELD: {
            if (e->lhs && e->lhs->kind == EXPR_IDENT) {
                const char *sname = var_struct_map_get(e->lhs->name);
                StructDef *sd = sname ? struct_find(sname) : NULL;
                for (Field *f = sd ? sd->fields : NULL; f; f = f->next) {
                    if (strcmp(f->name, e->name) == 0) return f->elem_elem_type;
                }
            }
            return TYPE_INT;
        }
        default:
            return TYPE_INT;
    }
}

/* M9: names reserved for the dynamic-list builtins. These are checked and
   codegen'd specially (like io::out) rather than being real user functions,
   so they aren't in the function-signature table and can't be redefined. */
static int is_list_builtin(const char *name) {
    return strcmp(name, "push") == 0 || strcmp(name, "listCopy") == 0 || strcmp(name, "len") == 0;
}

/* M12: names reserved for the sysprog builtins (raw system-programming
   primitives) - same treatment as is_list_builtin above. */
static int is_sysprog_builtin(const char *name) {
    return strcmp(name, "alloc") == 0 || strcmp(name, "free") == 0 || strcmp(name, "syscall") == 0;
}

/* M20: names reserved for the string<->int conversion builtins - same
   treatment as is_list_builtin/is_sysprog_builtin above (not real user
   functions, checked and codegen'd specially). */
static int is_conv_builtin(const char *name) {
    return strcmp(name, "toInt") == 0 || strcmp(name, "toString") == 0;
}

/* M23: replaces the old hand-written pairwise `(a==TYPE_BYTE && b==TYPE_INT)
   || (a==TYPE_INT && b==TYPE_BYTE)` checks scattered across this file (call
   args, var-decl init, return, assignment, struct literal fields). Any two
   integer types (int/byte/int8..64/uint8..64) are now mutually compatible —
   narrowing/widening is implicit, matching this language's existing
   int<->byte behavior; a future milestone can add opt-in strictness
   (warnings on narrowing) without touching call sites that use this. */
static int types_int_compatible(TypeKind a, TypeKind b) {
    return type_is_integer(a) && type_is_integer(b);
}

/* M25: float and double are mutually compatible (implicit widen/narrow),
   mirroring the integer family's types_int_compatible above. Integer and
   float types are deliberately NOT compatible here — this language already
   treats int/byte/int8..64 as interchangeable, but mixing an integer and a
   floating type implicitly would hide a real representation change (and a
   real QBE register-class change); that conversion has to be explicit
   (toFloat()/toDouble()/toInt()-style builtins are left to a follow-up
   milestone) rather than silently inserted by binop/assignment/call-arg
   checks. */
static int types_float_compatible(TypeKind a, TypeKind b) {
    return type_is_float(a) && type_is_float(b);
}

/* M25: result type of combining two float-family operands of possibly
   different widths — mirrors type_int_promote (the wider one wins). Callers
   must check types_float_compatible() on both operands first. */
static TypeKind type_float_promote(TypeKind a, TypeKind b) {
    if (a == b) return a;
    return (a == TYPE_DOUBLE || b == TYPE_DOUBLE) ? TYPE_DOUBLE : TYPE_FLOAT;
}

static void check_stmt(SymTable *st, Stmt *s);

static TypeKind check_expr(SymTable *st, Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LITERAL:
            return TYPE_INT;
        case EXPR_FLOAT_LITERAL:
            /* M25: a bare `3.14` literal defaults to double (the language's
               general-purpose floating type), same spirit as EXPR_INT_LITERAL
               defaulting to TYPE_INT — a declaration like `var f: float =
               3.14;` still works because of types_float_compatible below. */
            return TYPE_DOUBLE;
        case EXPR_CHAR_LITERAL:
            return TYPE_BYTE;
        case EXPR_STRING_LITERAL:
            return TYPE_STRING;
        case EXPR_IDENT: {
            int idx = sym_find(st, e->name);
            if (idx == -1) {
                /* M11: not a variable — if it names a known top-level
                   function, it's a first-class function value instead
                   (no closures: just the bare function reference). */
                if (fnsig_find(e->name)) {
                    e->kind = EXPR_FUNC_REF;
                    return TYPE_FN;
                }
                sem_error("undefined identifier", e->name);
                return TYPE_INT; /* keep going */
            }
            return st->types[idx];
        }
        case EXPR_IO_OUT_CALL: {
            check_expr(st, e->arg);
            return TYPE_VOID;
        }
        case EXPR_IO_IN_CALL: {
            /* M17: io::in() takes no arguments and always yields a string */
            return TYPE_STRING;
        }
        case EXPR_IO_FORMAT_CALL: {
            /* M17: io::out(fmt, args...) with 2+ arguments - fmt must be a
               string; each further arg can be int/byte/string (the only
               printable types plain io::out already supports). */
            TypeKind fmt_type = check_expr(st, e->lhs);
            if (fmt_type != TYPE_STRING) {
                sem_error("io::out format string must be a string", NULL);
            }
            for (ExprList *l = e->args; l; l = l->next) {
                TypeKind at = check_expr(st, l->expr);
                if (at != TYPE_INT && at != TYPE_BYTE && at != TYPE_STRING) {
                    sem_error("io::out arguments must be int, byte, or string", NULL);
                }
            }
            return TYPE_VOID;
        }
        case EXPR_IO_SYSCALL_CALL: {
            /* M18: io::open/read/write/close - raw file syscall wrappers.
               Argument shapes:
                 io::open(path: string, flags: int, mode: int) -> int (fd)
                 io::read(fd: int, buf: byte*, count: int)     -> int (nread)
                 io::write(fd: int, buf: byte*, count: int)    -> int (nwritten)
                 io::close(fd: int)                            -> int (0 or -1)
               Checked here rather than via the function-signature table for
               the same reason as sysprog:: builtins: no single fixed
               signature shape is shared with ordinary user functions. */
            int argc = 0;
            TypeKind arg_types[8];
            for (ExprList *l = e->args; l && argc < 8; l = l->next, argc++) {
                arg_types[argc] = check_expr(st, l->expr);
            }
            for (ExprList *l = e->args; l && argc >= 8; l = l->next) {
                /* still typecheck any extras beyond the cap, just don't record them */
                check_expr(st, l->expr);
            }

            if (strcmp(e->name, "open") == 0) {
                if (argc != 3) {
                    sem_error("io::open takes exactly 3 arguments (path, flags, mode)", NULL);
                    return TYPE_INT;
                }
                if (arg_types[0] != TYPE_STRING) {
                    sem_error("io::open's path argument must be a string", NULL);
                }
                if (arg_types[1] != TYPE_INT || arg_types[2] != TYPE_INT) {
                    sem_error("io::open's flags and mode arguments must be int", NULL);
                }
                return TYPE_INT;
            }
            if (strcmp(e->name, "read") == 0 || strcmp(e->name, "write") == 0) {
                if (argc != 3) {
                    sem_error("io::read/io::write take exactly 3 arguments (fd, buf, count)", NULL);
                    return TYPE_INT;
                }
                if (arg_types[0] != TYPE_INT) {
                    sem_error("io::read/io::write's fd argument must be int", NULL);
                }
                if (arg_types[1] != TYPE_PTR && arg_types[1] != TYPE_STRING) {
                    sem_error("io::read/io::write's buf argument must be a pointer or a string", NULL);
                }
                if (arg_types[2] != TYPE_INT) {
                    sem_error("io::read/io::write's count argument must be int", NULL);
                }
                return TYPE_INT;
            }
            /* close */
            if (strcmp(e->name, "close") == 0) {
                if (argc != 1) {
                    sem_error("io::close takes exactly 1 argument (fd)", NULL);
                    return TYPE_INT;
                }
                if (arg_types[0] != TYPE_INT) {
                    sem_error("io::close's fd argument must be int", NULL);
                }
                return TYPE_INT;
            }
            /* errno */
            if (argc != 0) {
                sem_error("io::errno takes no arguments", NULL);
            }
            return TYPE_INT;
        }
        case EXPR_FUNC_REF: {
            return TYPE_FN;
        }
        case EXPR_CALL_INDIRECT: {
            check_expr(st, e->lhs);
            for (ExprList *l = e->args; l; l = l->next) check_expr(st, l->expr);
            return TYPE_INT;
        }
        case EXPR_ARRAY_LITERAL: {
            /* Milestone limit: only int[] literals are supported so far.
               `[]` (empty literal) type-checks as TYPE_INT_ARRAY here too;
               a `list<int>` var/const declaration initialized from a literal
               is special-cased in check_stmt to build a dynamic list instead. */
            for (ExprList *l = e->elements; l; l = l->next) {
                TypeKind et = check_expr(st, l->expr);
                if (et != TYPE_INT) {
                    sem_error("array literal elements must be int (milestone limit)", NULL);
                }
            }
            return TYPE_INT_ARRAY;
        }
        case EXPR_INDEX: {
            TypeKind arr_type = check_expr(st, e->lhs);
            TypeKind idx_type = check_expr(st, e->rhs);
            if (arr_type != TYPE_INT_ARRAY && arr_type != TYPE_INT_LIST && arr_type != TYPE_BYTE_LIST && arr_type != TYPE_STRING) {
                sem_error("indexing requires an array, list, or string", NULL);
            }
            if (idx_type != TYPE_INT) {
                sem_error("index must be int", NULL);
            }
            if (arr_type == TYPE_STRING) return TYPE_BYTE;
            return element_type_of(arr_type);
        }
        case EXPR_ADDR_OF: {
            TypeKind t = check_expr(st, e->arg);
            /* M26: any type_ptr_ok() operand may now have its address
               taken (previously only int/byte). elem_type is stashed on
               `e` itself (the EXPR_ADDR_OF node) so codegen and any
               enclosing var-decl/param/return check can recover exactly
               what this pointer points to — check_expr only returns a
               bare TypeKind, so this side field is how the extra info
               escapes this call. */
            if (!type_ptr_ok(t)) {
                sem_error("address-of operator requires an addressable lvalue of a pointer-capable type", NULL);
            }
            e->elem_type = t;
            /* M32: if the operand is itself a pointer (t == TYPE_PTR, e.g.
               `&p` where p: T*), this EXPR_ADDR_OF node is T** — stash what
               *that* inner pointer points to (p's own elem_type) so a
               later *pp double-deref, or a `var pp: T** = &p;` decl check,
               can recover the full two-level shape via
               expr_elem_elem_type_of(). Meaningless (left at whatever it
               already was, typically TYPE_VOID/TYPE_INT) when t != TYPE_PTR. */
            if (t == TYPE_PTR) {
                e->elem_elem_type = expr_elem_type_of(st, e->arg);
            }
            return TYPE_PTR;
        }
        case EXPR_DEREF: {
            TypeKind t = check_expr(st, e->arg);
            if (t != TYPE_PTR) {
                sem_error("dereference requires a pointer", NULL);
                e->elem_type = TYPE_INT;
                return TYPE_INT;
            }
            /* M26: the operand's elem_type was set when *it* was checked
               (either directly on an EXPR_ADDR_OF node, or propagated from
               a variable's declared elem_type via EXPR_IDENT below). */
            TypeKind arg_elem = expr_elem_type_of(st, e->arg);
            /* M32: arg_elem is *this expr's own static type* (e.g. for
               `*pp` where pp: T**, arg_elem == TYPE_PTR since *pp is T*).
               e->elem_type must hold *this expr's* pointee (what *this*
               expr points to), which is one level further down: if
               arg_elem is itself TYPE_PTR, that pointee is e->arg's
               elem_elem_type (pp's elem_elem_type, i.e. T); otherwise
               (plain T*) it's e->arg's own elem_type as before. */
            if (arg_elem == TYPE_PTR) {
                e->elem_type = expr_elem_elem_type_of(st, e->arg);
                e->elem_elem_type = TYPE_VOID; /* no T*** support */
            } else {
                e->elem_type = arg_elem;
            }
            return arg_elem;
        }
        case EXPR_NEG: {
            /* M29: unary minus. Result type is exactly the operand's type
               (int stays int, float/double stays float/double) — unlike
               the `0 - expr` desugar this replaces, there is no zero
               literal here to be incompatible with a float operand. */
            TypeKind t = check_expr(st, e->arg);
            if (!type_is_integer(t) && !type_is_float(t)) {
                sem_error("unary '-' requires an integer or float operand", NULL);
            }
            return t;
        }
        case EXPR_CALL: {
            /* M11: `f(args)` where `f` is a variable/param holding a
               function value (TYPE_FN), not a top-level function name —
               indirect call. Mark the node so codegen knows to call through
               a register instead of `$f_<name>`. Argument count/types
               aren't verified against the held function's signature in
               this milestone (TYPE_FN is a flat tag with no retained
               per-value signature). */
            {
                int idx = sym_find(st, e->name);
                if (idx != -1 && st->types[idx] == TYPE_FN) {
                    e->kind = EXPR_CALL_INDIRECT;
                    e->lhs = expr_new_ident(e->name);
                    for (ExprList *l = e->args; l; l = l->next) check_expr(st, l->expr);
                    return TYPE_INT; /* unknown static return type; treated as int */
                }
            }
            /* M12: sysprog builtins (alloc/free/syscall) - raw
               system-programming primitives, checked here for the same
               reason as the list builtins above (no single fixed
               signature shape shared with user functions). */
            if (is_sysprog_builtin(e->name)) {
                int argc = 0;
                for (ExprList *l = e->args; l; l = l->next, argc++) {
                    TypeKind t = check_expr(st, l->expr);
                    if (strcmp(e->name, "free") == 0) {
                        if (argc == 0 && t != TYPE_PTR) {
                            sem_error("free's argument must be a pointer", NULL);
                        }
                    } else {
                        /* alloc's size and syscall's nr/args are all plain ints */
                        if (t != TYPE_INT) {
                            sem_error("sysprog builtin arguments must be int (or a pointer for free)", NULL);
                        }
                    }
                }
                if (strcmp(e->name, "alloc") == 0) {
                    if (argc != 1) sem_error("alloc takes exactly 1 argument (size)", NULL);
                    /* M26: elem_type is not known here — alloc() is
                       context-typed, inferred by whichever enclosing
                       var-decl/assign/param/return checked this call and
                       set e->elem_type accordingly (see STMT_VAR_DECL /
                       STMT_ASSIGN). Defaults to TYPE_INT if alloc() is used
                       somewhere that doesn't infer it (e.g. passed directly
                       as a call argument) — matches the historical
                       int*-by-default behavior. */
                    if (e->elem_type == TYPE_VOID) e->elem_type = TYPE_INT;
                    return TYPE_PTR;
                }
                if (strcmp(e->name, "free") == 0) {
                    if (argc != 1) sem_error("free takes exactly 1 argument (ptr)", NULL);
                    return TYPE_VOID;
                }
                /* syscall */
                if (argc < 1 || argc > 7) {
                    sem_error("syscall takes 1 to 7 arguments (nr, up to 6 args)", NULL);
                }
                return TYPE_INT;
            }
            /* M20: toInt(str)/toString(int) - checked here for the same
               reason as the other builtin families above (no single fixed
               signature shared with user functions). */
            if (is_conv_builtin(e->name)) {
                int argc = 0;
                TypeKind arg_type = TYPE_VOID;
                for (ExprList *l = e->args; l; l = l->next, argc++) {
                    arg_type = check_expr(st, l->expr);
                }
                if (strcmp(e->name, "toInt") == 0) {
                    if (argc != 1) {
                        sem_error("toInt takes exactly 1 argument (string)", NULL);
                        return TYPE_INT;
                    }
                    if (arg_type != TYPE_STRING) {
                        sem_error("toInt's argument must be a string", NULL);
                    }
                    return TYPE_INT;
                }
                /* toString */
                if (argc != 1) {
                    sem_error("toString takes exactly 1 argument (int)", NULL);
                    return TYPE_STRING;
                }
                if (arg_type != TYPE_INT && arg_type != TYPE_BYTE) {
                    sem_error("toString's argument must be an int (or byte)", NULL);
                }
                return TYPE_STRING;
            }
            /* M9: list builtins are checked here instead of going through
               the user function-signature table (they have no single fixed
               signature: push/listCopy are generic over element type). */
            if (is_list_builtin(e->name)) {
                int argc = 0;
                ExprList *l = e->args;
                TypeKind arg_types[8];
                for (; l && argc < 8; l = l->next, argc++) arg_types[argc] = check_expr(st, l->expr);
                for (; l; l = l->next) check_expr(st, l->expr); /* still typecheck extras */
                if (strcmp(e->name, "push") == 0) {
                    if (argc != 2) {
                        sem_error("push takes exactly 2 arguments (list, value)", NULL);
                        return TYPE_VOID;
                    }
                    if (arg_types[0] != TYPE_INT_LIST && arg_types[0] != TYPE_BYTE_LIST) {
                        sem_error("push's first argument must be a list<int> or list<byte>", NULL);
                    } else if (arg_types[0] == TYPE_INT_LIST && arg_types[1] != TYPE_INT) {
                        sem_error("push's second argument must be an int", NULL);
                    } else if (arg_types[0] == TYPE_BYTE_LIST && arg_types[1] != TYPE_BYTE && arg_types[1] != TYPE_INT) {
                        sem_error("push's second argument must be a byte (or int)", NULL);
                    }
                    if (e->args->expr->kind != EXPR_IDENT) {
                        sem_error("push's first argument must be a list variable", NULL);
                    }
                    return TYPE_VOID;
                }
                if (strcmp(e->name, "listCopy") == 0) {
                    if (argc != 1) {
                        sem_error("listCopy takes exactly 1 argument", NULL);
                        return TYPE_INT_LIST;
                    }
                    if (arg_types[0] != TYPE_INT_LIST && arg_types[0] != TYPE_BYTE_LIST) {
                        sem_error("listCopy's argument must be a list<int> or list<byte>", NULL);
                        return TYPE_INT_LIST;
                    }
                    return arg_types[0];
                }
                /* len */
                if (argc != 1) {
                    sem_error("len takes exactly 1 argument", NULL);
                    return TYPE_INT;
                }
                if (arg_types[0] != TYPE_INT_LIST && arg_types[0] != TYPE_BYTE_LIST && arg_types[0] != TYPE_INT_ARRAY && arg_types[0] != TYPE_STRING) {
                    sem_error("len's argument must be a list, array, or string", NULL);
                }
                return TYPE_INT;
            }
            FuncSig *fs = func_find(e->name);
            if (!fs) {
                sem_error("call to undefined function", e->name);
                for (ExprList *l = e->args; l; l = l->next) check_expr(st, l->expr);
                return TYPE_VOID;
            }
            int i = 0;
            ExprList *l = e->args;
            for (; l && i < fs->param_count; l = l->next, i++) {
                TypeKind at = check_expr(st, l->expr);
                if (at != fs->param_types[i]) {
                    if (!types_int_compatible(fs->param_types[i], at) &&
                        !types_float_compatible(fs->param_types[i], at)) {
                        sem_error("argument type mismatch in call to", e->name);
                    }
                } else if (at == TYPE_PTR) {
                    /* M26: both sides are TYPE_PTR at the flat-enum level —
                       still need matching elem_type (int* passed where
                       float* expected shouldn't type-check just because
                       both are "a pointer"). */
                    TypeKind arg_elem = expr_elem_type_of(st, l->expr);
                    if (arg_elem != fs->param_elem_types[i]) {
                        sem_error("argument pointer type mismatch (pointee types differ) in call to", e->name);
                    } else if (arg_elem == TYPE_PTR) {
                        /* M32: T** passed where U** expected — elem_type
                           matching alone isn't enough (both are TYPE_PTR),
                           need the pointee-of-the-pointee to match too
                           (int** passed where byte** expected shouldn't
                           type-check just because both are "pointer to
                           pointer"). */
                        if (expr_elem_elem_type_of(st, l->expr) != fs->param_elem_elem_types[i]) {
                            sem_error("argument pointer type mismatch (pointee types differ) in call to", e->name);
                        }
                    }
                }
            }
            /* leftover args or leftover params means arity mismatch —
               except for a variadic extern (M27: e.g. printf), where extra
               args beyond the declared fixed params are allowed; they're
               still type-checked (just not matched against a declared
               parameter type, since there isn't one). */
            int arg_count = 0;
            for (ExprList *c = e->args; c; c = c->next) arg_count++;
            for (; l; l = l->next) check_expr(st, l->expr); /* still typecheck extras */
            if (arg_count != fs->param_count) {
                if (!(fs->is_variadic && arg_count > fs->param_count)) {
                    sem_error("wrong number of arguments in call to", e->name);
                }
            }
            e->elem_type = fs->ret_elem_type; /* M26: propagate for `var p: T* = f(...);` */
            e->elem_elem_type = fs->ret_elem_elem_type; /* M32: propagate for `var pp: T** = f(...);` */
            return fs->ret_type;
        }
        case EXPR_FIELD: {
            /* M8: `base.field` — base must be a struct variable whose struct
               name we remember via the var -> struct map. */
            TypeKind base_type = check_expr(st, e->lhs);
            if (base_type != TYPE_STRUCT || e->lhs->kind != EXPR_IDENT) {
                sem_error("field access requires a struct variable", NULL);
                return TYPE_INT;
            }
            const char *sname = var_struct_map_get(e->lhs->name);
            StructDef *sd = sname ? struct_find(sname) : NULL;
            if (!sd) {
                sem_error("cannot determine struct type of variable", e->lhs->name);
                return TYPE_INT;
            }
            for (Field *f = sd->fields; f; f = f->next) {
                if (strcmp(f->name, e->name) == 0) return f->type;
            }
            sem_error("no such field in struct", e->name);
            return TYPE_INT;
        }
        case EXPR_ENUM_LITERAL: {
            /* M8: `Color::GREEN` — enum and member must both exist. */
            EnumDef *ed = e->enum_name ? enum_find(e->enum_name) : NULL;
            if (!ed) {
                sem_error("undefined enum", e->enum_name);
                return TYPE_ENUM;
            }
            for (EnumMember *m = ed->members; m; m = m->next) {
                if (strcmp(m->name, e->member_name) == 0) return TYPE_ENUM;
            }
            sem_error("no such member in enum", e->member_name);
            return TYPE_ENUM;
        }
        case EXPR_STRUCT_LITERAL: {
            /* M8: `Point { x: 1, y: 2 }` — every listed field must exist in
               the struct, appear at most once, and type-match. */
            StructDef *sd = e->name ? struct_find(e->name) : NULL;
            if (!sd) {
                sem_error("undefined struct", e->name);
                return TYPE_STRUCT;
            }
            for (StructLiteralField *fl = e->struct_fields; fl; fl = fl->next) {
                Field *fd = NULL;
                for (Field *f = sd->fields; f; f = f->next) {
                    if (strcmp(f->name, fl->field) == 0) { fd = f; break; }
                }
                if (!fd) {
                    sem_error("no such field in struct literal", fl->field);
                    continue;
                }
                for (StructLiteralField *prev = e->struct_fields; prev != fl; prev = prev->next) {
                    if (strcmp(prev->field, fl->field) == 0) {
                        sem_error("duplicate field in struct literal", fl->field);
                    }
                }
                TypeKind vt = check_expr(st, fl->value);
                if (vt != fd->type) {
                    sem_error("type mismatch in struct literal field", fl->field);
                }
            }
            return TYPE_STRUCT;
        }
        case EXPR_BINOP: {
            TypeKind lt = check_expr(st, e->lhs);
            TypeKind rt = check_expr(st, e->rhs);
            if (lt != rt) {
                /* M23: any two integer types are compatible in binary
                   expressions (widened/narrowed implicitly, same as the
                   original byte<->int behavior). */
                int compat = types_int_compatible(lt, rt);
                /* M25: any two float-family types are likewise compatible
                   (float op double promotes to double). */
                int fcompat = types_float_compatible(lt, rt);
                /* M26: pointer arithmetic: T* + <any integer> or - (any
                   elem_type, generalized off TYPE_PTR instead of the old
                   two hardcoded pointer type names). */
                int ptr_arith = (lt == TYPE_PTR && type_is_integer(rt));
                if (!compat && !fcompat && !ptr_arith) {
                    sem_error("type mismatch in binary expression", NULL);
                    return TYPE_BOOL;
                }
                if (ptr_arith) {
                    /* only + and - are allowed; result is the same pointer
                       kind (elem_type carried on `e` itself so callers of
                       check_expr(e) on this binop can still recover it). */
                    if (e->op != OP_ADD && e->op != OP_SUB) {
                        sem_error("only + and - are allowed for pointer arithmetic", NULL);
                    }
                    TypeKind lelem = expr_elem_type_of(st, e->lhs);
                    e->elem_type = lelem;
                    /* M32: T** +/- int is still T** — propagate the
                       pointee-of-the-pointee too, same as elem_type just
                       above, so a further *result or a decl/assign check
                       against this binop still sees the full two-level
                       shape via expr_elem_elem_type_of(). */
                    if (lelem == TYPE_PTR) {
                        e->elem_elem_type = expr_elem_elem_type_of(st, e->lhs);
                    }
                    return lt;
                }
                if (fcompat) {
                    lt = rt = type_float_promote(lt, rt);
                } else {
                    /* mixed integer widths → promote per usual-arithmetic-conversion rules */
                    lt = rt = type_int_promote(lt, rt);
                }
            }
            /* M26: pointer op pointer — comparison is always OK, only
               subtraction for arithmetic. Both sides must point to the
               same elem_type (a T* and a U* aren't comparable/subtractable
               even though both are TYPE_PTR at the flat-enum level). */
            if (lt == TYPE_PTR && rt == TYPE_PTR) {
                TypeKind lelem = expr_elem_type_of(st, e->lhs);
                TypeKind relem = expr_elem_type_of(st, e->rhs);
                if (lelem != relem) {
                    sem_error("pointer type mismatch (pointee types differ)", NULL);
                } else if (lelem == TYPE_PTR) {
                    /* M32: T** vs U** — elem_type matching alone isn't
                       enough (both are TYPE_PTR at that level too); the
                       pointee-of-the-pointee must also match (int** vs
                       byte** shouldn't compare/subtract just because both
                       are "pointer to pointer"). */
                    if (expr_elem_elem_type_of(st, e->lhs) != expr_elem_elem_type_of(st, e->rhs)) {
                        sem_error("pointer type mismatch (pointee types differ)", NULL);
                    }
                }
                switch (e->op) {
                    case OP_EQ: case OP_NE:
                    case OP_LT: case OP_LE: case OP_GT: case OP_GE:
                        return TYPE_BOOL;
                    case OP_SUB:
                        return TYPE_INT;  /* pointer difference */
                    default:
                        sem_error("only comparison or subtraction allowed between two pointers", NULL);
                        return TYPE_INT;
                }
            }
            switch (e->op) {
                case OP_EQ: case OP_NE:
                    /* Allow comparison of pointers, any integer type, any
                       float type, or strings */
                    if (!type_is_integer(lt) && !type_is_float(lt) && lt != TYPE_STRING &&
                        lt != TYPE_PTR) {
                        sem_error("equality comparison requires an integer, float, string, or pointer operand", NULL);
                    }
                    return TYPE_BOOL;
                case OP_LT: case OP_LE: case OP_GT: case OP_GE:
                    if (!type_is_integer(lt) && !type_is_float(lt) && lt != TYPE_PTR) {
                        sem_error("relational comparison requires an integer, float, or pointer operand", NULL);
                    }
                    return TYPE_BOOL;
                case OP_ADD:
                    /* `+` works for any integer type (arithmetic), any float
                       type, and string (concatenation) */
                    if (!type_is_integer(lt) && !type_is_float(lt) && lt != TYPE_STRING) {
                        sem_error("'+' requires an integer, float, or string operand", NULL);
                    }
                    return lt;
                case OP_SUB: case OP_MUL: case OP_DIV:
                    if (!type_is_integer(lt) && !type_is_float(lt)) {
                        sem_error("arithmetic operator requires an integer or float operand", NULL);
                    }
                    return lt;
            }
            return TYPE_VOID;
        }
    }
    return TYPE_VOID;
}

static void check_block(SymTable *st, Stmt *body) {
    /* block scope: locals declared inside are not visible after the block */
    int saved_count = st->count;
    for (Stmt *s = body; s; s = s->next) {
        check_stmt(st, s);
    }
    st->count = saved_count;
}

static void check_stmt(SymTable *st, Stmt *s) {
    switch (s->kind) {
        case STMT_VAR_DECL:
        case STMT_CONST_DECL: {
            if (s->type == TYPE_STRUCT && !s->struct_name) {
                sem_error("struct-typed variable is missing its struct name", s->name);
            }
            if (s->expr) {
                TypeKind init_type = check_expr(st, s->expr);
                if (s->type == TYPE_STRUCT) {
                    if (init_type != TYPE_STRUCT) {
                        sem_error("type mismatch in declaration", s->name);
                    } else if (s->expr->kind == EXPR_STRUCT_LITERAL && s->struct_name &&
                               strcmp(s->expr->name, s->struct_name) != 0) {
                        sem_error("struct literal type mismatch in declaration", s->name);
                    }
                } else if (s->type == TYPE_INT_LIST) {
                    /* M9: `list<int> xs = [1, 2, 3];` (or `= [];`) is allowed —
                       an array literal initializes a dynamic list. Anything
                       else must already be TYPE_INT_LIST (e.g. listCopy(...)
                       or another list variable). */
                    if (init_type != TYPE_INT_ARRAY && init_type != TYPE_INT_LIST) {
                        sem_error("type mismatch in declaration", s->name);
                    }
                } else if (s->type == TYPE_BYTE_LIST) {
                    /* M20: `list<byte> bs = [];` / `= listCopy(other);` /
                       `= other;` — mirrors the TYPE_INT_LIST case above. An
                       array literal here must be empty (byte array literal
                       syntax doesn't exist yet), otherwise the initializer
                       must already be a byte list. */
                    if (init_type != TYPE_BYTE_LIST &&
                        !(init_type == TYPE_INT_ARRAY && s->expr->kind == EXPR_ARRAY_LITERAL && s->expr->elem_count == 0)) {
                        sem_error("type mismatch in declaration", s->name);
                    }
                } else if (s->type == TYPE_PTR) {
                    /* M26: `T* p = alloc(n);` — alloc() is a flat untyped
                       builtin (always statically TYPE_PTR with no elem_type
                       of its own), so its elem_type is inferred here from
                       the declared target type, same spot the old int
                       pointer / byte pointer alloc_ptr_compat carve-out
                       used to live.
                       Any other pointer-typed initializer (address-of,
                       another pointer variable, a function call returning
                       T*, a deref of a T**-shaped expr) must already carry
                       a matching elem_type. */
                    if (init_type != TYPE_PTR) {
                        sem_error("type mismatch in declaration", s->name);
                    } else if (s->expr->kind == EXPR_CALL && strcmp(s->expr->name, "alloc") == 0) {
                        s->expr->elem_type = s->elem_type; /* infer alloc's pointee from the target */
                        s->expr->elem_elem_type = s->elem_elem_type; /* M32: same, one level deeper */
                    } else {
                        TypeKind init_elem = expr_elem_type_of(st, s->expr);
                        if (init_elem != s->elem_type) {
                            sem_error("pointer type mismatch in declaration (pointee types differ)", s->name);
                        } else if (init_elem == TYPE_PTR && expr_elem_elem_type_of(st, s->expr) != s->elem_elem_type) {
                            /* M32: `var pp: int** = other_pp;` where
                               other_pp is a byte** — both sides are T*
                               pointing at "a pointer" (elem_type ==
                               TYPE_PTR matches), but the pointee-of-the-
                               pointee still needs to match. */
                            sem_error("pointer type mismatch in declaration (pointee types differ)", s->name);
                        }
                    }
                } else if (init_type != s->type) {
                    /* M23: any two integer types are interchangeable for
                       initialization (e.g. an int literal can initialize a
                       byte/int8/uint32/... variable and vice versa) — same
                       implicit-widen/narrow behavior the language already
                       had for byte<->int, generalized to every width.
                       M25: likewise, any two float-family types (float,
                       double) are interchangeable for initialization. */
                    int compat = types_int_compatible(s->type, init_type) ||
                                 types_float_compatible(s->type, init_type);
                    if (!compat) {
                        sem_error("type mismatch in declaration", s->name);
                    }
                }
            } else {
                /* M8: only struct declarations may omit an initializer
                   (`var p: Point;` — zero-initialized). */
                if (s->type != TYPE_STRUCT) {
                    sem_error("declaration requires an initializer", s->name);
                }
            }
            sym_add(st, s->name, s->type, s->kind == STMT_CONST_DECL, s->struct_name, s->elem_type, s->elem_elem_type);
            break;
        }
        case STMT_RETURN:
            if (s->expr) {
                TypeKind rt = check_expr(st, s->expr);
                if (rt != current_fn_ret_type) {
                    if (!types_int_compatible(current_fn_ret_type, rt) &&
                        !types_float_compatible(current_fn_ret_type, rt)) {
                        sem_error("return type mismatch", NULL);
                    }
                } else if (rt == TYPE_PTR) {
                    /* M26: mirrors STMT_VAR_DECL/STMT_ASSIGN — infer
                       alloc()'s elem_type from the function's declared
                       return type, otherwise require a matching elem_type. */
                    if (s->expr->kind == EXPR_CALL && strcmp(s->expr->name, "alloc") == 0) {
                        s->expr->elem_type = current_fn_ret_elem_type;
                        s->expr->elem_elem_type = current_fn_ret_elem_elem_type; /* M32 */
                    } else if (expr_elem_type_of(st, s->expr) != current_fn_ret_elem_type) {
                        sem_error("pointer return type mismatch (pointee types differ)", NULL);
                    } else if (current_fn_ret_elem_type == TYPE_PTR &&
                               expr_elem_elem_type_of(st, s->expr) != current_fn_ret_elem_elem_type) {
                        /* M32: `fn f(): int** { return pp; }` where pp is a
                           byte** — elem_type (TYPE_PTR) matches at the
                           first level, but the pointee-of-the-pointee still
                           needs to match. */
                        sem_error("pointer return type mismatch (pointee types differ)", NULL);
                    }
                }
            } else if (current_fn_ret_type != TYPE_VOID) {
                sem_error("missing return value for non-void function", NULL);
            }
            break;
        case STMT_EXPR:
            check_expr(st, s->expr);
            break;
        case STMT_IF: {
            TypeKind cond_type = check_expr(st, s->expr);
            if (cond_type != TYPE_BOOL) {
                sem_error("if condition must be bool", NULL);
            }
            check_block(st, s->then_body);
            if (s->else_body) {
                /* else_body is either a single STMT_IF (else-if chain) or a block's stmt list */
                if (s->else_body->kind == STMT_IF && s->else_body->next == NULL) {
                    check_stmt(st, s->else_body);
                } else {
                    check_block(st, s->else_body);
                }
            }
            break;
        }
        case STMT_ASSIGN: {
            int idx = sym_find(st, s->name);
            if (idx == -1) {
                sem_error("undefined identifier", s->name);
                break;
            }
            if (st->is_const[idx]) {
                sem_error("cannot assign to const", s->name);
                break;
            }
            TypeKind val_type = check_expr(st, s->expr);
            if (st->types[idx] == TYPE_PTR) {
                /* M26: mirrors STMT_VAR_DECL's TYPE_PTR handling — infer
                   alloc()'s elem_type from the assignment target, otherwise
                   require the value's elem_type to already match. */
                if (val_type != TYPE_PTR) {
                    sem_error("type mismatch in assignment", s->name);
                } else if (s->expr->kind == EXPR_CALL && strcmp(s->expr->name, "alloc") == 0) {
                    s->expr->elem_type = st->elem_types[idx];
                    s->expr->elem_elem_type = st->elem_elem_types[idx]; /* M32 */
                } else {
                    TypeKind val_elem = expr_elem_type_of(st, s->expr);
                    if (val_elem != st->elem_types[idx]) {
                        sem_error("pointer type mismatch in assignment (pointee types differ)", s->name);
                    } else if (val_elem == TYPE_PTR && expr_elem_elem_type_of(st, s->expr) != st->elem_elem_types[idx]) {
                        /* M32: `pp = other_pp;` where pp: int** and
                           other_pp: byte** — elem_type (TYPE_PTR) matches,
                           pointee-of-the-pointee doesn't. */
                        sem_error("pointer type mismatch in assignment (pointee types differ)", s->name);
                    }
                }
            } else if (val_type != st->types[idx]) {
                int compat = types_int_compatible(st->types[idx], val_type) ||
                             types_float_compatible(st->types[idx], val_type);
                if (!compat) {
                    sem_error("type mismatch in assignment", s->name);
                }
            }
            break;
        }
        case STMT_INDEX_ASSIGN: {
            TypeKind arr_type = check_expr(st, s->target);
            TypeKind idx_type = check_expr(st, s->index);
            TypeKind val_type = check_expr(st, s->value);
            if (arr_type != TYPE_INT_ARRAY && arr_type != TYPE_INT_LIST && arr_type != TYPE_BYTE_LIST && arr_type != TYPE_STRING) {
                sem_error("indexed assignment requires an array, list, or string", NULL);
            }
            if (idx_type != TYPE_INT) {
                sem_error("array index must be int", NULL);
            }
            if (arr_type == TYPE_STRING || arr_type == TYPE_BYTE_LIST) {
                /* M16/M20: s[i] = b / bs[i] = b — b must be a byte (int also
                   accepted, same byte/int compat rule used elsewhere). */
                if (!type_is_integer(val_type)) {
                    sem_error("string/byte-list index assignment requires a byte value", NULL);
                }
            } else if (val_type != element_type_of(arr_type)) {
                sem_error("type mismatch in indexed assignment", NULL);
            }
            break;
        }
        case STMT_DEREF_ASSIGN: {
            TypeKind target_type = check_expr(st, s->target);
            TypeKind val_type = check_expr(st, s->value);
            if (target_type != TYPE_PTR) {
                sem_error("dereference assignment requires a pointer", NULL);
                break;
            }
            TypeKind elem = expr_elem_type_of(st, s->target);
            /* M26/M32: value must be compatible with the pointee type —
               any integer width for an integer-family pointee, any float
               width for a float-family pointee, matching elem_type (and,
               if that's itself TYPE_PTR, matching elem_elem_type too) for
               a pointer-family pointee — e.g. `*pp = q;` where pp: T**
               requires q: T* with the same T. */
            int ok;
            if (type_is_integer(elem)) {
                ok = type_is_integer(val_type);
            } else if (type_is_float(elem)) {
                ok = type_is_float(val_type);
            } else if (elem == TYPE_PTR) {
                TypeKind target_elem2 = expr_elem_elem_type_of(st, s->target);
                ok = (val_type == TYPE_PTR && expr_elem_type_of(st, s->value) == target_elem2);
            } else {
                ok = (val_type == elem);
            }
            if (!ok) {
                sem_error("type mismatch in dereference assignment", NULL);
            }
            break;
        }
        case STMT_FIELD_ASSIGN: {
            /* M8: `p.f = expr;` — p must be a struct variable, f must exist
               and the value must match the field's type. */
            TypeKind base_type = check_expr(st, s->target);
            if (base_type != TYPE_STRUCT || s->target->kind != EXPR_IDENT) {
                sem_error("field assignment requires a struct variable", NULL);
                check_expr(st, s->value);
                break;
            }
            const char *sname = var_struct_map_get(s->target->name);
            StructDef *sd = sname ? struct_find(sname) : NULL;
            if (!sd) {
                sem_error("cannot determine struct type of variable", s->target->name);
                check_expr(st, s->value);
                break;
            }
            Field *fd = NULL;
            for (Field *f = sd->fields; f; f = f->next) {
                if (strcmp(f->name, s->field) == 0) { fd = f; break; }
            }
            if (!fd) {
                sem_error("no such field in struct", s->field);
                check_expr(st, s->value);
                break;
            }
            TypeKind val_type = check_expr(st, s->value);
            if (val_type != fd->type) {
                sem_error("type mismatch in field assignment", s->field);
            }
            break;
        }
        case STMT_INC:
        case STMT_DEC: {
            int idx = sym_find(st, s->name);
            if (idx == -1) {
                sem_error("undefined identifier", s->name);
                break;
            }
            if (st->is_const[idx]) {
                sem_error("cannot modify const", s->name);
                break;
            }
            if (!type_is_integer(st->types[idx])) {
                sem_error("++/-- requires an integer type", s->name);
            }
            break;
        }
        case STMT_WHILE: {
            TypeKind cond_type = check_expr(st, s->expr);
            if (cond_type != TYPE_BOOL) {
                sem_error("while condition must be bool", NULL);
            }
            semantic_loop_depth++;
            check_block(st, s->body);
            semantic_loop_depth--;
            break;
        }
        case STMT_FOR: {
            /* for's init variable is scoped to the whole for statement (init+cond+post+body) */
            int saved_count = st->count;
            check_stmt(st, s->init);
            TypeKind cond_type = check_expr(st, s->expr);
            if (cond_type != TYPE_BOOL) {
                sem_error("for condition must be bool", NULL);
            }
            check_stmt(st, s->post);
            semantic_loop_depth++;
            check_block(st, s->body);
            semantic_loop_depth--;
            st->count = saved_count;
            break;
        }
        case STMT_FOR_IN: {
            /* `for (x in xs)`: xs must be an array; x is bound to its element
               type, scoped to the loop body only. */
            TypeKind arr_type = check_expr(st, s->expr);
            if (arr_type != TYPE_INT_ARRAY && arr_type != TYPE_INT_LIST && arr_type != TYPE_BYTE_LIST) {
                sem_error("for-in requires an array or list", NULL);
            }
            int saved_count = st->count;
            sym_add(st, s->name, element_type_of(arr_type), 0 /* loop var is mutable */, NULL, TYPE_VOID, TYPE_VOID);
            semantic_loop_depth++;
            for (Stmt *bs = s->body; bs; bs = bs->next) {
                check_stmt(st, bs);
            }
            semantic_loop_depth--;
            st->count = saved_count;
            break;
        }
        case STMT_BREAK:
            if (semantic_loop_depth == 0) {
                sem_error("break outside of loop", NULL);
            }
            break;
        case STMT_CONTINUE:
            if (semantic_loop_depth == 0) {
                sem_error("continue outside of loop", NULL);
            }
            break;
        case STMT_SWITCH: {
            TypeKind subj_type = check_expr(st, s->expr);
            int saw_default = 0;
            for (CaseClause *c = s->cases; c; c = c->next) {
                if (c->value) {
                    TypeKind vt = check_expr(st, c->value);
                    if (vt != subj_type) {
                        sem_error("case value type must match switch subject", NULL);
                    }
                } else {
                    if (saw_default) {
                        sem_error("multiple default clauses in switch", NULL);
                    }
                    saw_default = 1;
                }
                /* Each case body is its own block scope; break exits the switch,
                   so it's treated like being inside a loop for that purpose. */
                semantic_loop_depth++;
                check_block(st, c->body);
                semantic_loop_depth--;
            }
            break;
        }
    }
}

static void check_function_body(Function *f) {
    SymTable st;
    st.count = 0;
    current_fn_ret_type = f->ret_type;
    current_fn_ret_elem_type = f->ret_elem_type; /* M26 */
    current_fn_ret_elem_elem_type = f->ret_elem_elem_type; /* M32 */
    for (Param *p = f->params; p; p = p->next) {
        sym_add(&st, p->name, p->type, 0 /* params are mutable locals */, NULL, p->elem_type, p->elem_elem_type);
    }
    for (Stmt *s = f->body; s; s = s->next) {
        check_stmt(&st, s);
    }
}

int semantic_check(Program *prog) {
    had_error = 0;
    semantic_loop_depth = 0;
    g_func_count = 0;

    if (!prog->main_fn) {
        sem_error("no main function found", NULL);
        return 0;
    }

    /* Pass 1: register every function's signature so calls (including
       forward references and recursion) can be checked regardless of
       definition order. `efn` functions are registered with a provisional
       TYPE_VOID ret_type here (their real declared type is unknown until
       their body expression is typed), then patched up to their inferred
       type below, before anything can observe/call them. This keeps EFN
       calling EFN (or regular functions, in any order) working exactly like
       regular function calls. */
    for (Function *f = prog->functions; f; f = f->next) {
        if (g_func_count >= MAX_FUNCS) {
            sem_error("too many functions (milestone limit)", f->name);
            continue;
        }
        if (func_find(f->name)) {
            sem_error("redeclaration of function", f->name);
            continue;
        }
        if (is_list_builtin(f->name) || is_sysprog_builtin(f->name) || is_conv_builtin(f->name)) {
            sem_error("cannot redefine builtin function", f->name);
            continue;
        }
        FuncSig *fs = &g_funcs[g_func_count++];
        fs->name = f->name;
        fs->ret_type = f->ret_type;
        fs->ret_elem_type = f->ret_elem_type; /* M26 */
        fs->ret_elem_elem_type = f->ret_elem_elem_type; /* M32 */
        fs->param_count = f->param_count;
        fs->is_variadic = f->is_variadic; /* M27 */
        int i = 0;
        for (Param *p = f->params; p; p = p->next, i++) {
            fs->param_types[i] = p->type;
            fs->param_elem_types[i] = p->elem_type; /* M26 */
            fs->param_elem_elem_types[i] = p->elem_elem_type; /* M32 */
        }
        /* M28: extern functions have no body to type-check in Pass 2 (that
           skip still applies below), but ARE now registered in the
           first-class-function registry same as user fn/efn — a bare
           reference to an extern's name (not immediately called) resolves
           as a TYPE_FN value and can be assigned/passed/called indirectly,
           same as any other function. Variadic externs (e.g. printf) can
           still be referenced this way, but an indirect call can only ever
           pass exactly the declared fixed parameters: TYPE_FN carries no
           signature payload once assigned to a variable, so there is no
           way for an indirect call site to also supply the `...` marker
           codegen needs for extra varargs (see EXPR_CALL_INDIRECT in
           codegen.c) — direct `printf(...)` calls are unaffected. */
        fnsig_register(f->name, f->ret_type, f->params, f->param_count);
        {
            FnSig *reg = fnsig_find(f->name);
            if (reg) {
                reg->ret_elem_type = f->ret_elem_type; /* M26 */
                reg->ret_elem_elem_type = f->ret_elem_elem_type; /* M32 */
            }
        }
        if (f->is_extern) continue;
    }

    /* Pass 1b: infer return types for `efn` functions now that every
       function's signature (including other EFNs) is visible, so an EFN's
       body expression can freely call other EFNs/functions regardless of
       source order. Patch both the AST node and its just-registered
       signature so pass 2's `return` check and any earlier/later callers
       agree on the real inferred type. */
    for (Function *f = prog->functions; f; f = f->next) {
        if (!f->is_efn) continue;
        SymTable st;
        st.count = 0;
        for (Param *p = f->params; p; p = p->next) {
            sym_add(&st, p->name, p->type, 0, NULL, p->elem_type, p->elem_elem_type);
        }
        TypeKind inferred = check_expr(&st, f->body->expr);
        f->ret_type = inferred;
        FuncSig *fs = func_find(f->name);
        if (fs) fs->ret_type = inferred;
        FnSig *fnsig = fnsig_find(f->name);
        if (fnsig) fnsig->ret_type = inferred;
    }
    if (g_func_count > 0 && strcmp(g_funcs[0].name, "main") != 0) {
        /* not an error by itself; main can be defined anywhere */
    }
    FuncSig *main_sig = func_find("main");
    if (main_sig && (main_sig->ret_type != TYPE_VOID || main_sig->param_count != 0)) {
        sem_error("main must take no parameters and return void", NULL);
    }

    /* Pass 2: check each function body. M27: extern functions have no body
       (they're a foreign declaration) — nothing to check. */
    for (Function *f = prog->functions; f; f = f->next) {
        if (f->is_extern) continue;
        check_function_body(f);
    }

    return !had_error;
}
