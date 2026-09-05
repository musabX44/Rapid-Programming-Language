%{
#include "ast.h"
#include <stdio.h>
#include <string.h>

int yylex(void);
void yyerror(const char *msg);

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

/* M27: set while reducing extern_param_list_opt when the param list ended
   in a trailing `, ...` (e.g. `extern fn printf(fmt: string, ...): int;`).
   Consumed immediately by the extern_decl action, same stash-then-consume
   convention as g_last_type_name/g_last_elem_type above. */
static int g_last_is_variadic = 0;
%}

%union {
    long num;
    double fnum;
    char *str;
    Expr *expr;
    Stmt *stmt;
    TypeKind type;
    ExprList *exprlist;
    Param *param;
    CaseClause *caseclause;
    StructLiteralField *structlit;
    Function *func;
    Field *field;
    EnumMember *enummember;
}

%token EXTERN ELLIPSIS LINK SHIM
%token FN EFN VAR CONST RETURN VOID INT_TYPE BOOL_TYPE STRING_TYPE BYTE_TYPE CHAR_TYPE
%token INT8_TYPE INT16_TYPE INT32_TYPE INT64_TYPE UINT8_TYPE UINT16_TYPE UINT32_TYPE UINT64_TYPE
%token FLOAT_TYPE DOUBLE_TYPE
%token STRUCT ENUM DOT LIST
%token IF ELSE WHILE FOR IN BREAK CONTINUE SWITCH CASE DEFAULT
%token IO COLONCOLON
%token USE AS PRIVATE
%token EQ NE LT LE GT GE INC DEC PLUS MINUS STAR SLASH AMPERSAND UMINUS
%token LPAREN RPAREN LBRACE RBRACE LBRACKET RBRACKET COLON SEMICOLON ASSIGN COMMA FATARROW

%token <num> INT_LITERAL CHAR_LITERAL
%token <fnum> FLOAT_LITERAL
%token <str> STRING_LITERAL
%token <str> IDENT

%nonassoc EQ NE LT LE GT GE
%left PLUS MINUS
%left STAR SLASH
%right UMINUS AMPERSAND
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

%type <expr> expr
%type <stmt> stmt stmt_list block else_clause for_init for_post case_body
%type <type> type base_type
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
        { $$ = param_new($1, $3, NULL); $$->elem_type = g_last_elem_type; g_last_is_variadic = 0; $$->elem_elem_type = g_last_elem_elem_type; }
    | IDENT COLON type COMMA extern_param_list
        { $$ = param_new($1, $3, $5); $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
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
    | FN IDENT LPAREN param_list_opt RPAREN COLON type LBRACE stmt_list RBRACE
        { $$ = function_new($2, $7, $4, $9); $$->ret_elem_type = g_last_elem_type; $$->ret_elem_elem_type = g_last_elem_elem_type; }
    | EFN IDENT LPAREN efn_param_list_opt RPAREN FATARROW expr SEMICOLON
        { $$ = efn_new($2, $4, $7); }
    | PRIVATE FN IDENT LPAREN param_list_opt RPAREN LBRACE stmt_list RBRACE
        { $$ = function_new($3, TYPE_VOID, $5, $8); }
    | PRIVATE FN IDENT LPAREN param_list_opt RPAREN COLON type LBRACE stmt_list RBRACE
        { $$ = function_new($3, $8, $5, $10); $$->ret_elem_type = g_last_elem_type; $$->ret_elem_elem_type = g_last_elem_elem_type; }
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
        { $$ = param_new($1, $3, NULL); $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
    | IDENT COMMA efn_param_list
        { $$ = param_new_untyped($1, $3); }
    | IDENT COLON type COMMA efn_param_list
        { $$ = param_new($1, $3, $5); $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
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
    IDENT COLON type { $$ = field_new($1, $3, NULL); $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
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
        { $$ = param_new($1, $3, NULL); $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
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
        COMMA param_list
        {
          $$ = param_new($1, $3, $7);
          $$->elem_type = $<type>4;
          $$->elem_elem_type = $<type>5;
        }
    ;

stmt_list:
      /* empty */             { $$ = NULL; }
    | stmt stmt_list          { $1->next = $2; $$ = $1; }
    ;

stmt:
      VAR IDENT COLON type ASSIGN expr SEMICOLON
        { $$ = stmt_new_var_decl($2, $4, $6); $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
    | VAR IDENT COLON type SEMICOLON
        { /* M8: initializer-less declaration (e.g. `var p: Point;`) */
          $$ = stmt_new_var_decl($2, $4, NULL); $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
    | CONST IDENT COLON type ASSIGN expr SEMICOLON
        { $$ = stmt_new_const_decl($2, $4, $6); $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
    | RETURN SEMICOLON
        { $$ = stmt_new_return(NULL); }
    | RETURN expr SEMICOLON
        { $$ = stmt_new_return($2); }
    | expr SEMICOLON
        { $$ = stmt_new_expr($1); }
    | IF LPAREN expr RPAREN block else_clause
        { $$ = stmt_new_if($3, $5, $6); }
    | WHILE LPAREN expr RPAREN block
        { $$ = stmt_new_while($3, $5); }
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
    | IDENT DOT IDENT ASSIGN expr SEMICOLON
        { $$ = stmt_new_field_assign(expr_new_ident($1), $3, $5); }
    | IDENT INC SEMICOLON
        { $$ = stmt_new_inc($1); }
    | IDENT DEC SEMICOLON
        { $$ = stmt_new_dec($1); }
    ;

for_init:
      VAR IDENT COLON type ASSIGN expr SEMICOLON
        { $$ = stmt_new_var_decl($2, $4, $6); $$->struct_name = g_last_type_name; $$->elem_type = g_last_elem_type; $$->elem_elem_type = g_last_elem_elem_type; }
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
    | INT_TYPE LBRACKET RBRACKET { $$ = TYPE_INT_ARRAY; g_last_type_name = NULL; g_last_elem_type = TYPE_VOID; g_last_elem_elem_type = TYPE_VOID; }
    | LIST LT INT_TYPE GT        { $$ = TYPE_INT_LIST; g_last_type_name = NULL; g_last_elem_type = TYPE_VOID; g_last_elem_elem_type = TYPE_VOID; }
    | LIST LT BYTE_TYPE GT       { $$ = TYPE_BYTE_LIST; g_last_type_name = NULL; g_last_elem_type = TYPE_VOID; g_last_elem_elem_type = TYPE_VOID; }
    | FN LPAREN type_list_opt RPAREN COLON type
        {
          /* M11: fn(...)::ret type annotation. No closures; just a bare
             function-pointer-like value. The specific param/return types
             aren't retained on TypeKind itself (it's a flat enum) — call
             sites are checked structurally against the actual function
             assigned, via FnSig, at the point of an indirect call. */
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
      type                  { $$ = param_new(NULL, $1, NULL); }
    | type COMMA type_list  { $$ = param_new(NULL, $1, $3); }
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
  | struct_literal_fields COMMA struct_literal_field { $$ = $1; $1->next = $3; }
;

struct_literal_field:
    IDENT COLON expr { $$ = struct_literal_field_new($1, $3, NULL); }
;

expr:
      INT_LITERAL              { $$ = expr_new_int($1); }
    | FLOAT_LITERAL            { $$ = expr_new_float($1); }
    | CHAR_LITERAL             { $$ = expr_new_char($1); }
    | STRING_LITERAL           { $$ = expr_new_string($1); }
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
            int n = 0;
            for (ExprList *l = $2; l; l = l->next) n++;
            $$ = expr_new_array_literal($2, n);
        }
    | expr LBRACKET expr RBRACKET %prec LBRACKET
        { $$ = expr_new_index($1, $3); }
    | AMPERSAND expr            { $$ = expr_new_addr_of($2); }
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
    | IDENT DOT IDENT { $$ = expr_new_field(expr_new_ident($1), $3); }
    | IDENT COLONCOLON IDENT { $$ = expr_new_enum_literal($1, $3); }
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
    | LPAREN expr RPAREN { $$ = $2; }
    ;

%%

void yyerror(const char *msg) {
    fprintf(stderr, "parse error: %s\n", msg);
}
