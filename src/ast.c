#include "ast.h"
#include "rapidc_limits.h"
#include <string.h>

Program g_program;

/* M42: konum takibi */
extern int yylineno;
const char *g_src_file = NULL;
int ast_cur_line(void) { return yylineno; }
const char *ast_cur_file(void) { return g_src_file; }

static StructDef *g_structs = NULL;
static EnumDef *g_enums = NULL;

/* Resets g_program and the parser's struct/enum lookup tables to their
   initial (empty) state, so a fresh yyparse() can build a clean AST
   without an embedder (LSP, watch-mode, build-server) seeing a prior
   run's leftover declarations or program contents.

   Deliberately does NOT free the AST nodes being discarded (functions,
   structs, enums, link_libs/link_objs, ...): they're allocated with plain
   calloc and nothing in this codebase tracks a master list independent of
   these chains to know they're unreferenced elsewhere, so walking and
   freeing them here would risk a use-after-free if anything outside this
   compile still holds a pointer into the old tree. A one-shot CLI process
   already leaks the whole AST to process exit; an embedder that calls this
   between compiles accepts the same per-compile leak in exchange for
   never freeing a tree something else might still be using. Should a
   future caller need the memory back, the safe fix is an arena allocator
   for the AST (free the whole arena here) rather than manual tree walks.

   Must run before yyparse() builds a new AST. */
void ast_reset(void) {
    memset(&g_program, 0, sizeof(g_program));
    g_structs = NULL;
    g_enums = NULL;
    g_src_file = NULL;
}
/* compiler_reset(), the single-call convenience wrapper for an embedder,
   lives in main.c: it needs module.h/semantic.h/codegen.h, which ast.c
   has no other reason to include, and main.c already includes all four
   reset-owning headers for its normal compile pipeline. */

Expr *expr_new_int(long v) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_INT_LITERAL;
    e->num = v;
    return e;
}

Expr *expr_new_float(double v) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_FLOAT_LITERAL;
    e->fnum = v;
    return e;
}

Expr *expr_new_char(long v) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_CHAR_LITERAL;
    e->num = v;
    return e;
}

Expr *expr_new_null(void) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_NULL_LITERAL;
    return e;
}

/* `true` / `false`. Reuses `num` (same slot EXPR_INT_LITERAL/EXPR_CHAR_LITERAL
   use), storing 1 or 0 — codegen just needs that raw value, same as any
   other integer-ish literal, and semantic.c types it as TYPE_BOOL. */
Expr *expr_new_bool(long val) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_BOOL_LITERAL;
    e->num = val;
    return e;
}

Expr *expr_new_string(char *s) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_STRING_LITERAL;
    e->str = s;
    return e;
}

Expr *expr_new_ident(char *name) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_IDENT;
    e->name = name;
    return e;
}

Expr *expr_new_io_out(Expr *arg) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_IO_OUT_CALL;
    e->arg = arg;
    return e;
}

Expr *expr_new_io_in(void) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_IO_IN_CALL;
    return e;
}

Expr *expr_new_io_format(Expr *fmt, ExprList *args) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_IO_FORMAT_CALL;
    e->lhs = fmt;
    e->args = args;
    return e;
}

Expr *expr_new_io_syscall(char *name, ExprList *args) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_IO_SYSCALL_CALL;
    e->name = name;
    e->args = args;
    return e;
}

Expr *expr_new_binop(BinOpKind op, Expr *lhs, Expr *rhs) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_BINOP;
    e->op = op;
    e->lhs = lhs;
    e->rhs = rhs;
    return e;
}

Expr *expr_new_array_literal(ExprList *elements, int count) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_ARRAY_LITERAL;
    e->elements = elements;
    e->elem_count = count;
    return e;
}

Expr *expr_new_index(Expr *arr, Expr *idx) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_INDEX;
    e->lhs = arr;
    e->rhs = idx;
    return e;
}

Expr *expr_new_call(char *name, ExprList *args) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_CALL;
    e->name = name;
    e->args = args;
    return e;
}

Expr *expr_new_deref(Expr *expr) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_DEREF;
    e->arg = expr;
    return e;
}

Expr *expr_new_addr_of(Expr *expr) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_ADDR_OF;
    e->arg = expr;
    return e;
}

/* M29: unary minus (-expr). Reuses `arg` (same slot EXPR_DEREF/EXPR_ADDR_OF
   use for their single operand) rather than adding a new Expr field. */
Expr *expr_new_neg(Expr *expr) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_NEG;
    e->arg = expr;
    return e;
}

/* logical negation (!expr). Reuses `arg`, same convention as EXPR_NEG. */
Expr *expr_new_not(Expr *expr) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_NOT;
    e->arg = expr;
    return e;
}

/* expr && expr, short-circuit. Reuses `lhs`/`rhs`, same slots EXPR_BINOP
   uses, since this is still fundamentally a two-operand node — it just
   needs its own ExprKind so codegen can emit branches instead of an
   eager instruction. */
Expr *expr_new_logand(Expr *lhs, Expr *rhs) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_LOGAND;
    e->lhs = lhs;
    e->rhs = rhs;
    return e;
}

Expr *expr_new_logor(Expr *lhs, Expr *rhs) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_LOGOR;
    e->lhs = lhs;
    e->rhs = rhs;
    return e;
}

Expr *expr_new_field(Expr *base, char *field) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_FIELD;
    e->lhs = base;
    e->name = field; /* reuse name field to store field name */
    return e;
}

Expr *expr_new_enum_literal(char *enum_name, char *member_name) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_ENUM_LITERAL;
    e->enum_name = enum_name;
    e->member_name = member_name;
    return e;
}

Expr *expr_new_struct_literal(char *struct_name, StructLiteralField *fields) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_STRUCT_LITERAL;
    e->name = struct_name; /* reuse name for struct name */
    e->struct_fields = fields;
    return e;
}

/* M11: bare function name used as a value. */
Expr *expr_new_func_ref(char *name) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_FUNC_REF;
    e->name = name;
    return e;
}

/* M11: `callee(args)` where callee is an arbitrary fn-typed expression
   (a variable/param, not a bare function name — those parse as a direct
   EXPR_CALL, unchanged). */
Expr *expr_new_call_indirect(Expr *callee, ExprList *args) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_CALL_INDIRECT;
    e->lhs = callee;
    e->args = args;
    return e;
}

ExprList *exprlist_new(Expr *e, ExprList *next) {
    ExprList *l = calloc(1, sizeof(ExprList));
    l->expr = e;
    l->next = next;
    return l;
}

Stmt *stmt_new_var_decl(char *name, TypeKind type, Expr *init) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_VAR_DECL;
    s->name = name;
    s->type = type;
    s->expr = init;
    return s;
}

Stmt *stmt_new_const_decl(char *name, TypeKind type, Expr *init) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_CONST_DECL;
    s->name = name;
    s->type = type;
    s->expr = init;
    return s;
}

Stmt *stmt_new_return(Expr *expr) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_RETURN;
    s->expr = expr;
    return s;
}

Stmt *stmt_new_expr(Expr *expr) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_EXPR;
    s->expr = expr;
    return s;
}

Stmt *stmt_new_if(Expr *cond, Stmt *then_body, Stmt *else_body) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_IF;
    s->expr = cond;
    s->then_body = then_body;
    s->else_body = else_body;
    return s;
}

Stmt *stmt_new_assign(char *name, Expr *value) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_ASSIGN;
    s->name = name;
    s->expr = value;
    return s;
}

Stmt *stmt_new_inc(char *name) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_INC;
    s->name = name;
    return s;
}

Stmt *stmt_new_dec(char *name) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_DEC;
    s->name = name;
    return s;
}

Stmt *stmt_new_while(Expr *cond, Stmt *body) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_WHILE;
    s->expr = cond;
    s->body = body;
    return s;
}

Stmt *stmt_new_do_while(Stmt *body, Expr *cond) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_DO_WHILE;
    s->expr = cond;
    s->body = body;
    return s;
}

Stmt *stmt_new_for(Stmt *init, Expr *cond, Stmt *post, Stmt *body) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_FOR;
    s->init = init;
    s->expr = cond;
    s->post = post;
    s->body = body;
    return s;
}

Stmt *stmt_new_for_in(char *var_name, Expr *array_expr, Stmt *body) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_FOR_IN;
    s->name = var_name;
    s->expr = array_expr;
    s->body = body;
    return s;
}

Stmt *stmt_new_break(void) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_BREAK;
    return s;
}

Stmt *stmt_new_continue(void) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_CONTINUE;
    return s;
}

Stmt *stmt_new_switch(Expr *subject, CaseClause *cases) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_SWITCH;
    s->expr = subject;
    s->cases = cases;
    return s;
}

Stmt *stmt_new_index_assign(Expr *target, Expr *index, Expr *value) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_INDEX_ASSIGN;
    s->target = target;
    s->index = index;
    s->value = value;
    return s;
}

Stmt *stmt_new_deref_assign(Expr *target, Expr *value) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_DEREF_ASSIGN;
    s->target = target;
    s->value = value;
    return s;
}

Stmt *stmt_new_field_assign(Expr *base, char *field, Expr *value) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_FIELD_ASSIGN;
    s->target = base;
    s->field = field;
    s->value = value;
    return s;
}

CaseClause *case_clause_new(Expr *value, Stmt *body, CaseClause *next) {
    CaseClause *c = calloc(1, sizeof(CaseClause));
    c->value = value;
    c->body = body;
    c->next = next;
    return c;
}

Param *param_new(char *name, TypeKind type, Param *next) {
    Param *p = calloc(1, sizeof(Param));
    p->name = name;
    p->type = type;
    p->next = next;
    return p;
}

Function *function_new(char *name, TypeKind ret_type, Param *params, Stmt *body) {
    Function *f = calloc(1, sizeof(Function));
    f->line = ast_cur_line(); f->file = ast_cur_file();
    f->name = name;
    f->ret_type = ret_type;
    f->params = params;
    int n = 0;
    for (Param *p = params; p; p = p->next) n++;
    f->param_count = n;
    f->body = body;
    return f;
}

Param *param_new_untyped(char *name, Param *next) {
    return param_new(name, TYPE_INT, next);
}

/* M27: extern fn declaration — body-less, is_extern=1. Reuses
   function_new's arity-counting so a variadic decl's fixed-param count
   (not including the trailing `...`) comes out right, since the ELLIPSIS
   grammar alternative never contributes a Param node. */
Function *function_new_extern(char *name, TypeKind ret_type, Param *params, int is_variadic) {
    Function *f = function_new(name, ret_type, params, NULL);
    f->is_extern = 1;
    f->is_variadic = is_variadic;
    return f;
}

/* M28: `link "name";` directive. Appends to g_program.link_libs preserving
   source order, but skips adding a duplicate if `name` was already
   requested (repeating `link "m";` in several imported files, or by hand,
   must not produce `-lm -lm` on the final cc command line). */
void link_lib_add(const char *name) {
    for (LinkLib *l = g_program.link_libs; l; l = l->next) {
        if (strcmp(l->name, name) == 0) return;
    }
    LinkLib *lib = calloc(1, sizeof(LinkLib));
    lib->name = strdup(name);
    lib->next = NULL;
    if (!g_program.link_libs) {
        g_program.link_libs = lib;
    } else {
        LinkLib *tail = g_program.link_libs;
        while (tail->next) tail = tail->next;
        tail->next = lib;
    }
}

/* M31: `shim "path.c";` directive. Mirrors link_lib_add exactly, just for
   a different list (link_objs instead of link_libs) — same de-dup /
   order-preserving append pattern. */
void link_obj_add(const char *abs_path) {
    for (LinkObj *o = g_program.link_objs; o; o = o->next) {
        if (strcmp(o->path, abs_path) == 0) return;
    }
    LinkObj *obj = calloc(1, sizeof(LinkObj));
    obj->path = strdup(abs_path);
    obj->next = NULL;
    if (!g_program.link_objs) {
        g_program.link_objs = obj;
    } else {
        LinkObj *tail = g_program.link_objs;
        while (tail->next) tail = tail->next;
        tail->next = obj;
    }
}

/* M10: EFN sugar -> plain Function with a single `return expr;` body. */
Function *efn_new(char *name, Param *params, Expr *body_expr) {
    Stmt *ret = stmt_new_return(body_expr);
    Function *f = function_new(name, TYPE_VOID /* placeholder, inferred later */, params, ret);
    f->is_efn = 1;
    return f;
}

/* M8: struct/enum helpers */

Field *field_new(char *name, TypeKind type, Field *next) {
    Field *f = calloc(1, sizeof(Field));
    f->name = name;
    f->type = type;
    f->offset = 0;
    f->next = next;
    return f;
}

StructDef *struct_def_new(char *name, Field *fields) {
    StructDef *sd = calloc(1, sizeof(StructDef));
    sd->name = name;
    sd->fields = fields;
    int offset = 0;
    for (Field *f = fields; f; f = f->next) {
        f->offset = offset;
        /* Bugfix: a struct-typed field is not 8 bytes like a scalar/
           pointer/enum field — it embeds the *entire* nested struct's
           bytes inline (see gen_struct_literal_into / emit_struct_copy_into,
           which byte-copy the full nested struct into the field's slot).
           Blindly adding a flat 8 here for every field caused a struct
           field to reserve only 8 bytes of the field that follows it to
           actually overlap the nested struct's own tail bytes whenever the
           nested struct was larger than 8 bytes (any struct with 2+
           fields) — corrupting any field declared after a nested struct
           field. Look up the nested struct's own (already-computed) size
           via struct_name, which the parser stashes onto struct-typed
           fields; struct definitions must precede their use as a field
           type, so the nested StructDef is always already registered in
           g_structs by the time this outer struct_def_new runs. */
        int field_size = 8;
        if (f->type == TYPE_STRUCT && f->struct_name) {
            StructDef *nested = struct_find(f->struct_name);
            if (nested) field_size = nested->size;
        }
        offset += field_size;
    }
    sd->size = offset;
    sd->next = g_structs;
    g_structs = sd;
    return sd;
}

StructDef *struct_find(const char *name) {
    for (StructDef *sd = g_structs; sd; sd = sd->next) {
        if (strcmp(sd->name, name) == 0) return sd;
    }
    return NULL;
}

StructLiteralField *struct_literal_field_new(char *field, Expr *value, StructLiteralField *next) {
    StructLiteralField *slf = calloc(1, sizeof(StructLiteralField));
    slf->field = field;
    slf->value = value;
    slf->next = next;
    return slf;
}

EnumMember *enum_member_new(char *name, int value, EnumMember *next) {
    EnumMember *m = calloc(1, sizeof(EnumMember));
    m->name = name;
    m->value = value;
    m->next = next;
    return m;
}

EnumDef *enum_def_new(char *name, EnumMember *members) {
    EnumDef *e = calloc(1, sizeof(EnumDef));
    e->name = name;
    e->members = members;
    int cur = 0;
    for (EnumMember *m = members; m; m = m->next) {
        if (m->value == -1) {
            m->value = cur;
        }
        cur = m->value + 1;
    }
    e->next = g_enums;
    g_enums = e;
    return e;
}

EnumDef *enum_find(const char *name) {
    for (EnumDef *e = g_enums; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

int enum_member_value(EnumDef *e, const char *member) {
    for (EnumMember *m = e->members; m; m = m->next) {
        if (strcmp(m->name, member) == 0) return m->value;
    }
    return 0; /* default fallback */
}

/* M11: first-class function signature registry. */
static FnSig *g_fnsigs = NULL;

void fnsig_register(const char *name, TypeKind ret_type, Param *params, int param_count) {
    FnSig *fs = calloc(1, sizeof(FnSig));
    fs->name = (char *)name;
    fs->ret_type = ret_type;
    fs->param_count = param_count > 16 ? 16 : param_count;
    int i = 0;
    for (Param *p = params; p && i < 16; p = p->next, i++) {
        fs->param_types[i] = p->type;
        fs->param_elem_types[i] = p->elem_type; /* M26 */
        fs->param_elem_elem_types[i] = p->elem_elem_type; /* M32 */
    }
    fs->next = g_fnsigs;
    g_fnsigs = fs;
}

FnSig *fnsig_find(const char *name) {
    for (FnSig *fs = g_fnsigs; fs; fs = fs->next) {
        if (strcmp(fs->name, name) == 0) return fs;
    }
    return NULL;
}

/* M23: fixed-width integer helpers (see ast.h for the rationale). */
int type_is_integer(TypeKind t) {
    switch (t) {
        case TYPE_INT: case TYPE_BYTE:
        case TYPE_INT8:  case TYPE_INT16:  case TYPE_INT32:  case TYPE_INT64:
        case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
            return 1;
        default:
            return 0;
    }
}

int type_int_width(TypeKind t) {
    switch (t) {
        case TYPE_BYTE:   case TYPE_INT8:  case TYPE_UINT8:  return 8;
        case TYPE_INT16:  case TYPE_UINT16: return 16;
        case TYPE_INT32:  case TYPE_UINT32: return 32;
        case TYPE_INT:    case TYPE_INT64: case TYPE_UINT64: return 64;
        default: return 0;
    }
}

int type_int_is_unsigned(TypeKind t) {
    switch (t) {
        case TYPE_BYTE:
        case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
            return 1;
        default:
            return 0;
    }
}

TypeKind type_int_promote(TypeKind a, TypeKind b) {
    if (a == b) return a;
    int wa = type_int_width(a), wb = type_int_width(b);
    if (wa != wb) return (wa > wb) ? a : b;
    /* same width, different signedness: unsigned wins (matches C) */
    if (type_int_is_unsigned(a)) return a;
    if (type_int_is_unsigned(b)) return b;
    return a; /* both signed, same width: identical semantics either way */
}

const char *type_kind_name(TypeKind t) {
    switch (t) {
        case TYPE_VOID: return "void";
        case TYPE_INT: return "int";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        case TYPE_INT_ARRAY_REMOVED_RESERVED: return "int[] (removed)";
        case TYPE_ARRAY_REMOVED_RESERVED: return "array (removed)"; /* M35: fixed-size arrays removed; see std/array.rapid */
        case TYPE_INT_LIST: return "list<int>";
        case TYPE_BYTE_LIST: return "list<byte>";
        case TYPE_STRUCT: return "struct";
        case TYPE_ENUM: return "enum";
        case TYPE_FN: return "fn(...)";
        case TYPE_BYTE: return "byte";
        case TYPE_INT8: return "int8";
        case TYPE_INT16: return "int16";
        case TYPE_INT32: return "int32";
        case TYPE_INT64: return "int64";
        case TYPE_UINT8: return "uint8";
        case TYPE_UINT16: return "uint16";
        case TYPE_UINT32: return "uint32";
        case TYPE_UINT64: return "uint64";
        case TYPE_FLOAT: return "float";
        case TYPE_DOUBLE: return "double";
        case TYPE_PTR: return "ptr"; /* M26: caller should prefer printing elem_type + "*" when available */
        default: return "?";
    }
}

/* M26/M32: true for every TypeKind that may be pointed to. */
int type_ptr_ok(TypeKind t) {
    switch (t) {
        case TYPE_INT: case TYPE_BOOL: case TYPE_BYTE:
        case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
        case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
        case TYPE_FLOAT: case TYPE_DOUBLE:
        case TYPE_STRUCT:
        case TYPE_PTR: /* M32: T** — pointer-to-pointer, one level deep */
            return 1;
        default:
            /* TYPE_VOID (no void*), TYPE_STRING, arrays/lists, TYPE_ENUM,
               TYPE_FN — not valid pointees. */
            return 0;
    }
}

/* M25: floating point helpers (see ast.h for the rationale). */
int type_is_float(TypeKind t) {
    return t == TYPE_FLOAT || t == TYPE_DOUBLE;
}

char type_qbe_class(TypeKind t) {
    if (t == TYPE_FLOAT) return 's';
    if (t == TYPE_DOUBLE) return 'd';
    return 'l';
}

/* ---- M43: hata yonetimi ------------------------------------------------ */
#define MAX_ERRORS RAPIDC_MAX_ERRORS
static char *g_error_names[MAX_ERRORS + 1]; /* indeks = kod; 0 kullanilmaz */
static int g_error_n = 0;

int error_code_intern(const char *name) {
    if (g_error_n == 0) {            /* kod 1 her zaman Unknown: `fail <0>` korumasi icin */
        g_error_names[++g_error_n] = strdup("Unknown");
    }
    for (int i = 1; i <= g_error_n; i++)
        if (strcmp(g_error_names[i], name) == 0) return i;
    if (g_error_n >= MAX_ERRORS) return 1;
    g_error_names[++g_error_n] = strdup(name);
    return g_error_n;
}
const char *error_name_of(int code) {
    return (code >= 1 && code <= g_error_n) ? g_error_names[code] : NULL;
}
int error_count(void) { (void)error_code_intern("Unknown"); return g_error_n; }

Expr *expr_new_try(Expr *call) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_TRY;
    e->arg = call;
    return e;
}
Expr *expr_new_catch(Expr *call, Expr *dflt) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_CATCH;
    e->lhs = call;
    e->rhs = dflt;
    return e;
}
Expr *expr_new_error_lit(char *name) {
    Expr *e = calloc(1, sizeof(Expr));
    e->line = ast_cur_line(); e->file = ast_cur_file();
    e->kind = EXPR_ERROR_LIT;
    e->name = name;
    e->num = error_code_intern(name);
    return e;
}
Stmt *stmt_new_fail(Expr *code) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->line = ast_cur_line(); s->file = ast_cur_file();
    s->kind = STMT_FAIL;
    s->expr = code;
    return s;
}
