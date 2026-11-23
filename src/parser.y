%{
#include "ast.h"
#include <stdio.h>
#include <string.h>
#include "diag.h" /* Rust-tarzi ayrintili tani ciktisi; bkz. yyerror() */

int yylex(void);
void yyerror(const char *msg);
extern int yylineno;
extern const char *g_src_file;
static int g_syntax_error_count = 0;
static int g_fn_fallible = 0, g_fail_saved = 0; /* M43 */

/* M8: source name of the most recently parsed `type` when it was a
   user-defined struct/enum identifier (NULL for builtin types). The var/const
   declaration actions read it right after their type is reduced to remember
   which struct a variable belongs to. */
static char *g_last_type_name = NULL;

/* M26: pointee type of the most recently parsed `type` when it was a
   pointer (`type STAR`). TYPE_VOID when the most recent type was not a
   pointer (mirrors g_last_type_name's NULL-when-not-applicable convention).
   Consumed the same way g_last_type_name is: immediately after a `type` is
   reduced, by whichever grammar action needs to stash it onto the AST node
   it just built (var/const decl, param, field, function return type). */
static TypeKind g_last_elem_type = TYPE_VOID;

/* M32: pointee-of-the-pointee of the most recently parsed `type` when it
   was a pointer-to-pointer (`base_type STAR STAR`). TYPE_VOID whenever
   g_last_elem_type != TYPE_PTR (mirrors g_last_elem_type's own
   not-applicable convention). Same stash-then-consume convention as
   g_last_type_name/g_last_elem_type. */
static TypeKind g_last_elem_elem_type = TYPE_VOID;

/* M40: en son parse edilen `fn(...): R` tipinin imzasi (TYPE_FN degilse anlamsiz;
   tuketen taraf yalniz tip TYPE_FN iken okur). */
static FnType *g_last_fn_type = NULL;

/* M27: set while reducing extern_param_list_opt when the param list ended
   in a trailing `, ...` (e.g. `extern fn printf(fmt: string, ...): int;`).
   Consumed immediately by the extern_decl action, same stash-then-consume
   convention as g_last_type_name/g_last_elem_type above. */
static int g_last_is_variadic = 0;

/* Compound-assignment desugaring helpers: `x OP= e` -> `x = x OP e`, etc.
   for the IDENT / IDENT[expr] / expr.IDENT target shapes. Kept as macros
   (not functions) since each needs the surrounding rule's own $$/$1../$n
   symbols and strdup'd copies of shared name/index/base pointers — same
   evaluate-more-than-once tradeoff already accepted by the plain four
   (+=, -=, *=, /=) compound-assign rules already in this grammar. */
#define ASSIGN_OP_IDENT(name, op, val) \
    stmt_new_assign((name), expr_new_binop((op), expr_new_ident(strdup(name)), (val)))
#define ASSIGN_OP_INDEX(name, idx, op, val) \
    stmt_new_index_assign(expr_new_ident(strdup(name)), (idx), \
        expr_new_binop((op), expr_new_index(expr_new_ident(strdup(name)), (idx)), (val)))
#define ASSIGN_OP_FIELD(base, field, op, val) \
    stmt_new_field_assign((base), (field), \
        expr_new_binop((op), expr_new_field((base), strdup(field)), (val)))
%}

%union {
    long num;
    double fnum;
    char *str;
    Expr *expr;
    Stmt *stmt;
    TypeKind type;
    BinOpKind opkind;
    ExprList *exprlist;
    Param *param;
    CaseClause *caseclause;
    StructLiteralField *structlit;
    Function *func;
    Field *field;
    EnumMember *enummember;
}

%token EXTERN ELLIPSIS LINK SHIM
%token TRY CATCH FAIL
%token <str> ERRNAME
%token FN EFN VAR CONST RETURN VOID NULLTOK INT_TYPE BOOL_TYPE STRING_TYPE BYTE_TYPE CHAR_TYPE
%token INT8_TYPE INT16_TYPE INT32_TYPE INT64_TYPE UINT8_TYPE UINT16_TYPE UINT32_TYPE UINT64_TYPE
%token FLOAT_TYPE DOUBLE_TYPE
%token STRUCT ENUM DOT LIST
%token IF ELSE WHILE DO FOR IN BREAK CONTINUE SWITCH CASE DEFAULT
%token IO COLONCOLON
%token USE AS PRIVATE
%token EQ NE LT LE GT GE INC DEC PLUS MINUS STAR SLASH AMPERSAND UMINUS
%token LPAREN RPAREN LBRACE RBRACE LBRACKET RBRACKET COLON SEMICOLON ASSIGN COMMA FATARROW
%token ANDAND OROR BANG PERCENT CARET PIPE SHL SHR POW
%token PLUSEQ MINUSEQ STAREQ SLASHEQ
%token PERCENTEQ AMPEQ PIPEEQ CARETEQ SHLEQ SHREQ POWEQ

%token <num> INT_LITERAL CHAR_LITERAL BOOL_LITERAL
%token <fnum> FLOAT_LITERAL
%token <str> STRING_LITERAL
%token <str> IDENT

%right ASSIGN
%left CATCH
%left OROR
%left ANDAND
%left PIPE
%left CARET
%left AMPERSAND
%nonassoc EQ NE LT LE GT GE
%left SHL SHR
%left PLUS MINUS
%left STAR SLASH PERCENT
%right UMINUS BANG
%right POW
/* Indexing (`expr[expr]`) is a postfix operator and must bind tighter than
   every other operator above it (including unary `*`/`&`), so `*p[i]` means
   `*(p[i])` and `a + b[i]` means `a + (b[i])`, matching normal C-like
   convention. Previously LBRACKET had no precedence at all here, which left
   `expr LBRACKET expr RBRACKET` to be resolved only by bison's blind
   shift-preferred default — it happened to produce the right parse, but
   silently, and any future grammar change could have flipped it. Declaring
   it explicitly (highest, above UMINUS/AMPERSAND) removes the ambiguity
   instead of relying on that default. */
%left LBRACKET
/* BUG-02/BUG-03 fix: `expr DOT IDENT` (nested field access, e.g. `o.inner.a`)
   is a postfix operator like `expr LBRACKET expr RBRACKET` above, and needs
   the same explicit precedence treatment for the same reason: left bare, it
   only reduces after a `%prec`-annotated attempt, `expr DOT IDENT` conflicts
   with every other `expr ... expr` binary rule because a DOT can always be
   read as extending the last expr just reduced (see `expr PLUS expr . DOT
   IDENT`-shaped conflicts). Declaring DOT here and above LBRACKET makes it
   bind tightest of all the postfix/binary operators (field access before
   indexing, e.g. `a.b[0]` and `a[0].b` both read the natural way), and
   resolves shift over reduce, which is what field-chaining needs anyway:
   `o.inner.a` must shift through both DOTs before reducing, not reduce
   `o.inner` early and get stuck. */
%left DOT

%type <expr> expr
%type <stmt> stmt stmt_list block else_clause for_init for_post case_body
%type <type> type base_type ret_type
%type <param> type_list type_list_opt
%type <exprlist> expr_list expr_list_opt
%type <param> param_list param_list_opt efn_param_list efn_param_list_opt
%type <caseclause> case_list
%type <func> function function_list
%type <func> struct_def enum_def
%type <func> use_decl extern_decl link_decl shim_decl
%type <param> extern_param_list_opt extern_param_list
%type <field> struct_field_list struct_field
%type <enummember> enum_member_list enum_member
%type <structlit> struct_literal_fields struct_literal_field
%type <opkind> compound_op

%%

program:
      function_list
        {
            g_program.functions = $1;
            g_program.main_fn = NULL;
            for (Function *f = $1; f; f = f->next) {
                if (strcmp(f->name, "main") == 0) g_program.main_fn = f;
            }
            if (!g_program.main_fn) {
                yyerror("no main function found");
            }
            if (g_syntax_error_count > 0) {
                /* Error recovery let us keep parsing past earlier syntax
                   errors to surface as many as possible, but the parse as
                   a whole must still be reported as failed once we reach
                   the end — YYABORT makes yyparse() return nonzero here,
                   same as it would have on an unrecovered error. */
                YYABORT;
            }
        }
    ;

/* M6: a program is one or more functions; builds the list in source order
   (right recursion + prepend-at-return would reverse it, so we thread it
   forward by returning the head and letting the caller attach `next`).
   M20: `use "file";` import directives are consumed here too. By the time
   the parser runs, the module resolver (main.c) has already inlined every
   imported file's content and rewritten private-symbol names to be
   file-unique, so a `use` line left in the merged source is purely
   documentation at this point and produces no AST node. */
function_list:
      function                    { $$ = $1; }
    | function function_list      { $1->next = $2; $$ = $1; }
    | struct_def function_list    { $$ = $2; }
    | enum_def function_list      { $$ = $2; }
    | use_decl function_list      { $$ = $2; }
    | extern_decl function_list   { $1->next = $2; $$ = $1; }
    | link_decl function_list     { $$ = $2; }
    | shim_decl function_list     { $$ = $2; }
    | error RBRACE function_list
        {
            /* Top-level error recovery: a malformed function/struct/enum/
               decl is skipped up to its closing `}` so the rest of the
               file is still parsed and can surface its own errors, instead
               of one bad top-level construct aborting the whole file. */
            $$ = $3;
        }
    | error SEMICOLON function_list
        {
            /* Same, for malformed decl-style top-level constructs that end
               in `;` instead of `}` (extern/link/shim/use). */
            $$ = $3;
        }
    ;

/* M27: `extern fn name(params[, ...]): ret;` — a foreign function
   declaration with no body, terminated by `;` instead of a `{ ... }`
   block. Always has an explicit return type (unlike plain `fn`, there's no
   "void by default" shorthand — foreign signatures should be spelled out
   in full since there's no body to infer anything from). void return is
   still expressible via `: void`. */
extern_decl:
      EXTERN FN IDENT LPAREN extern_param_list_opt RPAREN COLON type SEMICOLON
        {
            $$ = function_new_extern($3, $8, $5, g_last_is_variadic);
            $$->ret_elem_type = g_last_elem_type; $$->ret_elem_elem_type = g_last_elem_elem_type;
            $$->ret_struct_name = g_last_type_name;
            g_last_is_variadic = 0;
        }
    ;

/* Deliberately its own list (not a reuse of param_list_opt) so the
   `, ...` trailing-vararg marker only has to be threaded through this one
   grammar path — regular fn/efn params are never variadic. */
extern_param_list_opt:
      /* empty */                { $$ = NULL; g_last_is_variadic = 0; }
    | extern_param_list          { $$ = $1; }
    ;

extern_param_list:
      ELLIPSIS
        { $$ = NULL; g_last_is_variadic = 1; }
    | IDENT COLON type
        { $$ = param_new($1, $3, NULL); $$->fn_type = ($3 == TYPE_FN) ? g_last_fn_type : NULL; $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; g_last_is_variadic = 0; $$->elem_elem_type = g_last_elem_elem_type; }
    | IDENT COLON type
        {
          /* Same stash-before-recursing fix as param_list / efn_param_list:
             this param's pointee info must be captured right after `type`
             reduces, before the nested extern_param_list (to the right)
             reduces its own type(s) and overwrites g_last_elem_type /
             g_last_elem_elem_type. Without it every earlier pointer
             parameter of an `extern fn` (and of a `keyword`, which expands
             to one) silently inherited the LAST parameter's pointee info.
             Only the pointee slots are stashed here — g_last_is_variadic
             is deliberately left alone: it must keep the value the nested
             list (or its trailing `...`) set. */
          $<type>$ = g_last_elem_type;
        }
        {
          $<type>$ = g_last_elem_elem_type;
        }
        {
          $<str>$ = g_last_type_name; /* M36 */
        }
        {
          $<str>$ = (char *)g_last_fn_type; /* M40 */
        }
        COMMA extern_param_list
        {
          $$ = param_new($1, $3, $9);
          $$->elem_type = $<type>4;
          $$->elem_elem_type = $<type>5;
          $$->struct_name = $<str>6;
          $$->fn_type = ($3 == TYPE_FN) ? (FnType *)$<str>7 : NULL;
        }
    ;

use_decl:
      USE STRING_LITERAL SEMICOLON            { $$ = NULL; }
    | USE STRING_LITERAL AS IDENT SEMICOLON    { $$ = NULL; }
    ;

/* M28: `link "name";` — requests `-l<name>` on the final cc link command,
   so `extern fn` declarations can reach symbols outside libc (e.g.
   `link "m";` for libm's `sin`/`cos`/`sqrt`, `link "sqlite3";` for
   libsqlite3). Declaration-only, like use_decl: recorded immediately onto
   g_program (source order, de-duplicated) and contributes no further AST
   node — codegen never sees it, only main.c's link step does. */
link_decl:
      LINK STRING_LITERAL SEMICOLON
        { link_lib_add($2); $$ = NULL; }
    ;

/* M31: `shim "path.c";` — requests that the given C source file (path
   resolved relative to the declaring .rapid file, same convention as
   `use "...";`) be compiled once and its `.o` added to the final link
   command, so shim-backed FFI code no longer needs a manual `cc -c
   shim.c && rapidc ... -obj shim.o` two-step. The actual path resolution
   happens earlier, textually, in module.c's resolve_file (which has the
   declaring file's directory on hand); by the time the real Bison parser
   sees this token the directive has already been recorded onto
   g_program.link_objs, so this grammar rule is a pure no-op here — it
   exists only so a `shim "...";` line left in the merged source (module.c
   does not strip it) parses instead of producing a syntax error, exactly
   like `link_decl` did not need module.c to strip `link "...";` either. */
shim_decl:
      SHIM STRING_LITERAL SEMICOLON
        { $$ = NULL; }
    ;

function:
      FN IDENT LPAREN param_list_opt RPAREN LBRACE stmt_list RBRACE
        { $$ = function_new($2, TYPE_VOID, $4, $7); }
    | FN IDENT LPAREN param_list_opt RPAREN COLON ret_type { $<str>$ = g_last_type_name; g_fail_saved = g_fn_fallible; } LBRACE stmt_list RBRACE
        { $$ = function_new($2, $7, $4, $10); $$->can_fail = g_fail_saved; $$->ret_elem_type = g_last_elem_type; $$->ret_elem_elem_type = g_last_elem_elem_type; $$->ret_struct_name = $<str>8; }
    | EFN IDENT LPAREN efn_param_list_opt RPAREN FATARROW expr SEMICOLON
        { $$ = efn_new($2, $4, $7); }
    | PRIVATE FN IDENT LPAREN param_list_opt RPAREN LBRACE stmt_list RBRACE
        { $$ = function_new($3, TYPE_VOID, $5, $8); }
    | PRIVATE FN IDENT LPAREN param_list_opt RPAREN COLON ret_type { $<str>$ = g_last_type_name; g_fail_saved = g_fn_fallible; } LBRACE stmt_list RBRACE
        { $$ = function_new($3, $8, $5, $11); $$->can_fail = g_fail_saved; $$->ret_elem_type = g_last_elem_type; $$->ret_elem_elem_type = g_last_elem_elem_type; $$->ret_struct_name = $<str>9; }
    | PRIVATE EFN IDENT LPAREN efn_param_list_opt RPAREN FATARROW expr SEMICOLON
        { $$ = efn_new($3, $5, $8); }
    ;

/* M10: EFN parameters. Each parameter may be a bare name (`x`, defaults to
   int) or an explicitly typed one (`x: int`), matching normal fn syntax, so
   an EFN can still be typed precisely when that's needed. */
efn_param_list_opt:
      /* empty */         { $$ = NULL; }
    | efn_param_list       { $$ = $1; }
    ;

efn_param_list:
      IDENT
        { $$ = param_new_untyped($1, NULL); }
    | IDENT COLON type
        { $$ = param_new($1, $3, NULL); $$->fn_type = ($3 == TYPE_FN) ? g_last_fn_type : NULL; $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
    | IDENT COMMA efn_param_list
        { $$ = param_new_untyped($1, $3); }
    | IDENT COLON type
        {
          /* Same M32 fix `param_list` already carries, missing here until
             now: stash this param's pointee info in mid-rule slots right
             after `type` reduces, BEFORE the nested efn_param_list to the
             right is parsed — that nested parse reduces its own type(s)
             and clobbers g_last_elem_type/g_last_elem_elem_type, so the
             end-of-rule action would otherwise read the LAST parameter's
             pointee info for every earlier typed pointer parameter
             (e.g. `efn f(a: int*, b: byte*, n: int)` gave `a` a `byte`
             pointee). */
          $<type>$ = g_last_elem_type;
        }
        {
          $<type>$ = g_last_elem_elem_type;
        }
        {
          $<str>$ = g_last_type_name; /* M36 */
        }
        {
          $<str>$ = (char *)g_last_fn_type; /* M40 */
        }
        COMMA efn_param_list
        {
          $$ = param_new($1, $3, $9);
          $$->elem_type = $<type>4;
          $$->elem_elem_type = $<type>5;
          $$->struct_name = $<str>6;
          $$->fn_type = ($3 == TYPE_FN) ? (FnType *)$<str>7 : NULL;
        }
    ;

struct_def:
      STRUCT IDENT LBRACE struct_field_list RBRACE
        { struct_def_new($2, $4); $$ = NULL; }
    | PRIVATE STRUCT IDENT LBRACE struct_field_list RBRACE
        { struct_def_new($3, $5); $$ = NULL; }
    ;

/* M8: fields are separated by semicolons (`x: int; y: int;`), with an
   optional trailing separator; commas are accepted too for convenience. */
struct_field_list:
      /* empty */                            { $$ = NULL; }
    | struct_field struct_field_sep struct_field_list { $$ = $1; $1->next = $3; }
    ;

struct_field_sep:
      SEMICOLON
    | COMMA
    ;

struct_field:
    IDENT COLON type { $$ = field_new($1, $3, NULL); $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; $$->struct_name = g_last_type_name; /* BUG-02/BUG-03 fix: needed to resolve struct type of a nested struct-typed field, e.g. `inner: Inner;` */ }
;

enum_def:
      ENUM IDENT LBRACE enum_member_list RBRACE
        { enum_def_new($2, $4); $$ = NULL; }
    | PRIVATE ENUM IDENT LBRACE enum_member_list RBRACE
        { enum_def_new($3, $5); $$ = NULL; }
    ;

enum_member_list:
    enum_member
    | enum_member COMMA enum_member_list { $$ = $1; $1->next = $3; }
;

enum_member:
    IDENT { $$ = enum_member_new($1, -1, NULL); }
  | IDENT ASSIGN INT_LITERAL { $$ = enum_member_new($1, $3, NULL); }
;

param_list_opt:
      /* empty */    { $$ = NULL; }
    | param_list      { $$ = $1; }
    ;

param_list:
      IDENT COLON type
        { $$ = param_new($1, $3, NULL); $$->fn_type = ($3 == TYPE_FN) ? g_last_fn_type : NULL; $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
    | IDENT COLON type
        {
          /* M32 fix: stash immediately after `type` reduces (right here,
             via mid-rule actions), before the nested param_list to our
             right is parsed — that nested parse reduces its own `type`(s)
             and clobbers g_last_elem_type/g_last_elem_elem_type before the
             end-of-rule action below would otherwise run (bison reduces
             bottom-up/right-to-left for this right-recursive list),
             silently losing this param's pointee info whenever a pointer
             param is followed by another typed param. Stashing into a
             mid-rule $<type>$ slot (not $$, which would collide with the
             surrounding rule's own $$) sidesteps the ordering issue. */
          $<type>$ = g_last_elem_type;
        }
        {
          $<type>$ = g_last_elem_elem_type;
        }
        {
          $<str>$ = g_last_type_name; /* M36 */
        }
        {
          $<str>$ = (char *)g_last_fn_type; /* M40 */
        }
        COMMA param_list
        {
          $$ = param_new($1, $3, $9);
          $$->elem_type = $<type>4;
          $$->elem_elem_type = $<type>5;
          $$->struct_name = $<str>6;
          $$->fn_type = ($3 == TYPE_FN) ? (FnType *)$<str>7 : NULL;
        }
    ;

stmt_list:
      /* empty */             { $$ = NULL; }
    | stmt stmt_list          { $1->next = $2; $$ = $1; }
    | error SEMICOLON stmt_list
        {
            /* Statement-level error recovery: on a syntax error inside a
               block, discard tokens up to (and including) the next `;`
               and keep parsing the rest of the block, so one bad statement
               doesn't abort the whole file and hide every later error.
               The bad statement contributes no AST node. */
            $$ = $3;
        }
    ;

/* Compound-assignment operators shared by the deref-target (`/OP=`) and
   general-index-target (`expr[i] OP=`) forms below. One nonterminal (instead
   of 2×11 individual grammar rules) maps every compound-assign token to its
   desugaring BinOpKind; the rules fold them into the existing binop-based
   stmt shapes just like the plain `IDENT OP=` rules above already do. */
compound_op:
      PLUSEQ     { $$ = OP_ADD; }
    | MINUSEQ    { $$ = OP_SUB; }
    | STAREQ     { $$ = OP_MUL; }
    | SLASHEQ    { $$ = OP_DIV; }
    | PERCENTEQ  { $$ = OP_MOD; }
    | AMPEQ      { $$ = OP_BAND; }
    | PIPEEQ     { $$ = OP_BOR; }
    | CARETEQ    { $$ = OP_BXOR; }
    | SHLEQ      { $$ = OP_SHL; }
    | SHREQ      { $$ = OP_SHR; }
    | POWEQ      { $$ = OP_POW; }
    ;

stmt:
      VAR IDENT COLON type ASSIGN expr SEMICOLON
        { $$ = stmt_new_var_decl($2, $4, $6); $$->fn_type = ($4 == TYPE_FN) ? g_last_fn_type : NULL; $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
    | VAR IDENT COLON type SEMICOLON
        { /* M8: initializer-less declaration (e.g. `var p: Point;`) */
          $$ = stmt_new_var_decl($2, $4, NULL); $$->fn_type = ($4 == TYPE_FN) ? g_last_fn_type : NULL; $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
    | CONST IDENT COLON type ASSIGN expr SEMICOLON
        { $$ = stmt_new_const_decl($2, $4, $6); $$->fn_type = ($4 == TYPE_FN) ? g_last_fn_type : NULL; $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
    | RETURN SEMICOLON
        { $$ = stmt_new_return(NULL); }
    | RETURN expr SEMICOLON
        { $$ = stmt_new_return($2); }
    | FAIL expr SEMICOLON
        { $$ = stmt_new_fail($2); }
    | expr SEMICOLON
        { $$ = stmt_new_expr($1); }
    | IF LPAREN expr RPAREN block else_clause
        { $$ = stmt_new_if($3, $5, $6); }
    | WHILE LPAREN expr RPAREN block
        { $$ = stmt_new_while($3, $5); }
    | DO block WHILE LPAREN expr RPAREN SEMICOLON
        { $$ = stmt_new_do_while($2, $5); }
    | FOR LPAREN for_init expr SEMICOLON for_post RPAREN block
        { $$ = stmt_new_for($3, $4, $6, $8); }
    | FOR LPAREN IDENT IN expr RPAREN block
        { $$ = stmt_new_for_in($3, $5, $7); }
    | BREAK SEMICOLON
        { $$ = stmt_new_break(); }
    | CONTINUE SEMICOLON
        { $$ = stmt_new_continue(); }
    | SWITCH LPAREN expr RPAREN LBRACE case_list RBRACE
        { $$ = stmt_new_switch($3, $6); }
    | IDENT ASSIGN expr SEMICOLON
        { $$ = stmt_new_assign($1, $3); }
    | IDENT LBRACKET expr RBRACKET ASSIGN expr SEMICOLON
        { $$ = stmt_new_index_assign(expr_new_ident($1), $3, $6); }
    | STAR expr ASSIGN expr SEMICOLON
         { $$ = stmt_new_deref_assign($2, $4); }
    | STAR expr compound_op expr SEMICOLON
         { $$ = stmt_new_deref_assign($2, expr_new_binop($3, expr_new_deref($2), $4)); }
    | expr DOT IDENT ASSIGN expr SEMICOLON
         { $$ = stmt_new_field_assign($1, $3, $5); }
    | expr LBRACKET expr RBRACKET ASSIGN expr SEMICOLON
         { $$ = stmt_new_index_assign($1, $3, $6); }
    | expr LBRACKET expr RBRACKET compound_op expr SEMICOLON
         { $$ = stmt_new_index_assign($1, $3, expr_new_binop($5, expr_new_index($1, $3), $6)); }
    /* Compound assignment (+= -= *= /=): desugared right here into the
       plain-assignment AST shapes above (`x += e` -> `x = x + e`, etc.), so
       semantic.c and codegen.c need no new statement kinds at all — they
       just see an ordinary STMT_ASSIGN/STMT_INDEX_ASSIGN/STMT_FIELD_ASSIGN
       whose value happens to be a binop reading the same target back. The
       index/base sub-expression is duplicated (once for the read side of
       the binop, once for the write side); that's the same
       evaluate-more-than-once tradeoff already accepted implicitly by the
       language having no CSE, and mirrors how `x++`/`x--` are just sugar
       for their own dedicated INC/DEC statement kinds. */
    | IDENT PLUSEQ expr SEMICOLON
        { $$ = stmt_new_assign($1, expr_new_binop(OP_ADD, expr_new_ident(strdup($1)), $3)); }
    | IDENT MINUSEQ expr SEMICOLON
        { $$ = stmt_new_assign($1, expr_new_binop(OP_SUB, expr_new_ident(strdup($1)), $3)); }
    | IDENT STAREQ expr SEMICOLON
        { $$ = stmt_new_assign($1, expr_new_binop(OP_MUL, expr_new_ident(strdup($1)), $3)); }
    | IDENT SLASHEQ expr SEMICOLON
        { $$ = stmt_new_assign($1, expr_new_binop(OP_DIV, expr_new_ident(strdup($1)), $3)); }
    | IDENT LBRACKET expr RBRACKET PLUSEQ expr SEMICOLON
        { $$ = stmt_new_index_assign(expr_new_ident(strdup($1)), $3, expr_new_binop(OP_ADD, expr_new_index(expr_new_ident($1), $3), $6)); }
    | IDENT LBRACKET expr RBRACKET MINUSEQ expr SEMICOLON
        { $$ = stmt_new_index_assign(expr_new_ident(strdup($1)), $3, expr_new_binop(OP_SUB, expr_new_index(expr_new_ident($1), $3), $6)); }
    | IDENT LBRACKET expr RBRACKET STAREQ expr SEMICOLON
        { $$ = stmt_new_index_assign(expr_new_ident(strdup($1)), $3, expr_new_binop(OP_MUL, expr_new_index(expr_new_ident($1), $3), $6)); }
    | IDENT LBRACKET expr RBRACKET SLASHEQ expr SEMICOLON
        { $$ = stmt_new_index_assign(expr_new_ident(strdup($1)), $3, expr_new_binop(OP_DIV, expr_new_index(expr_new_ident($1), $3), $6)); }
    | expr DOT IDENT PLUSEQ expr SEMICOLON
        { $$ = stmt_new_field_assign($1, $3, expr_new_binop(OP_ADD, expr_new_field($1, strdup($3)), $5)); }
    | expr DOT IDENT MINUSEQ expr SEMICOLON
        { $$ = stmt_new_field_assign($1, $3, expr_new_binop(OP_SUB, expr_new_field($1, strdup($3)), $5)); }
    | expr DOT IDENT STAREQ expr SEMICOLON
        { $$ = stmt_new_field_assign($1, $3, expr_new_binop(OP_MUL, expr_new_field($1, strdup($3)), $5)); }
    | expr DOT IDENT SLASHEQ expr SEMICOLON
        { $$ = stmt_new_field_assign($1, $3, expr_new_binop(OP_DIV, expr_new_field($1, strdup($3)), $5)); }
    | IDENT PERCENTEQ expr SEMICOLON
        { $$ = ASSIGN_OP_IDENT($1, OP_MOD, $3); }
    | IDENT AMPEQ expr SEMICOLON
        { $$ = ASSIGN_OP_IDENT($1, OP_BAND, $3); }
    | IDENT PIPEEQ expr SEMICOLON
        { $$ = ASSIGN_OP_IDENT($1, OP_BOR, $3); }
    | IDENT CARETEQ expr SEMICOLON
        { $$ = ASSIGN_OP_IDENT($1, OP_BXOR, $3); }
    | IDENT SHLEQ expr SEMICOLON
        { $$ = ASSIGN_OP_IDENT($1, OP_SHL, $3); }
    | IDENT SHREQ expr SEMICOLON
        { $$ = ASSIGN_OP_IDENT($1, OP_SHR, $3); }
    | IDENT POWEQ expr SEMICOLON
        { $$ = ASSIGN_OP_IDENT($1, OP_POW, $3); }
    | IDENT LBRACKET expr RBRACKET PERCENTEQ expr SEMICOLON
        { $$ = ASSIGN_OP_INDEX($1, $3, OP_MOD, $6); }
    | IDENT LBRACKET expr RBRACKET AMPEQ expr SEMICOLON
        { $$ = ASSIGN_OP_INDEX($1, $3, OP_BAND, $6); }
    | IDENT LBRACKET expr RBRACKET PIPEEQ expr SEMICOLON
        { $$ = ASSIGN_OP_INDEX($1, $3, OP_BOR, $6); }
    | IDENT LBRACKET expr RBRACKET CARETEQ expr SEMICOLON
        { $$ = ASSIGN_OP_INDEX($1, $3, OP_BXOR, $6); }
    | IDENT LBRACKET expr RBRACKET SHLEQ expr SEMICOLON
        { $$ = ASSIGN_OP_INDEX($1, $3, OP_SHL, $6); }
    | IDENT LBRACKET expr RBRACKET SHREQ expr SEMICOLON
        { $$ = ASSIGN_OP_INDEX($1, $3, OP_SHR, $6); }
    | IDENT LBRACKET expr RBRACKET POWEQ expr SEMICOLON
        { $$ = ASSIGN_OP_INDEX($1, $3, OP_POW, $6); }
    | expr DOT IDENT PERCENTEQ expr SEMICOLON
        { $$ = ASSIGN_OP_FIELD($1, $3, OP_MOD, $5); }
    | expr DOT IDENT AMPEQ expr SEMICOLON
        { $$ = ASSIGN_OP_FIELD($1, $3, OP_BAND, $5); }
    | expr DOT IDENT PIPEEQ expr SEMICOLON
        { $$ = ASSIGN_OP_FIELD($1, $3, OP_BOR, $5); }
    | expr DOT IDENT CARETEQ expr SEMICOLON
        { $$ = ASSIGN_OP_FIELD($1, $3, OP_BXOR, $5); }
    | expr DOT IDENT SHLEQ expr SEMICOLON
        { $$ = ASSIGN_OP_FIELD($1, $3, OP_SHL, $5); }
    | expr DOT IDENT SHREQ expr SEMICOLON
        { $$ = ASSIGN_OP_FIELD($1, $3, OP_SHR, $5); }
    | expr DOT IDENT POWEQ expr SEMICOLON
        { $$ = ASSIGN_OP_FIELD($1, $3, OP_POW, $5); }
    | IDENT INC SEMICOLON
        { $$ = stmt_new_inc($1); }
    | IDENT DEC SEMICOLON
        { $$ = stmt_new_dec($1); }
    ;

for_init:
      VAR IDENT COLON type ASSIGN expr SEMICOLON
        { $$ = stmt_new_var_decl($2, $4, $6); $$->fn_type = ($4 == TYPE_FN) ? g_last_fn_type : NULL; $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
    ;

for_post:
      IDENT ASSIGN expr   { $$ = stmt_new_assign($1, $3); }
    | IDENT INC           { $$ = stmt_new_inc($1); }
    | IDENT DEC           { $$ = stmt_new_dec($1); }
    ;

/* M5: switch body is a sequence of `case <expr>:` / `default:` labels each
   followed by an (optionally empty, C-style fall-through) statement list. */
case_list:
      /* empty */
        { $$ = NULL; }
    | CASE expr COLON case_body case_list
        { $$ = case_clause_new($2, $4, $5); }
    | DEFAULT COLON case_body case_list
        { $$ = case_clause_new(NULL, $3, $4); }
    ;

case_body:
      /* empty: falls through to next case */
        { $$ = NULL; }
    | stmt case_body
        { $1->next = $2; $$ = $1; }
    ;

block:
      LBRACE stmt_list RBRACE   { $$ = $2; }
    ;

else_clause:
      /* empty */                       { $$ = NULL; }
    | ELSE IF LPAREN expr RPAREN block else_clause
        { $$ = stmt_new_if($4, $6, $7); }
    | ELSE block
        { $$ = $2; }
    ;

ret_type:
      type         { g_fn_fallible = 0; $$ = $1; }
    | BANG type    { g_fn_fallible = 1; $$ = $2; }
    ;

type:
      base_type                   { $$ = $1; g_last_elem_type = TYPE_VOID; g_last_elem_elem_type = TYPE_VOID; }
    | base_type STAR
        {
          /* M26: generalized pointer type — any base_type followed by `*`,
             replacing the old hardcoded INT_TYPE STAR / BYTE_TYPE STAR /
             CHAR_TYPE STAR rules with one rule that covers every pointee
             type at once (int8..uint64, float, double, bool, struct, enum,
             plus the original int/byte/char). void* still parses (VOID is
             a base_type) but is rejected in semantic.c via
             type_ptr_ok(TYPE_VOID) == false, since excluding it here would
             mean duplicating this whole rule. */
          $$ = TYPE_PTR;
          g_last_elem_type = $1;
          g_last_elem_elem_type = TYPE_VOID;
          g_last_type_name = NULL;
        }
    | base_type STAR STAR
        {
          /* M32: pointer-to-pointer — `base_type` followed by `**`. Kept as
             its own rule (rather than generalizing `type STAR`) so T*** and
             beyond remain a grammar error, same deliberate one-level-at-a-
             time scoping the base_type-STAR rule above already used going
             from flat types to T*. $1 is the base_type token itself (the
             ultimate pointee, e.g. INT_TYPE -> TYPE_INT), so g_last_elem_type
             is TYPE_PTR (this expr's immediate pointee is itself a pointer)
             and g_last_elem_elem_type is $1 (what that inner pointer points
             to). void** still parses but is rejected the same way void* is,
             since void isn't type_ptr_ok(). */
          $$ = TYPE_PTR;
          g_last_elem_type = TYPE_PTR;
          g_last_elem_elem_type = $1;
          g_last_type_name = NULL;
        }
    /* M35-arraylib: `base_type LBRACKET RBRACKET` (`int[]`, `byte[]`, ...) —
       the fixed-size array *type* — has been removed. Fixed-size arrays are
       no longer a language type; see std/array.rapid for the replacement.
       `list<int>`/`list<byte>` below are unaffected. */
    | LIST LT INT_TYPE GT        { $$ = TYPE_INT_LIST; g_last_type_name = NULL; g_last_elem_type = TYPE_VOID; g_last_elem_elem_type = TYPE_VOID; }
    | LIST LT BYTE_TYPE GT       { $$ = TYPE_BYTE_LIST; g_last_type_name = NULL; g_last_elem_type = TYPE_VOID; g_last_elem_elem_type = TYPE_VOID; }
    | FN LPAREN type_list_opt RPAREN COLON type
        {
          /* M11: fn(...)::ret type annotation. No closures; just a bare
             function-pointer-like value. The specific param/return types
             aren't retained on TypeKind itself (it's a flat enum) — call
             sites are checked structurally against the actual function
             assigned, via FnSig, at the point of an indirect call. */
          {
            FnType *ft = calloc(1, sizeof(FnType));
            int n = 0;
            for (Param *pp = $3; pp; pp = pp->next, n++) {
                if (n < 8) { ft->pt[n] = pp->type; ft->pe[n] = pp->elem_type; ft->pn[n] = pp->struct_name; }
                else ft->unchecked = 1;
            }
            ft->param_count = n;
            ft->ret = $6;
            ft->ret_elem = g_last_elem_type;
            ft->ret_name = g_last_type_name;
            g_last_fn_type = ft;
          }
          $$ = TYPE_FN;
          g_last_type_name = NULL;
          g_last_elem_type = TYPE_VOID;
          g_last_elem_elem_type = TYPE_VOID;
        }
    ;

/* M26: split out of `type` so `base_type STAR` can express a pointer to
   any of these without a separate hardcoded rule per pointee type. Every
   non-pointer `type` alternative that isn't array/list/fn is a base_type. */
base_type:
      INT_TYPE                  { $$ = TYPE_INT; g_last_type_name = NULL; }
    | BOOL_TYPE                 { $$ = TYPE_BOOL; g_last_type_name = NULL; }
    | STRING_TYPE                { $$ = TYPE_STRING; g_last_type_name = NULL; }
    | VOID                       { $$ = TYPE_VOID; g_last_type_name = NULL; }
    | BYTE_TYPE                  { $$ = TYPE_BYTE; g_last_type_name = NULL; }
    | CHAR_TYPE                  { $$ = TYPE_BYTE; g_last_type_name = NULL; } /* M16: char is an alias for byte */
    | INT8_TYPE                  { $$ = TYPE_INT8; g_last_type_name = NULL; }   /* M23 */
    | INT16_TYPE                 { $$ = TYPE_INT16; g_last_type_name = NULL; }
    | INT32_TYPE                 { $$ = TYPE_INT32; g_last_type_name = NULL; }
    | INT64_TYPE                 { $$ = TYPE_INT64; g_last_type_name = NULL; }
    | UINT8_TYPE                 { $$ = TYPE_UINT8; g_last_type_name = NULL; }
    | UINT16_TYPE                { $$ = TYPE_UINT16; g_last_type_name = NULL; }
    | UINT32_TYPE                { $$ = TYPE_UINT32; g_last_type_name = NULL; }
    | UINT64_TYPE                { $$ = TYPE_UINT64; g_last_type_name = NULL; }
    | FLOAT_TYPE                 { $$ = TYPE_FLOAT; g_last_type_name = NULL; }   /* M25 */
    | DOUBLE_TYPE                { $$ = TYPE_DOUBLE; g_last_type_name = NULL; }  /* M25 */
    | IDENT
        {
          /* M8: user-defined struct/enum type name */
          if (struct_find($1)) {
              $$ = TYPE_STRUCT;
          } else if (enum_find($1)) {
              $$ = TYPE_ENUM;
          } else {
              yyerror("unknown type name");
              $$ = TYPE_INT;
          }
          g_last_type_name = $1;
        }
    ;

type_list_opt:
      /* empty */    { $$ = NULL; }
    | type_list        { $$ = $1; }
    ;

type_list:
      type                  { $$ = param_new(NULL, $1, NULL); $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; }
    | type { $<str>$ = g_last_type_name; } { $<type>$ = g_last_elem_type; } COMMA type_list
        { $$ = param_new(NULL, $1, $5); $$->struct_name = $<str>2; $$->elem_type = $<type>3; }
    ;

expr_list_opt:
      /* empty */   { $$ = NULL; }
    | expr_list      { $$ = $1; }
    ;

expr_list:
      expr                     { $$ = exprlist_new($1, NULL); }
    | expr COMMA expr_list     { $$ = exprlist_new($1, $3); }
    ;

struct_literal_fields:
    /* empty */ { $$ = NULL; }
  | struct_literal_field
  | struct_literal_fields COMMA struct_literal_field {
        $$ = $1;
        StructLiteralField *tail = $1;
        while (tail->next) tail = tail->next;
        tail->next = $3;
    }
;

struct_literal_field:
    IDENT COLON expr { $$ = struct_literal_field_new($1, $3, NULL); }
;

expr:
      INT_LITERAL              { $$ = expr_new_int($1); }
    | FLOAT_LITERAL            { $$ = expr_new_float($1); }
    | CHAR_LITERAL             { $$ = expr_new_char($1); }
    | STRING_LITERAL           { $$ = expr_new_string($1); }
    | NULLTOK                  { $$ = expr_new_null(); }
    | BOOL_LITERAL             { $$ = expr_new_bool($1); }
    | IDENT                    { $$ = expr_new_ident($1); }
    | IO COLONCOLON IDENT LPAREN expr_list_opt RPAREN
        {
            /* M16/M17/M18: io::out(...), io::in(), and the io:: file
               syscall wrappers (open/read/write/close).
               - io::out(x)        -> plain print (unchanged behavior)
               - io::out(fmt, ...) -> "{}"-templated formatted print
               - io::in()          -> read one line from stdin, as a string
               - io::open/read/write/close -> thin raw-syscall wrappers,
                 argument/type checking done in semantic.c like the
                 sysprog:: builtins. */
            if (strcmp($3, "out") == 0) {
                int argc = 0;
                for (ExprList *l = $5; l; l = l->next) argc++;
                if (argc == 0) {
                    yyerror("io::out requires at least one argument");
                    $$ = expr_new_io_out(expr_new_int(0));
                } else if (argc == 1) {
                    $$ = expr_new_io_out($5->expr);
                } else {
                    $$ = expr_new_io_format($5->expr, $5->next);
                }
            } else if (strcmp($3, "open") == 0 || strcmp($3, "read") == 0 ||
                       strcmp($3, "write") == 0 || strcmp($3, "close") == 0) {
                $$ = expr_new_io_syscall($3, $5);
            } else if (strcmp($3, "errno") == 0) {
                if ($5 != NULL) {
                    yyerror("io::errno takes no arguments");
                }
                $$ = expr_new_io_syscall($3, NULL);
            } else {
                yyerror("only io::out, io::in, io::open, io::read, io::write, io::close, io::errno are supported so far");
                $$ = expr_new_io_in();
            }
        }
    | IO COLONCOLON IN LPAREN expr_list_opt RPAREN
        {
            /* `in` is a reserved keyword (for-in), so it can't come through
               as an IDENT here the way "out" does — needs its own rule. */
            if ($5 != NULL) {
                yyerror("io::in takes no arguments");
            }
            $$ = expr_new_io_in();
        }
    | IDENT LPAREN expr_list_opt RPAREN
        { $$ = expr_new_call($1, $3); }
    | LBRACKET expr_list_opt RBRACKET
        {
            /* M35-arraylib note: this `[e0, e1, ...]` literal is kept only
               as the initializer syntax for `list<int> xs = [1,2,3];` /
               `list<byte> bs = [];` (see semantic.c) — it no longer denotes
               a standalone fixed-size-array value anywhere else. */
            int n = 0;
            for (ExprList *l = $2; l; l = l->next) n++;
            $$ = expr_new_array_literal($2, n);
        }
    | expr LBRACKET expr RBRACKET %prec LBRACKET
        { $$ = expr_new_index($1, $3); }
    | AMPERSAND expr %prec UMINUS  { $$ = expr_new_addr_of($2); }
    | STAR expr %prec UMINUS    { $$ = expr_new_deref($2); }
    | MINUS expr %prec UMINUS
        {
            /* Unary minus (`-5`, `-5.5`, `-x`) was entirely missing from
               the grammar before this fix: `expr` only had a binary
               `expr MINUS expr` rule, so a leading `-` in expression
               position (a negative literal in a var/const initializer,
               a bare `-x`, `fabs(-5.5)`, etc.) was a syntax error.
               Represented as its own EXPR_NEG node (see ast.h) rather than
               desugared to `0 - expr`, because that desugar's `0` is
               always int-typed and semantic.c's binop check rejects
               mixing an int literal with a float operand
               (types_float_compatible requires both sides already
               float-typed) — EXPR_NEG instead types itself as exactly its
               operand's type. Declared %prec UMINUS (highest, right-assoc,
               same precedence STAR-deref already uses above) so `-a * b`
               binds as `(-a) * b` and `-a + b` as `(-a) + b`, matching
               normal unary-minus precedence instead of MINUS's low binary
               precedence. */
            $$ = expr_new_neg($2);
        }
    | expr DOT IDENT %prec DOT { $$ = expr_new_field($1, $3); }
    | IDENT COLONCOLON IDENT { $$ = expr_new_enum_literal($1, $3); }
    /* M35 note: multi-level qualified calls (`ns::sub::fn(args)`, e.g.
       usb::interface::claim(dev)) are no longer handled here. They're
       now resolved for real, before the parser ever runs, by module.c's
       subalias-table walk (see resolve_file/record_subalias/
       lookup_subalias/replace_alias_calls in module.c): each `::segment`
       is checked against the actual `use ... as alias;` imports of the
       module it appears in, and the whole chain collapses to the single
       real mangled function name (e.g. `interface__claim`) that a plain
       call already knows how to parse. That's a genuine resolution
       against the module graph - it fails at semantic-check time if the
       chain doesn't correspond to a real nested import (see the
       private-function-leak test), unlike the old grammar rule this
       replaced, which just string-joined however many idents preceded
       `(` into one literal callee name with no relation to the module
       system at all. Any chain deeper than two levels (`a::b::c::d`)
       is handled the same way - the walk isn't limited to one hop. */
    | IDENT LBRACE struct_literal_fields RBRACE { $$ = expr_new_struct_literal($1, $3); }
    | expr EQ expr   { $$ = expr_new_binop(OP_EQ, $1, $3); }
    | expr NE expr   { $$ = expr_new_binop(OP_NE, $1, $3); }
    | expr LT expr   { $$ = expr_new_binop(OP_LT, $1, $3); }
    | expr LE expr   { $$ = expr_new_binop(OP_LE, $1, $3); }
    | expr GT expr   { $$ = expr_new_binop(OP_GT, $1, $3); }
    | expr GE expr   { $$ = expr_new_binop(OP_GE, $1, $3); }
    | expr PLUS expr  { $$ = expr_new_binop(OP_ADD, $1, $3); }
    | expr MINUS expr { $$ = expr_new_binop(OP_SUB, $1, $3); }
    | expr STAR expr  { $$ = expr_new_binop(OP_MUL, $1, $3); }
    | expr SLASH expr { $$ = expr_new_binop(OP_DIV, $1, $3); }
    | expr PERCENT expr { $$ = expr_new_binop(OP_MOD, $1, $3); }
    | expr AMPERSAND expr { $$ = expr_new_binop(OP_BAND, $1, $3); }
    | expr PIPE expr     { $$ = expr_new_binop(OP_BOR, $1, $3); }
    | expr CARET expr    { $$ = expr_new_binop(OP_BXOR, $1, $3); }
    | expr SHL expr      { $$ = expr_new_binop(OP_SHL, $1, $3); }
    | expr SHR expr      { $$ = expr_new_binop(OP_SHR, $1, $3); }
    | expr POW expr
        {
            /* Power spelled `^^` rather than the more conventional `**`:
               `**pp` (double-dereference of a T** pointer, an established
               M32 feature — `*(*pp)`) already means something in this
               grammar, and a greedy `**` lexer token would swallow that
               prefix double-star before the unary-deref rule ever saw it,
               silently breaking every existing pointer-to-pointer
               dereference (confirmed: made ptrptr_test.rapid stop parsing
               entirely). Declaring `expr STAR STAR expr` as a binary rule
               instead doesn't work either — it's genuinely ambiguous with
               `expr STAR expr` (binary multiply) followed by a unary
               `STAR expr` (deref), which bison can't resolve at the LALR
               level (confirmed via -Wcounterexamples: 25 reduce/reduce
               conflicts, rule reported unreachable). `^^` sidesteps both:
               it doesn't share a prefix with any existing operator. */
            $$ = expr_new_binop(OP_POW, $1, $3);
        }
    | expr ANDAND expr   { $$ = expr_new_logand($1, $3); }
    | expr OROR expr     { $$ = expr_new_logor($1, $3); }
    | BANG expr %prec BANG { $$ = expr_new_not($2); }
    | TRY expr %prec UMINUS { $$ = expr_new_try($2); }
    | expr CATCH expr { $$ = expr_new_catch($1, $3); }
    | ERRNAME { $$ = expr_new_error_lit($1); }
    | LPAREN expr RPAREN { $$ = $2; }
    ;

%%

void yyerror(const char *msg) {
    /* Rust-tarzi ayrintili tani (bkz. diag.h); eski "duz" formatla ayni
       baslik metnini kullanir, ustune kaynak satirinin cercevelenmis
       gorunumunu ve (varsa) bir help ipucunu ekler. */
    diag_emit("parse error", g_src_file, yylineno, 0, msg, NULL, diag_find_help(msg));
    g_syntax_error_count++;
}
