/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 1 "src/parser.y"

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

#line 107 "src/parser.tab.c"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "parser.tab.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_EXTERN = 3,                     /* EXTERN  */
  YYSYMBOL_ELLIPSIS = 4,                   /* ELLIPSIS  */
  YYSYMBOL_LINK = 5,                       /* LINK  */
  YYSYMBOL_SHIM = 6,                       /* SHIM  */
  YYSYMBOL_FN = 7,                         /* FN  */
  YYSYMBOL_EFN = 8,                        /* EFN  */
  YYSYMBOL_VAR = 9,                        /* VAR  */
  YYSYMBOL_CONST = 10,                     /* CONST  */
  YYSYMBOL_RETURN = 11,                    /* RETURN  */
  YYSYMBOL_VOID = 12,                      /* VOID  */
  YYSYMBOL_INT_TYPE = 13,                  /* INT_TYPE  */
  YYSYMBOL_BOOL_TYPE = 14,                 /* BOOL_TYPE  */
  YYSYMBOL_STRING_TYPE = 15,               /* STRING_TYPE  */
  YYSYMBOL_BYTE_TYPE = 16,                 /* BYTE_TYPE  */
  YYSYMBOL_CHAR_TYPE = 17,                 /* CHAR_TYPE  */
  YYSYMBOL_INT8_TYPE = 18,                 /* INT8_TYPE  */
  YYSYMBOL_INT16_TYPE = 19,                /* INT16_TYPE  */
  YYSYMBOL_INT32_TYPE = 20,                /* INT32_TYPE  */
  YYSYMBOL_INT64_TYPE = 21,                /* INT64_TYPE  */
  YYSYMBOL_UINT8_TYPE = 22,                /* UINT8_TYPE  */
  YYSYMBOL_UINT16_TYPE = 23,               /* UINT16_TYPE  */
  YYSYMBOL_UINT32_TYPE = 24,               /* UINT32_TYPE  */
  YYSYMBOL_UINT64_TYPE = 25,               /* UINT64_TYPE  */
  YYSYMBOL_FLOAT_TYPE = 26,                /* FLOAT_TYPE  */
  YYSYMBOL_DOUBLE_TYPE = 27,               /* DOUBLE_TYPE  */
  YYSYMBOL_STRUCT = 28,                    /* STRUCT  */
  YYSYMBOL_ENUM = 29,                      /* ENUM  */
  YYSYMBOL_DOT = 30,                       /* DOT  */
  YYSYMBOL_LIST = 31,                      /* LIST  */
  YYSYMBOL_IF = 32,                        /* IF  */
  YYSYMBOL_ELSE = 33,                      /* ELSE  */
  YYSYMBOL_WHILE = 34,                     /* WHILE  */
  YYSYMBOL_FOR = 35,                       /* FOR  */
  YYSYMBOL_IN = 36,                        /* IN  */
  YYSYMBOL_BREAK = 37,                     /* BREAK  */
  YYSYMBOL_CONTINUE = 38,                  /* CONTINUE  */
  YYSYMBOL_SWITCH = 39,                    /* SWITCH  */
  YYSYMBOL_CASE = 40,                      /* CASE  */
  YYSYMBOL_DEFAULT = 41,                   /* DEFAULT  */
  YYSYMBOL_IO = 42,                        /* IO  */
  YYSYMBOL_COLONCOLON = 43,                /* COLONCOLON  */
  YYSYMBOL_USE = 44,                       /* USE  */
  YYSYMBOL_AS = 45,                        /* AS  */
  YYSYMBOL_PRIVATE = 46,                   /* PRIVATE  */
  YYSYMBOL_EQ = 47,                        /* EQ  */
  YYSYMBOL_NE = 48,                        /* NE  */
  YYSYMBOL_LT = 49,                        /* LT  */
  YYSYMBOL_LE = 50,                        /* LE  */
  YYSYMBOL_GT = 51,                        /* GT  */
  YYSYMBOL_GE = 52,                        /* GE  */
  YYSYMBOL_INC = 53,                       /* INC  */
  YYSYMBOL_DEC = 54,                       /* DEC  */
  YYSYMBOL_PLUS = 55,                      /* PLUS  */
  YYSYMBOL_MINUS = 56,                     /* MINUS  */
  YYSYMBOL_STAR = 57,                      /* STAR  */
  YYSYMBOL_SLASH = 58,                     /* SLASH  */
  YYSYMBOL_AMPERSAND = 59,                 /* AMPERSAND  */
  YYSYMBOL_UMINUS = 60,                    /* UMINUS  */
  YYSYMBOL_LPAREN = 61,                    /* LPAREN  */
  YYSYMBOL_RPAREN = 62,                    /* RPAREN  */
  YYSYMBOL_LBRACE = 63,                    /* LBRACE  */
  YYSYMBOL_RBRACE = 64,                    /* RBRACE  */
  YYSYMBOL_LBRACKET = 65,                  /* LBRACKET  */
  YYSYMBOL_RBRACKET = 66,                  /* RBRACKET  */
  YYSYMBOL_COLON = 67,                     /* COLON  */
  YYSYMBOL_SEMICOLON = 68,                 /* SEMICOLON  */
  YYSYMBOL_ASSIGN = 69,                    /* ASSIGN  */
  YYSYMBOL_COMMA = 70,                     /* COMMA  */
  YYSYMBOL_FATARROW = 71,                  /* FATARROW  */
  YYSYMBOL_INT_LITERAL = 72,               /* INT_LITERAL  */
  YYSYMBOL_CHAR_LITERAL = 73,              /* CHAR_LITERAL  */
  YYSYMBOL_FLOAT_LITERAL = 74,             /* FLOAT_LITERAL  */
  YYSYMBOL_STRING_LITERAL = 75,            /* STRING_LITERAL  */
  YYSYMBOL_IDENT = 76,                     /* IDENT  */
  YYSYMBOL_YYACCEPT = 77,                  /* $accept  */
  YYSYMBOL_program = 78,                   /* program  */
  YYSYMBOL_function_list = 79,             /* function_list  */
  YYSYMBOL_extern_decl = 80,               /* extern_decl  */
  YYSYMBOL_extern_param_list_opt = 81,     /* extern_param_list_opt  */
  YYSYMBOL_extern_param_list = 82,         /* extern_param_list  */
  YYSYMBOL_use_decl = 83,                  /* use_decl  */
  YYSYMBOL_link_decl = 84,                 /* link_decl  */
  YYSYMBOL_shim_decl = 85,                 /* shim_decl  */
  YYSYMBOL_function = 86,                  /* function  */
  YYSYMBOL_efn_param_list_opt = 87,        /* efn_param_list_opt  */
  YYSYMBOL_efn_param_list = 88,            /* efn_param_list  */
  YYSYMBOL_struct_def = 89,                /* struct_def  */
  YYSYMBOL_struct_field_list = 90,         /* struct_field_list  */
  YYSYMBOL_struct_field_sep = 91,          /* struct_field_sep  */
  YYSYMBOL_struct_field = 92,              /* struct_field  */
  YYSYMBOL_enum_def = 93,                  /* enum_def  */
  YYSYMBOL_enum_member_list = 94,          /* enum_member_list  */
  YYSYMBOL_enum_member = 95,               /* enum_member  */
  YYSYMBOL_param_list_opt = 96,            /* param_list_opt  */
  YYSYMBOL_param_list = 97,                /* param_list  */
  YYSYMBOL_98_1 = 98,                      /* @1  */
  YYSYMBOL_99_2 = 99,                      /* @2  */
  YYSYMBOL_stmt_list = 100,                /* stmt_list  */
  YYSYMBOL_stmt = 101,                     /* stmt  */
  YYSYMBOL_for_init = 102,                 /* for_init  */
  YYSYMBOL_for_post = 103,                 /* for_post  */
  YYSYMBOL_case_list = 104,                /* case_list  */
  YYSYMBOL_case_body = 105,                /* case_body  */
  YYSYMBOL_block = 106,                    /* block  */
  YYSYMBOL_else_clause = 107,              /* else_clause  */
  YYSYMBOL_type = 108,                     /* type  */
  YYSYMBOL_base_type = 109,                /* base_type  */
  YYSYMBOL_type_list_opt = 110,            /* type_list_opt  */
  YYSYMBOL_type_list = 111,                /* type_list  */
  YYSYMBOL_expr_list_opt = 112,            /* expr_list_opt  */
  YYSYMBOL_expr_list = 113,                /* expr_list  */
  YYSYMBOL_struct_literal_fields = 114,    /* struct_literal_fields  */
  YYSYMBOL_struct_literal_field = 115,     /* struct_literal_field  */
  YYSYMBOL_expr = 116                      /* expr  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int16 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if !defined yyoverflow

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* !defined yyoverflow */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  31
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   777

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  77
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  40
/* YYNRULES -- Number of rules.  */
#define YYNRULES  148
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  363

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   331


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73,    74,
      75,    76
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,   102,   102,   124,   125,   126,   127,   128,   129,   130,
     131,   141,   153,   154,   158,   160,   162,   167,   168,   178,
     195,   200,   202,   204,   206,   208,   210,   218,   219,   223,
     225,   227,   229,   234,   236,   243,   244,   248,   249,   253,
     257,   259,   264,   265,   269,   270,   274,   275,   279,   282,
     295,   281,   307,   308,   312,   314,   317,   319,   321,   323,
     325,   327,   329,   331,   333,   335,   337,   339,   341,   343,
     345,   347,   349,   354,   359,   360,   361,   368,   369,   371,
     377,   378,   383,   387,   388,   390,   395,   396,   411,   428,
     429,   430,   431,   449,   450,   451,   452,   453,   454,   455,
     456,   457,   458,   459,   460,   461,   462,   463,   464,   465,
     481,   482,   486,   487,   491,   492,   496,   497,   501,   502,
     503,   507,   511,   512,   513,   514,   515,   516,   550,   559,
     561,   567,   569,   570,   571,   591,   592,   593,   594,   595,
     596,   597,   598,   599,   600,   601,   602,   603,   604
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if YYDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "EXTERN", "ELLIPSIS",
  "LINK", "SHIM", "FN", "EFN", "VAR", "CONST", "RETURN", "VOID",
  "INT_TYPE", "BOOL_TYPE", "STRING_TYPE", "BYTE_TYPE", "CHAR_TYPE",
  "INT8_TYPE", "INT16_TYPE", "INT32_TYPE", "INT64_TYPE", "UINT8_TYPE",
  "UINT16_TYPE", "UINT32_TYPE", "UINT64_TYPE", "FLOAT_TYPE", "DOUBLE_TYPE",
  "STRUCT", "ENUM", "DOT", "LIST", "IF", "ELSE", "WHILE", "FOR", "IN",
  "BREAK", "CONTINUE", "SWITCH", "CASE", "DEFAULT", "IO", "COLONCOLON",
  "USE", "AS", "PRIVATE", "EQ", "NE", "LT", "LE", "GT", "GE", "INC", "DEC",
  "PLUS", "MINUS", "STAR", "SLASH", "AMPERSAND", "UMINUS", "LPAREN",
  "RPAREN", "LBRACE", "RBRACE", "LBRACKET", "RBRACKET", "COLON",
  "SEMICOLON", "ASSIGN", "COMMA", "FATARROW", "INT_LITERAL",
  "CHAR_LITERAL", "FLOAT_LITERAL", "STRING_LITERAL", "IDENT", "$accept",
  "program", "function_list", "extern_decl", "extern_param_list_opt",
  "extern_param_list", "use_decl", "link_decl", "shim_decl", "function",
  "efn_param_list_opt", "efn_param_list", "struct_def",
  "struct_field_list", "struct_field_sep", "struct_field", "enum_def",
  "enum_member_list", "enum_member", "param_list_opt", "param_list", "@1",
  "@2", "stmt_list", "stmt", "for_init", "for_post", "case_list",
  "case_body", "block", "else_clause", "type", "base_type",
  "type_list_opt", "type_list", "expr_list_opt", "expr_list",
  "struct_literal_fields", "struct_literal_field", "expr", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-294)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-50)

#define yytable_value_is_error(Yyn) \
  ((Yyn) == YYTABLE_NINF)

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
      76,     4,   -54,   -45,   -36,   -23,   -14,    40,   -30,    49,
     128,  -294,    76,    76,    76,    76,    76,    76,    76,    59,
      69,    73,    86,    87,    91,    92,   -31,    80,    83,    84,
      85,  -294,  -294,  -294,  -294,  -294,  -294,  -294,  -294,   101,
    -294,  -294,    88,    89,    90,    93,    96,  -294,   102,   107,
     114,   115,    -1,   106,   117,  -294,    -7,   118,  -294,   116,
     120,     3,   112,   121,   123,   127,    88,    89,    90,    93,
    -294,   124,   126,  -294,   224,   -41,   224,    89,   111,   224,
    -294,  -294,  -294,    90,   125,  -294,    93,  -294,   134,   138,
     139,   140,   224,   135,   149,  -294,   146,  -294,  -294,  -294,
    -294,  -294,  -294,  -294,  -294,  -294,  -294,  -294,  -294,  -294,
    -294,   164,  -294,   144,   158,   247,   224,   147,  -294,   133,
    -294,  -294,  -294,  -294,    51,   145,  -294,  -294,   148,   224,
     224,   153,   110,  -294,   163,   150,   151,    77,   160,   161,
     162,   157,   165,   168,   189,   133,   133,   133,   133,   133,
    -294,  -294,  -294,  -294,   -15,   171,   247,   277,   190,    89,
     133,   -20,   299,   247,   224,   133,    -1,   184,   196,   192,
    -294,  -294,   208,   216,   202,  -294,   206,   207,  -294,   321,
     133,   133,    -2,  -294,  -294,   133,   -27,   210,    52,   210,
     586,   211,  -294,   213,   200,   204,   219,   220,   133,   214,
     133,   133,  -294,  -294,   133,   133,   133,   133,   133,   133,
     133,   133,   133,   133,   133,  -294,   247,  -294,   210,   215,
    -294,   228,   230,   343,  -294,  -294,   224,   227,  -294,  -294,
      88,   224,   224,  -294,   605,   624,   221,   259,   133,   643,
     235,   237,   133,  -294,  -294,   133,   232,  -294,  -294,  -294,
     240,   238,     6,  -294,   562,   365,   700,   700,   700,   700,
     700,   700,   -38,   -38,   210,   210,   574,   243,  -294,  -294,
     247,  -294,  -294,   224,  -294,    62,   241,   236,   236,   242,
     133,   387,   248,   133,   133,   409,  -294,   133,  -294,   133,
    -294,   214,   244,  -294,  -294,  -294,   250,  -294,  -294,   133,
     133,   247,   282,  -294,   224,   662,   254,    99,   255,   256,
    -294,   431,   712,  -294,   133,  -294,   453,   475,   252,   -19,
    -294,   262,   236,    11,   274,   133,   270,   275,  -294,  -294,
    -294,   497,  -294,  -294,  -294,   279,  -294,   133,  -294,  -294,
    -294,   133,   236,   541,   247,  -294,  -294,   133,   519,   712,
    -294,   247,   247,    99,   681,  -294,    99,  -294,  -294,   236,
    -294,   282,  -294
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     2,     0,     0,     0,     0,     3,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     1,     8,     7,     9,    10,     4,     5,     6,     0,
      19,    20,    46,    27,    35,     0,     0,    17,     0,     0,
       0,     0,    12,     0,     0,    47,    29,     0,    28,     0,
       0,     0,    44,     0,    42,     0,    46,    27,    35,     0,
      14,     0,     0,    13,     0,     0,     0,     0,     0,     0,
      33,    37,    38,    35,     0,    40,     0,    18,     0,     0,
       0,     0,     0,     0,     0,    96,    93,    94,    95,    97,
      98,    99,   100,   101,   102,   103,   104,   105,   106,   107,
     108,     0,   109,    48,    86,    52,     0,    30,    31,     0,
      39,    36,    45,    43,     0,     0,    34,    41,    15,     0,
     110,     0,     0,    50,    87,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,   114,
     122,   124,   123,   125,   126,     0,    52,     0,     0,     0,
       0,   126,     0,    52,     0,     0,     0,     0,   112,     0,
     111,    89,     0,     0,     0,    88,     0,     0,    57,     0,
       0,     0,     0,    64,    65,     0,     0,   134,   133,   132,
       0,     0,   115,   116,     0,     0,     0,     0,   114,   118,
       0,     0,    21,    53,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,    59,    52,    32,   133,     0,
      23,     0,     0,     0,    16,    11,     0,     0,    90,    91,
       0,     0,     0,    58,     0,     0,     0,     0,     0,     0,
       0,     0,     0,   148,   130,     0,   135,   136,    71,    72,
       0,     0,     0,   119,     0,     0,   138,   139,   140,   141,
     142,   143,   144,   145,   146,   147,     0,     0,   135,    24,
      52,    26,   113,     0,    51,     0,     0,     0,     0,     0,
       0,     0,     0,   114,   114,     0,   117,     0,   129,     0,
     137,     0,     0,    67,   131,    22,     0,    92,    55,     0,
       0,    52,    83,    61,     0,     0,     0,    77,     0,     0,
      69,     0,   121,   120,     0,    25,     0,     0,     0,     0,
      60,     0,     0,     0,     0,     0,     0,     0,   128,   127,
      70,     0,    54,    56,    82,     0,    85,     0,    63,    75,
      76,     0,     0,     0,    80,    66,    68,     0,     0,    74,
      62,    80,    80,    77,     0,    73,    77,    81,    79,     0,
      78,    83,    84
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -294,  -294,    95,  -294,  -294,   172,  -294,  -294,  -294,  -294,
     276,   -73,  -294,   -52,  -294,  -294,  -294,   -57,  -294,   278,
     122,  -294,  -294,  -155,  -293,  -294,  -294,  -229,  -208,  -253,
      -8,   -74,  -294,  -294,   132,  -181,   129,  -294,    50,  -113
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
       0,    10,    11,    12,    72,    73,    13,    14,    15,    16,
      57,    58,    17,    60,    83,    61,    18,    63,    64,    54,
      55,   133,   174,   155,   156,   238,   324,   327,   353,   302,
     320,   168,   114,   169,   170,   191,   192,   252,   253,   157
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
     113,   203,   117,    70,   118,   120,   162,   236,   221,   240,
     219,    19,    91,   335,    46,   194,    90,   250,   128,   212,
     213,    20,   115,   195,   179,   303,   116,   214,   195,   123,
      21,   121,   187,   188,   189,   190,   193,    47,   196,   197,
      22,   198,   158,   199,   301,    26,   198,   218,   199,   241,
     200,   352,   223,    23,   201,   167,    27,    28,   352,   352,
      76,   267,    24,    77,   339,   340,   336,   234,   235,   338,
     290,    81,   239,    82,   237,    71,   291,    29,    30,     1,
     341,     2,     3,     4,     5,   193,   217,   254,   255,   350,
     222,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   308,   309,     6,     7,   361,    32,    33,    34,
      35,    36,    37,    38,   163,   296,    25,   214,   164,   144,
       8,   242,     9,   172,   358,   281,   173,   360,    31,   285,
     298,   299,   193,   145,   160,    39,   147,    40,   148,   325,
     326,    41,   149,   356,   357,   178,   318,    42,    43,   150,
     151,   152,   153,   161,    44,    45,    48,   275,   276,    49,
      50,    51,    52,    66,    53,    56,    59,   305,    67,    62,
     193,   193,    65,    74,   311,   144,   312,    68,    69,    75,
      78,    84,   119,    79,    80,    85,   316,   317,    93,   145,
     160,    92,   147,    86,   148,    87,   124,   122,   149,   297,
     125,   331,   129,   126,   127,   150,   151,   152,   153,   161,
     130,   131,   343,   132,   -49,   134,   165,   159,   166,   171,
     175,   180,   181,   182,   348,   183,   176,   177,   349,   185,
     321,    94,   186,   184,   354,   202,    95,    96,    97,    98,
      99,   100,   101,   102,   103,   104,   105,   106,   107,   108,
     109,   110,   225,   216,   227,   111,   135,   136,   137,   228,
     204,   205,   206,   207,   208,   209,   226,   229,   210,   211,
     212,   213,   230,   231,   232,   214,   246,   244,   214,   138,
     247,   139,   140,   245,   141,   142,   143,   248,   249,   144,
     251,   268,   269,   270,   273,   280,   283,   279,   284,   301,
     112,   287,   288,   145,   146,   289,   147,   295,   148,   304,
     300,   307,   149,   314,   315,   319,   334,   328,   329,   150,
     151,   152,   153,   154,   204,   205,   206,   207,   208,   209,
     323,   337,   210,   211,   212,   213,   342,   344,   224,   345,
     347,   313,   214,    89,    88,   215,   204,   205,   206,   207,
     208,   209,   274,   362,   210,   211,   212,   213,   272,     0,
       0,     0,     0,     0,   214,     0,     0,   220,   204,   205,
     206,   207,   208,   209,   286,     0,   210,   211,   212,   213,
       0,     0,     0,     0,     0,     0,   214,     0,     0,   233,
     204,   205,   206,   207,   208,   209,     0,     0,   210,   211,
     212,   213,     0,     0,     0,     0,     0,     0,   214,     0,
       0,   271,   204,   205,   206,   207,   208,   209,     0,     0,
     210,   211,   212,   213,     0,     0,     0,     0,     0,     0,
     214,     0,     0,   293,   204,   205,   206,   207,   208,   209,
       0,     0,   210,   211,   212,   213,     0,     0,     0,     0,
       0,     0,   214,     0,     0,   306,   204,   205,   206,   207,
     208,   209,     0,     0,   210,   211,   212,   213,     0,     0,
       0,     0,     0,     0,   214,     0,     0,   310,   204,   205,
     206,   207,   208,   209,     0,     0,   210,   211,   212,   213,
       0,     0,     0,     0,     0,     0,   214,     0,     0,   330,
     204,   205,   206,   207,   208,   209,     0,     0,   210,   211,
     212,   213,     0,     0,     0,     0,     0,     0,   214,     0,
       0,   332,   204,   205,   206,   207,   208,   209,     0,     0,
     210,   211,   212,   213,     0,     0,     0,     0,     0,     0,
     214,     0,     0,   333,   204,   205,   206,   207,   208,   209,
       0,     0,   210,   211,   212,   213,     0,     0,     0,     0,
       0,     0,   214,     0,     0,   346,   204,   205,   206,   207,
     208,   209,     0,     0,   210,   211,   212,   213,     0,     0,
       0,     0,     0,     0,   214,     0,     0,   355,   204,   205,
     206,   207,   208,   209,     0,     0,   210,   211,   212,   213,
       0,     0,     0,     0,     0,     0,   214,     0,   351,   204,
     205,   206,   207,   208,   209,     0,     0,   210,   211,   212,
     213,   204,   205,   206,   207,   208,   209,   214,   292,   210,
     211,   212,   213,   204,   205,   206,   207,   208,   209,   214,
     294,   210,   211,   212,   213,     0,     0,     0,   243,     0,
       0,   214,   204,   205,   206,   207,   208,   209,     0,     0,
     210,   211,   212,   213,     0,     0,     0,   277,     0,     0,
     214,   204,   205,   206,   207,   208,   209,     0,     0,   210,
     211,   212,   213,     0,     0,     0,   278,     0,     0,   214,
     204,   205,   206,   207,   208,   209,     0,     0,   210,   211,
     212,   213,     0,     0,     0,   282,     0,     0,   214,   204,
     205,   206,   207,   208,   209,     0,     0,   210,   211,   212,
     213,     0,     0,     0,   322,     0,     0,   214,   204,   205,
     206,   207,   208,   209,     0,     0,   210,   211,   212,   213,
       0,     0,     0,   359,     0,     0,   214,   -50,   -50,   -50,
     -50,   -50,   -50,     0,     0,   210,   211,   212,   213,   204,
     205,   206,   207,   208,   209,   214,     0,   210,   211,   212,
     213,     0,     0,     0,     0,     0,     0,   214
};

static const yytype_int16 yycheck[] =
{
      74,   156,    76,     4,    77,    79,   119,     9,   163,    36,
      30,     7,    69,    32,    45,    30,    68,   198,    92,    57,
      58,    75,    63,    43,   137,   278,    67,    65,    43,    86,
      75,    83,   145,   146,   147,   148,   149,    68,    53,    54,
      76,    61,   116,    63,    63,    75,    61,   160,    63,    76,
      65,   344,   165,    76,    69,   129,     7,     8,   351,   352,
      67,   216,    76,    70,    53,    54,   319,   180,   181,   322,
      64,    68,   185,    70,    76,    76,    70,    28,    29,     3,
      69,     5,     6,     7,     8,   198,   159,   200,   201,   342,
     164,   204,   205,   206,   207,   208,   209,   210,   211,   212,
     213,   214,   283,   284,    28,    29,   359,    12,    13,    14,
      15,    16,    17,    18,    63,   270,    76,    65,    67,    42,
      44,    69,    46,    13,   353,   238,    16,   356,     0,   242,
      68,    69,   245,    56,    57,    76,    59,    68,    61,    40,
      41,    68,    65,   351,   352,    68,   301,    61,    61,    72,
      73,    74,    75,    76,    63,    63,    76,   231,   232,    76,
      76,    76,    61,    61,    76,    76,    76,   280,    61,    76,
     283,   284,    76,    67,   287,    42,   289,    63,    63,    62,
      62,    69,    71,    67,    64,    64,   299,   300,    62,    56,
      57,    67,    59,    70,    61,    68,    62,    72,    65,   273,
      62,   314,    67,    64,    64,    72,    73,    74,    75,    76,
      61,    65,   325,    49,    70,    57,    71,    70,    70,    66,
      57,    61,    61,    61,   337,    68,    76,    76,   341,    61,
     304,     7,    43,    68,   347,    64,    12,    13,    14,    15,
      16,    17,    18,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    68,    63,    62,    31,     9,    10,    11,    51,
      47,    48,    49,    50,    51,    52,    70,    51,    55,    56,
      57,    58,    70,    67,    67,    65,    76,    66,    65,    32,
      76,    34,    35,    70,    37,    38,    39,    68,    68,    42,
      76,    76,    64,    63,    67,    36,    61,    76,    61,    63,
      76,    69,    62,    56,    57,    67,    59,    64,    61,    67,
      69,    63,    65,    69,    64,    33,    64,    62,    62,    72,
      73,    74,    75,    76,    47,    48,    49,    50,    51,    52,
      76,    69,    55,    56,    57,    58,    62,    67,   166,    64,
      61,   291,    65,    67,    66,    68,    47,    48,    49,    50,
      51,    52,   230,   361,    55,    56,    57,    58,   226,    -1,
      -1,    -1,    -1,    -1,    65,    -1,    -1,    68,    47,    48,
      49,    50,    51,    52,   245,    -1,    55,    56,    57,    58,
      -1,    -1,    -1,    -1,    -1,    -1,    65,    -1,    -1,    68,
      47,    48,    49,    50,    51,    52,    -1,    -1,    55,    56,
      57,    58,    -1,    -1,    -1,    -1,    -1,    -1,    65,    -1,
      -1,    68,    47,    48,    49,    50,    51,    52,    -1,    -1,
      55,    56,    57,    58,    -1,    -1,    -1,    -1,    -1,    -1,
      65,    -1,    -1,    68,    47,    48,    49,    50,    51,    52,
      -1,    -1,    55,    56,    57,    58,    -1,    -1,    -1,    -1,
      -1,    -1,    65,    -1,    -1,    68,    47,    48,    49,    50,
      51,    52,    -1,    -1,    55,    56,    57,    58,    -1,    -1,
      -1,    -1,    -1,    -1,    65,    -1,    -1,    68,    47,    48,
      49,    50,    51,    52,    -1,    -1,    55,    56,    57,    58,
      -1,    -1,    -1,    -1,    -1,    -1,    65,    -1,    -1,    68,
      47,    48,    49,    50,    51,    52,    -1,    -1,    55,    56,
      57,    58,    -1,    -1,    -1,    -1,    -1,    -1,    65,    -1,
      -1,    68,    47,    48,    49,    50,    51,    52,    -1,    -1,
      55,    56,    57,    58,    -1,    -1,    -1,    -1,    -1,    -1,
      65,    -1,    -1,    68,    47,    48,    49,    50,    51,    52,
      -1,    -1,    55,    56,    57,    58,    -1,    -1,    -1,    -1,
      -1,    -1,    65,    -1,    -1,    68,    47,    48,    49,    50,
      51,    52,    -1,    -1,    55,    56,    57,    58,    -1,    -1,
      -1,    -1,    -1,    -1,    65,    -1,    -1,    68,    47,    48,
      49,    50,    51,    52,    -1,    -1,    55,    56,    57,    58,
      -1,    -1,    -1,    -1,    -1,    -1,    65,    -1,    67,    47,
      48,    49,    50,    51,    52,    -1,    -1,    55,    56,    57,
      58,    47,    48,    49,    50,    51,    52,    65,    66,    55,
      56,    57,    58,    47,    48,    49,    50,    51,    52,    65,
      66,    55,    56,    57,    58,    -1,    -1,    -1,    62,    -1,
      -1,    65,    47,    48,    49,    50,    51,    52,    -1,    -1,
      55,    56,    57,    58,    -1,    -1,    -1,    62,    -1,    -1,
      65,    47,    48,    49,    50,    51,    52,    -1,    -1,    55,
      56,    57,    58,    -1,    -1,    -1,    62,    -1,    -1,    65,
      47,    48,    49,    50,    51,    52,    -1,    -1,    55,    56,
      57,    58,    -1,    -1,    -1,    62,    -1,    -1,    65,    47,
      48,    49,    50,    51,    52,    -1,    -1,    55,    56,    57,
      58,    -1,    -1,    -1,    62,    -1,    -1,    65,    47,    48,
      49,    50,    51,    52,    -1,    -1,    55,    56,    57,    58,
      -1,    -1,    -1,    62,    -1,    -1,    65,    47,    48,    49,
      50,    51,    52,    -1,    -1,    55,    56,    57,    58,    47,
      48,    49,    50,    51,    52,    65,    -1,    55,    56,    57,
      58,    -1,    -1,    -1,    -1,    -1,    -1,    65
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,     3,     5,     6,     7,     8,    28,    29,    44,    46,
      78,    79,    80,    83,    84,    85,    86,    89,    93,     7,
      75,    75,    76,    76,    76,    76,    75,     7,     8,    28,
      29,     0,    79,    79,    79,    79,    79,    79,    79,    76,
      68,    68,    61,    61,    63,    63,    45,    68,    76,    76,
      76,    76,    61,    76,    96,    97,    76,    87,    88,    76,
      90,    92,    76,    94,    95,    76,    61,    61,    63,    63,
       4,    76,    81,    82,    67,    62,    67,    70,    62,    67,
      64,    68,    70,    91,    69,    64,    70,    68,    96,    87,
      90,    94,    67,    62,     7,    12,    13,    14,    15,    16,
      17,    18,    19,    20,    21,    22,    23,    24,    25,    26,
      27,    31,    76,   108,   109,    63,    67,   108,    88,    71,
     108,    90,    72,    94,    62,    62,    64,    64,   108,    67,
      61,    65,    49,    98,    57,     9,    10,    11,    32,    34,
      35,    37,    38,    39,    42,    56,    57,    59,    61,    65,
      72,    73,    74,    75,    76,   100,   101,   116,   108,    70,
      57,    76,   116,    63,    67,    71,    70,   108,   108,   110,
     111,    66,    13,    16,    99,    57,    76,    76,    68,   116,
      61,    61,    61,    68,    68,    61,    43,   116,   116,   116,
     116,   112,   113,   116,    30,    43,    53,    54,    61,    63,
      65,    69,    64,   100,    47,    48,    49,    50,    51,    52,
      55,    56,    57,    58,    65,    68,    63,    88,   116,    30,
      68,   100,   108,   116,    82,    68,    70,    62,    51,    51,
      70,    67,    67,    68,   116,   116,     9,    76,   102,   116,
      36,    76,    69,    62,    66,    70,    76,    76,    68,    68,
     112,    76,   114,   115,   116,   116,   116,   116,   116,   116,
     116,   116,   116,   116,   116,   116,   116,   100,    76,    64,
      63,    68,   111,    67,    97,   108,   108,    62,    62,    76,
      36,   116,    62,    61,    61,   116,   113,    69,    62,    67,
      64,    70,    66,    68,    66,    64,   100,   108,    68,    69,
      69,    63,   106,   106,    67,   116,    68,    63,   112,   112,
      68,   116,   116,   115,    69,    64,   116,   116,   100,    33,
     107,   108,    62,    76,   103,    40,    41,   104,    62,    62,
      68,   116,    68,    68,    64,    32,   106,    69,   106,    53,
      54,    69,    62,   116,    67,    64,    68,    61,   116,   116,
     106,    67,   101,   105,   116,    68,   105,   105,   104,    62,
     104,   106,   107
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    77,    78,    79,    79,    79,    79,    79,    79,    79,
      79,    80,    81,    81,    82,    82,    82,    83,    83,    84,
      85,    86,    86,    86,    86,    86,    86,    87,    87,    88,
      88,    88,    88,    89,    89,    90,    90,    91,    91,    92,
      93,    93,    94,    94,    95,    95,    96,    96,    97,    98,
      99,    97,   100,   100,   101,   101,   101,   101,   101,   101,
     101,   101,   101,   101,   101,   101,   101,   101,   101,   101,
     101,   101,   101,   102,   103,   103,   103,   104,   104,   104,
     105,   105,   106,   107,   107,   107,   108,   108,   108,   108,
     108,   108,   108,   109,   109,   109,   109,   109,   109,   109,
     109,   109,   109,   109,   109,   109,   109,   109,   109,   109,
     110,   110,   111,   111,   112,   112,   113,   113,   114,   114,
     114,   115,   116,   116,   116,   116,   116,   116,   116,   116,
     116,   116,   116,   116,   116,   116,   116,   116,   116,   116,
     116,   116,   116,   116,   116,   116,   116,   116,   116
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     1,     1,     2,     2,     2,     2,     2,     2,
       2,     9,     0,     1,     1,     3,     5,     3,     5,     3,
       3,     8,    10,     8,     9,    11,     9,     0,     1,     1,
       3,     3,     5,     5,     6,     0,     3,     1,     1,     3,
       5,     6,     1,     3,     1,     3,     0,     1,     3,     0,
       0,     7,     0,     2,     7,     5,     7,     2,     3,     2,
       6,     5,     8,     7,     2,     2,     7,     4,     7,     5,
       6,     3,     3,     7,     3,     2,     2,     0,     5,     4,
       0,     2,     3,     0,     7,     2,     1,     2,     3,     3,
       4,     4,     6,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       0,     1,     1,     3,     0,     1,     1,     3,     0,     1,
       3,     3,     1,     1,     1,     1,     1,     6,     6,     4,
       3,     4,     2,     2,     2,     3,     3,     4,     3,     3,
       3,     3,     3,     3,     3,     3,     3,     3,     3
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)




# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  yy_symbol_value_print (yyo, yykind, yyvaluep);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp,
                 int yyrule)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)]);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif






/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep)
{
  YY_USE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/* Lookahead token kind.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
int yynerrs;




/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;



#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex ();
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2: /* program: function_list  */
#line 103 "src/parser.y"
        {
            g_program.functions = (yyvsp[0].func);
            g_program.main_fn = NULL;
            for (Function *f = (yyvsp[0].func); f; f = f->next) {
                if (strcmp(f->name, "main") == 0) g_program.main_fn = f;
            }
            if (!g_program.main_fn) {
                yyerror("no main function found");
            }
        }
#line 1542 "src/parser.tab.c"
    break;

  case 3: /* function_list: function  */
#line 124 "src/parser.y"
                                  { (yyval.func) = (yyvsp[0].func); }
#line 1548 "src/parser.tab.c"
    break;

  case 4: /* function_list: function function_list  */
#line 125 "src/parser.y"
                                  { (yyvsp[-1].func)->next = (yyvsp[0].func); (yyval.func) = (yyvsp[-1].func); }
#line 1554 "src/parser.tab.c"
    break;

  case 5: /* function_list: struct_def function_list  */
#line 126 "src/parser.y"
                                  { (yyval.func) = (yyvsp[0].func); }
#line 1560 "src/parser.tab.c"
    break;

  case 6: /* function_list: enum_def function_list  */
#line 127 "src/parser.y"
                                  { (yyval.func) = (yyvsp[0].func); }
#line 1566 "src/parser.tab.c"
    break;

  case 7: /* function_list: use_decl function_list  */
#line 128 "src/parser.y"
                                  { (yyval.func) = (yyvsp[0].func); }
#line 1572 "src/parser.tab.c"
    break;

  case 8: /* function_list: extern_decl function_list  */
#line 129 "src/parser.y"
                                  { (yyvsp[-1].func)->next = (yyvsp[0].func); (yyval.func) = (yyvsp[-1].func); }
#line 1578 "src/parser.tab.c"
    break;

  case 9: /* function_list: link_decl function_list  */
#line 130 "src/parser.y"
                                  { (yyval.func) = (yyvsp[0].func); }
#line 1584 "src/parser.tab.c"
    break;

  case 10: /* function_list: shim_decl function_list  */
#line 131 "src/parser.y"
                                  { (yyval.func) = (yyvsp[0].func); }
#line 1590 "src/parser.tab.c"
    break;

  case 11: /* extern_decl: EXTERN FN IDENT LPAREN extern_param_list_opt RPAREN COLON type SEMICOLON  */
#line 142 "src/parser.y"
        {
            (yyval.func) = function_new_extern((yyvsp[-6].str), (yyvsp[-1].type), (yyvsp[-4].param), g_last_is_variadic);
            (yyval.func)->ret_elem_type = g_last_elem_type; (yyval.func)->ret_elem_elem_type = g_last_elem_elem_type;
            g_last_is_variadic = 0;
        }
#line 1600 "src/parser.tab.c"
    break;

  case 12: /* extern_param_list_opt: %empty  */
#line 153 "src/parser.y"
                                 { (yyval.param) = NULL; g_last_is_variadic = 0; }
#line 1606 "src/parser.tab.c"
    break;

  case 13: /* extern_param_list_opt: extern_param_list  */
#line 154 "src/parser.y"
                                 { (yyval.param) = (yyvsp[0].param); }
#line 1612 "src/parser.tab.c"
    break;

  case 14: /* extern_param_list: ELLIPSIS  */
#line 159 "src/parser.y"
        { (yyval.param) = NULL; g_last_is_variadic = 1; }
#line 1618 "src/parser.tab.c"
    break;

  case 15: /* extern_param_list: IDENT COLON type  */
#line 161 "src/parser.y"
        { (yyval.param) = param_new((yyvsp[-2].str), (yyvsp[0].type), NULL); (yyval.param)->elem_type = g_last_elem_type; g_last_is_variadic = 0; (yyval.param)->elem_elem_type = g_last_elem_elem_type; }
#line 1624 "src/parser.tab.c"
    break;

  case 16: /* extern_param_list: IDENT COLON type COMMA extern_param_list  */
#line 163 "src/parser.y"
        { (yyval.param) = param_new((yyvsp[-4].str), (yyvsp[-2].type), (yyvsp[0].param)); (yyval.param)->elem_type = g_last_elem_type; (yyval.param)->elem_elem_type = g_last_elem_elem_type; }
#line 1630 "src/parser.tab.c"
    break;

  case 17: /* use_decl: USE STRING_LITERAL SEMICOLON  */
#line 167 "src/parser.y"
                                              { (yyval.func) = NULL; }
#line 1636 "src/parser.tab.c"
    break;

  case 18: /* use_decl: USE STRING_LITERAL AS IDENT SEMICOLON  */
#line 168 "src/parser.y"
                                               { (yyval.func) = NULL; }
#line 1642 "src/parser.tab.c"
    break;

  case 19: /* link_decl: LINK STRING_LITERAL SEMICOLON  */
#line 179 "src/parser.y"
        { link_lib_add((yyvsp[-1].str)); (yyval.func) = NULL; }
#line 1648 "src/parser.tab.c"
    break;

  case 20: /* shim_decl: SHIM STRING_LITERAL SEMICOLON  */
#line 196 "src/parser.y"
        { (yyval.func) = NULL; }
#line 1654 "src/parser.tab.c"
    break;

  case 21: /* function: FN IDENT LPAREN param_list_opt RPAREN LBRACE stmt_list RBRACE  */
#line 201 "src/parser.y"
        { (yyval.func) = function_new((yyvsp[-6].str), TYPE_VOID, (yyvsp[-4].param), (yyvsp[-1].stmt)); }
#line 1660 "src/parser.tab.c"
    break;

  case 22: /* function: FN IDENT LPAREN param_list_opt RPAREN COLON type LBRACE stmt_list RBRACE  */
#line 203 "src/parser.y"
        { (yyval.func) = function_new((yyvsp[-8].str), (yyvsp[-3].type), (yyvsp[-6].param), (yyvsp[-1].stmt)); (yyval.func)->ret_elem_type = g_last_elem_type; (yyval.func)->ret_elem_elem_type = g_last_elem_elem_type; }
#line 1666 "src/parser.tab.c"
    break;

  case 23: /* function: EFN IDENT LPAREN efn_param_list_opt RPAREN FATARROW expr SEMICOLON  */
#line 205 "src/parser.y"
        { (yyval.func) = efn_new((yyvsp[-6].str), (yyvsp[-4].param), (yyvsp[-1].expr)); }
#line 1672 "src/parser.tab.c"
    break;

  case 24: /* function: PRIVATE FN IDENT LPAREN param_list_opt RPAREN LBRACE stmt_list RBRACE  */
#line 207 "src/parser.y"
        { (yyval.func) = function_new((yyvsp[-6].str), TYPE_VOID, (yyvsp[-4].param), (yyvsp[-1].stmt)); }
#line 1678 "src/parser.tab.c"
    break;

  case 25: /* function: PRIVATE FN IDENT LPAREN param_list_opt RPAREN COLON type LBRACE stmt_list RBRACE  */
#line 209 "src/parser.y"
        { (yyval.func) = function_new((yyvsp[-8].str), (yyvsp[-3].type), (yyvsp[-6].param), (yyvsp[-1].stmt)); (yyval.func)->ret_elem_type = g_last_elem_type; (yyval.func)->ret_elem_elem_type = g_last_elem_elem_type; }
#line 1684 "src/parser.tab.c"
    break;

  case 26: /* function: PRIVATE EFN IDENT LPAREN efn_param_list_opt RPAREN FATARROW expr SEMICOLON  */
#line 211 "src/parser.y"
        { (yyval.func) = efn_new((yyvsp[-6].str), (yyvsp[-4].param), (yyvsp[-1].expr)); }
#line 1690 "src/parser.tab.c"
    break;

  case 27: /* efn_param_list_opt: %empty  */
#line 218 "src/parser.y"
                          { (yyval.param) = NULL; }
#line 1696 "src/parser.tab.c"
    break;

  case 28: /* efn_param_list_opt: efn_param_list  */
#line 219 "src/parser.y"
                           { (yyval.param) = (yyvsp[0].param); }
#line 1702 "src/parser.tab.c"
    break;

  case 29: /* efn_param_list: IDENT  */
#line 224 "src/parser.y"
        { (yyval.param) = param_new_untyped((yyvsp[0].str), NULL); }
#line 1708 "src/parser.tab.c"
    break;

  case 30: /* efn_param_list: IDENT COLON type  */
#line 226 "src/parser.y"
        { (yyval.param) = param_new((yyvsp[-2].str), (yyvsp[0].type), NULL); (yyval.param)->elem_type = g_last_elem_type; (yyval.param)->elem_elem_type = g_last_elem_elem_type; }
#line 1714 "src/parser.tab.c"
    break;

  case 31: /* efn_param_list: IDENT COMMA efn_param_list  */
#line 228 "src/parser.y"
        { (yyval.param) = param_new_untyped((yyvsp[-2].str), (yyvsp[0].param)); }
#line 1720 "src/parser.tab.c"
    break;

  case 32: /* efn_param_list: IDENT COLON type COMMA efn_param_list  */
#line 230 "src/parser.y"
        { (yyval.param) = param_new((yyvsp[-4].str), (yyvsp[-2].type), (yyvsp[0].param)); (yyval.param)->elem_type = g_last_elem_type; (yyval.param)->elem_elem_type = g_last_elem_elem_type; }
#line 1726 "src/parser.tab.c"
    break;

  case 33: /* struct_def: STRUCT IDENT LBRACE struct_field_list RBRACE  */
#line 235 "src/parser.y"
        { struct_def_new((yyvsp[-3].str), (yyvsp[-1].field)); (yyval.func) = NULL; }
#line 1732 "src/parser.tab.c"
    break;

  case 34: /* struct_def: PRIVATE STRUCT IDENT LBRACE struct_field_list RBRACE  */
#line 237 "src/parser.y"
        { struct_def_new((yyvsp[-3].str), (yyvsp[-1].field)); (yyval.func) = NULL; }
#line 1738 "src/parser.tab.c"
    break;

  case 35: /* struct_field_list: %empty  */
#line 243 "src/parser.y"
                                             { (yyval.field) = NULL; }
#line 1744 "src/parser.tab.c"
    break;

  case 36: /* struct_field_list: struct_field struct_field_sep struct_field_list  */
#line 244 "src/parser.y"
                                                      { (yyval.field) = (yyvsp[-2].field); (yyvsp[-2].field)->next = (yyvsp[0].field); }
#line 1750 "src/parser.tab.c"
    break;

  case 39: /* struct_field: IDENT COLON type  */
#line 253 "src/parser.y"
                     { (yyval.field) = field_new((yyvsp[-2].str), (yyvsp[0].type), NULL); (yyval.field)->elem_type = g_last_elem_type; (yyval.field)->elem_elem_type = g_last_elem_elem_type; }
#line 1756 "src/parser.tab.c"
    break;

  case 40: /* enum_def: ENUM IDENT LBRACE enum_member_list RBRACE  */
#line 258 "src/parser.y"
        { enum_def_new((yyvsp[-3].str), (yyvsp[-1].enummember)); (yyval.func) = NULL; }
#line 1762 "src/parser.tab.c"
    break;

  case 41: /* enum_def: PRIVATE ENUM IDENT LBRACE enum_member_list RBRACE  */
#line 260 "src/parser.y"
        { enum_def_new((yyvsp[-3].str), (yyvsp[-1].enummember)); (yyval.func) = NULL; }
#line 1768 "src/parser.tab.c"
    break;

  case 43: /* enum_member_list: enum_member COMMA enum_member_list  */
#line 265 "src/parser.y"
                                         { (yyval.enummember) = (yyvsp[-2].enummember); (yyvsp[-2].enummember)->next = (yyvsp[0].enummember); }
#line 1774 "src/parser.tab.c"
    break;

  case 44: /* enum_member: IDENT  */
#line 269 "src/parser.y"
          { (yyval.enummember) = enum_member_new((yyvsp[0].str), -1, NULL); }
#line 1780 "src/parser.tab.c"
    break;

  case 45: /* enum_member: IDENT ASSIGN INT_LITERAL  */
#line 270 "src/parser.y"
                             { (yyval.enummember) = enum_member_new((yyvsp[-2].str), (yyvsp[0].num), NULL); }
#line 1786 "src/parser.tab.c"
    break;

  case 46: /* param_list_opt: %empty  */
#line 274 "src/parser.y"
                     { (yyval.param) = NULL; }
#line 1792 "src/parser.tab.c"
    break;

  case 47: /* param_list_opt: param_list  */
#line 275 "src/parser.y"
                      { (yyval.param) = (yyvsp[0].param); }
#line 1798 "src/parser.tab.c"
    break;

  case 48: /* param_list: IDENT COLON type  */
#line 280 "src/parser.y"
        { (yyval.param) = param_new((yyvsp[-2].str), (yyvsp[0].type), NULL); (yyval.param)->elem_type = g_last_elem_type; (yyval.param)->elem_elem_type = g_last_elem_elem_type; }
#line 1804 "src/parser.tab.c"
    break;

  case 49: /* @1: %empty  */
#line 282 "src/parser.y"
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
          (yyval.type) = g_last_elem_type;
        }
#line 1822 "src/parser.tab.c"
    break;

  case 50: /* @2: %empty  */
#line 295 "src/parser.y"
        {
          (yyval.type) = g_last_elem_elem_type;
        }
#line 1830 "src/parser.tab.c"
    break;

  case 51: /* param_list: IDENT COLON type @1 @2 COMMA param_list  */
#line 299 "src/parser.y"
        {
          (yyval.param) = param_new((yyvsp[-6].str), (yyvsp[-4].type), (yyvsp[0].param));
          (yyval.param)->elem_type = (yyvsp[-3].type);
          (yyval.param)->elem_elem_type = (yyvsp[-2].type);
        }
#line 1840 "src/parser.tab.c"
    break;

  case 52: /* stmt_list: %empty  */
#line 307 "src/parser.y"
                              { (yyval.stmt) = NULL; }
#line 1846 "src/parser.tab.c"
    break;

  case 53: /* stmt_list: stmt stmt_list  */
#line 308 "src/parser.y"
                              { (yyvsp[-1].stmt)->next = (yyvsp[0].stmt); (yyval.stmt) = (yyvsp[-1].stmt); }
#line 1852 "src/parser.tab.c"
    break;

  case 54: /* stmt: VAR IDENT COLON type ASSIGN expr SEMICOLON  */
#line 313 "src/parser.y"
        { (yyval.stmt) = stmt_new_var_decl((yyvsp[-5].str), (yyvsp[-3].type), (yyvsp[-1].expr)); (yyval.stmt)->struct_name = g_last_type_name; (yyval.stmt)->elem_type = g_last_elem_type; (yyval.stmt)->elem_elem_type = g_last_elem_elem_type; }
#line 1858 "src/parser.tab.c"
    break;

  case 55: /* stmt: VAR IDENT COLON type SEMICOLON  */
#line 315 "src/parser.y"
        { /* M8: initializer-less declaration (e.g. `var p: Point;`) */
          (yyval.stmt) = stmt_new_var_decl((yyvsp[-3].str), (yyvsp[-1].type), NULL); (yyval.stmt)->struct_name = g_last_type_name; (yyval.stmt)->elem_type = g_last_elem_type; (yyval.stmt)->elem_elem_type = g_last_elem_elem_type; }
#line 1865 "src/parser.tab.c"
    break;

  case 56: /* stmt: CONST IDENT COLON type ASSIGN expr SEMICOLON  */
#line 318 "src/parser.y"
        { (yyval.stmt) = stmt_new_const_decl((yyvsp[-5].str), (yyvsp[-3].type), (yyvsp[-1].expr)); (yyval.stmt)->struct_name = g_last_type_name; (yyval.stmt)->elem_type = g_last_elem_type; (yyval.stmt)->elem_elem_type = g_last_elem_elem_type; }
#line 1871 "src/parser.tab.c"
    break;

  case 57: /* stmt: RETURN SEMICOLON  */
#line 320 "src/parser.y"
        { (yyval.stmt) = stmt_new_return(NULL); }
#line 1877 "src/parser.tab.c"
    break;

  case 58: /* stmt: RETURN expr SEMICOLON  */
#line 322 "src/parser.y"
        { (yyval.stmt) = stmt_new_return((yyvsp[-1].expr)); }
#line 1883 "src/parser.tab.c"
    break;

  case 59: /* stmt: expr SEMICOLON  */
#line 324 "src/parser.y"
        { (yyval.stmt) = stmt_new_expr((yyvsp[-1].expr)); }
#line 1889 "src/parser.tab.c"
    break;

  case 60: /* stmt: IF LPAREN expr RPAREN block else_clause  */
#line 326 "src/parser.y"
        { (yyval.stmt) = stmt_new_if((yyvsp[-3].expr), (yyvsp[-1].stmt), (yyvsp[0].stmt)); }
#line 1895 "src/parser.tab.c"
    break;

  case 61: /* stmt: WHILE LPAREN expr RPAREN block  */
#line 328 "src/parser.y"
        { (yyval.stmt) = stmt_new_while((yyvsp[-2].expr), (yyvsp[0].stmt)); }
#line 1901 "src/parser.tab.c"
    break;

  case 62: /* stmt: FOR LPAREN for_init expr SEMICOLON for_post RPAREN block  */
#line 330 "src/parser.y"
        { (yyval.stmt) = stmt_new_for((yyvsp[-5].stmt), (yyvsp[-4].expr), (yyvsp[-2].stmt), (yyvsp[0].stmt)); }
#line 1907 "src/parser.tab.c"
    break;

  case 63: /* stmt: FOR LPAREN IDENT IN expr RPAREN block  */
#line 332 "src/parser.y"
        { (yyval.stmt) = stmt_new_for_in((yyvsp[-4].str), (yyvsp[-2].expr), (yyvsp[0].stmt)); }
#line 1913 "src/parser.tab.c"
    break;

  case 64: /* stmt: BREAK SEMICOLON  */
#line 334 "src/parser.y"
        { (yyval.stmt) = stmt_new_break(); }
#line 1919 "src/parser.tab.c"
    break;

  case 65: /* stmt: CONTINUE SEMICOLON  */
#line 336 "src/parser.y"
        { (yyval.stmt) = stmt_new_continue(); }
#line 1925 "src/parser.tab.c"
    break;

  case 66: /* stmt: SWITCH LPAREN expr RPAREN LBRACE case_list RBRACE  */
#line 338 "src/parser.y"
        { (yyval.stmt) = stmt_new_switch((yyvsp[-4].expr), (yyvsp[-1].caseclause)); }
#line 1931 "src/parser.tab.c"
    break;

  case 67: /* stmt: IDENT ASSIGN expr SEMICOLON  */
#line 340 "src/parser.y"
        { (yyval.stmt) = stmt_new_assign((yyvsp[-3].str), (yyvsp[-1].expr)); }
#line 1937 "src/parser.tab.c"
    break;

  case 68: /* stmt: IDENT LBRACKET expr RBRACKET ASSIGN expr SEMICOLON  */
#line 342 "src/parser.y"
        { (yyval.stmt) = stmt_new_index_assign(expr_new_ident((yyvsp[-6].str)), (yyvsp[-4].expr), (yyvsp[-1].expr)); }
#line 1943 "src/parser.tab.c"
    break;

  case 69: /* stmt: STAR expr ASSIGN expr SEMICOLON  */
#line 344 "src/parser.y"
        { (yyval.stmt) = stmt_new_deref_assign((yyvsp[-3].expr), (yyvsp[-1].expr)); }
#line 1949 "src/parser.tab.c"
    break;

  case 70: /* stmt: IDENT DOT IDENT ASSIGN expr SEMICOLON  */
#line 346 "src/parser.y"
        { (yyval.stmt) = stmt_new_field_assign(expr_new_ident((yyvsp[-5].str)), (yyvsp[-3].str), (yyvsp[-1].expr)); }
#line 1955 "src/parser.tab.c"
    break;

  case 71: /* stmt: IDENT INC SEMICOLON  */
#line 348 "src/parser.y"
        { (yyval.stmt) = stmt_new_inc((yyvsp[-2].str)); }
#line 1961 "src/parser.tab.c"
    break;

  case 72: /* stmt: IDENT DEC SEMICOLON  */
#line 350 "src/parser.y"
        { (yyval.stmt) = stmt_new_dec((yyvsp[-2].str)); }
#line 1967 "src/parser.tab.c"
    break;

  case 73: /* for_init: VAR IDENT COLON type ASSIGN expr SEMICOLON  */
#line 355 "src/parser.y"
        { (yyval.stmt) = stmt_new_var_decl((yyvsp[-5].str), (yyvsp[-3].type), (yyvsp[-1].expr)); (yyval.stmt)->struct_name = g_last_type_name; (yyval.stmt)->elem_type = g_last_elem_type; (yyval.stmt)->elem_elem_type = g_last_elem_elem_type; }
#line 1973 "src/parser.tab.c"
    break;

  case 74: /* for_post: IDENT ASSIGN expr  */
#line 359 "src/parser.y"
                          { (yyval.stmt) = stmt_new_assign((yyvsp[-2].str), (yyvsp[0].expr)); }
#line 1979 "src/parser.tab.c"
    break;

  case 75: /* for_post: IDENT INC  */
#line 360 "src/parser.y"
                          { (yyval.stmt) = stmt_new_inc((yyvsp[-1].str)); }
#line 1985 "src/parser.tab.c"
    break;

  case 76: /* for_post: IDENT DEC  */
#line 361 "src/parser.y"
                          { (yyval.stmt) = stmt_new_dec((yyvsp[-1].str)); }
#line 1991 "src/parser.tab.c"
    break;

  case 77: /* case_list: %empty  */
#line 368 "src/parser.y"
        { (yyval.caseclause) = NULL; }
#line 1997 "src/parser.tab.c"
    break;

  case 78: /* case_list: CASE expr COLON case_body case_list  */
#line 370 "src/parser.y"
        { (yyval.caseclause) = case_clause_new((yyvsp[-3].expr), (yyvsp[-1].stmt), (yyvsp[0].caseclause)); }
#line 2003 "src/parser.tab.c"
    break;

  case 79: /* case_list: DEFAULT COLON case_body case_list  */
#line 372 "src/parser.y"
        { (yyval.caseclause) = case_clause_new(NULL, (yyvsp[-1].stmt), (yyvsp[0].caseclause)); }
#line 2009 "src/parser.tab.c"
    break;

  case 80: /* case_body: %empty  */
#line 377 "src/parser.y"
        { (yyval.stmt) = NULL; }
#line 2015 "src/parser.tab.c"
    break;

  case 81: /* case_body: stmt case_body  */
#line 379 "src/parser.y"
        { (yyvsp[-1].stmt)->next = (yyvsp[0].stmt); (yyval.stmt) = (yyvsp[-1].stmt); }
#line 2021 "src/parser.tab.c"
    break;

  case 82: /* block: LBRACE stmt_list RBRACE  */
#line 383 "src/parser.y"
                                { (yyval.stmt) = (yyvsp[-1].stmt); }
#line 2027 "src/parser.tab.c"
    break;

  case 83: /* else_clause: %empty  */
#line 387 "src/parser.y"
                                        { (yyval.stmt) = NULL; }
#line 2033 "src/parser.tab.c"
    break;

  case 84: /* else_clause: ELSE IF LPAREN expr RPAREN block else_clause  */
#line 389 "src/parser.y"
        { (yyval.stmt) = stmt_new_if((yyvsp[-3].expr), (yyvsp[-1].stmt), (yyvsp[0].stmt)); }
#line 2039 "src/parser.tab.c"
    break;

  case 85: /* else_clause: ELSE block  */
#line 391 "src/parser.y"
        { (yyval.stmt) = (yyvsp[0].stmt); }
#line 2045 "src/parser.tab.c"
    break;

  case 86: /* type: base_type  */
#line 395 "src/parser.y"
                                  { (yyval.type) = (yyvsp[0].type); g_last_elem_type = TYPE_VOID; g_last_elem_elem_type = TYPE_VOID; }
#line 2051 "src/parser.tab.c"
    break;

  case 87: /* type: base_type STAR  */
#line 397 "src/parser.y"
        {
          /* M26: generalized pointer type — any base_type followed by `*`,
             replacing the old hardcoded INT_TYPE STAR / BYTE_TYPE STAR /
             CHAR_TYPE STAR rules with one rule that covers every pointee
             type at once (int8..uint64, float, double, bool, struct, enum,
             plus the original int/byte/char). void* still parses (VOID is
             a base_type) but is rejected in semantic.c via
             type_ptr_ok(TYPE_VOID) == false, since excluding it here would
             mean duplicating this whole rule. */
          (yyval.type) = TYPE_PTR;
          g_last_elem_type = (yyvsp[-1].type);
          g_last_elem_elem_type = TYPE_VOID;
          g_last_type_name = NULL;
        }
#line 2070 "src/parser.tab.c"
    break;

  case 88: /* type: base_type STAR STAR  */
#line 412 "src/parser.y"
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
          (yyval.type) = TYPE_PTR;
          g_last_elem_type = TYPE_PTR;
          g_last_elem_elem_type = (yyvsp[-2].type);
          g_last_type_name = NULL;
        }
#line 2091 "src/parser.tab.c"
    break;

  case 89: /* type: INT_TYPE LBRACKET RBRACKET  */
#line 428 "src/parser.y"
                                 { (yyval.type) = TYPE_INT_ARRAY; g_last_type_name = NULL; g_last_elem_type = TYPE_VOID; g_last_elem_elem_type = TYPE_VOID; }
#line 2097 "src/parser.tab.c"
    break;

  case 90: /* type: LIST LT INT_TYPE GT  */
#line 429 "src/parser.y"
                                 { (yyval.type) = TYPE_INT_LIST; g_last_type_name = NULL; g_last_elem_type = TYPE_VOID; g_last_elem_elem_type = TYPE_VOID; }
#line 2103 "src/parser.tab.c"
    break;

  case 91: /* type: LIST LT BYTE_TYPE GT  */
#line 430 "src/parser.y"
                                 { (yyval.type) = TYPE_BYTE_LIST; g_last_type_name = NULL; g_last_elem_type = TYPE_VOID; g_last_elem_elem_type = TYPE_VOID; }
#line 2109 "src/parser.tab.c"
    break;

  case 92: /* type: FN LPAREN type_list_opt RPAREN COLON type  */
#line 432 "src/parser.y"
        {
          /* M11: fn(...)::ret type annotation. No closures; just a bare
             function-pointer-like value. The specific param/return types
             aren't retained on TypeKind itself (it's a flat enum) — call
             sites are checked structurally against the actual function
             assigned, via FnSig, at the point of an indirect call. */
          (yyval.type) = TYPE_FN;
          g_last_type_name = NULL;
          g_last_elem_type = TYPE_VOID;
          g_last_elem_elem_type = TYPE_VOID;
        }
#line 2125 "src/parser.tab.c"
    break;

  case 93: /* base_type: INT_TYPE  */
#line 449 "src/parser.y"
                                { (yyval.type) = TYPE_INT; g_last_type_name = NULL; }
#line 2131 "src/parser.tab.c"
    break;

  case 94: /* base_type: BOOL_TYPE  */
#line 450 "src/parser.y"
                                { (yyval.type) = TYPE_BOOL; g_last_type_name = NULL; }
#line 2137 "src/parser.tab.c"
    break;

  case 95: /* base_type: STRING_TYPE  */
#line 451 "src/parser.y"
                                 { (yyval.type) = TYPE_STRING; g_last_type_name = NULL; }
#line 2143 "src/parser.tab.c"
    break;

  case 96: /* base_type: VOID  */
#line 452 "src/parser.y"
                                 { (yyval.type) = TYPE_VOID; g_last_type_name = NULL; }
#line 2149 "src/parser.tab.c"
    break;

  case 97: /* base_type: BYTE_TYPE  */
#line 453 "src/parser.y"
                                 { (yyval.type) = TYPE_BYTE; g_last_type_name = NULL; }
#line 2155 "src/parser.tab.c"
    break;

  case 98: /* base_type: CHAR_TYPE  */
#line 454 "src/parser.y"
                                 { (yyval.type) = TYPE_BYTE; g_last_type_name = NULL; }
#line 2161 "src/parser.tab.c"
    break;

  case 99: /* base_type: INT8_TYPE  */
#line 455 "src/parser.y"
                                 { (yyval.type) = TYPE_INT8; g_last_type_name = NULL; }
#line 2167 "src/parser.tab.c"
    break;

  case 100: /* base_type: INT16_TYPE  */
#line 456 "src/parser.y"
                                 { (yyval.type) = TYPE_INT16; g_last_type_name = NULL; }
#line 2173 "src/parser.tab.c"
    break;

  case 101: /* base_type: INT32_TYPE  */
#line 457 "src/parser.y"
                                 { (yyval.type) = TYPE_INT32; g_last_type_name = NULL; }
#line 2179 "src/parser.tab.c"
    break;

  case 102: /* base_type: INT64_TYPE  */
#line 458 "src/parser.y"
                                 { (yyval.type) = TYPE_INT64; g_last_type_name = NULL; }
#line 2185 "src/parser.tab.c"
    break;

  case 103: /* base_type: UINT8_TYPE  */
#line 459 "src/parser.y"
                                 { (yyval.type) = TYPE_UINT8; g_last_type_name = NULL; }
#line 2191 "src/parser.tab.c"
    break;

  case 104: /* base_type: UINT16_TYPE  */
#line 460 "src/parser.y"
                                 { (yyval.type) = TYPE_UINT16; g_last_type_name = NULL; }
#line 2197 "src/parser.tab.c"
    break;

  case 105: /* base_type: UINT32_TYPE  */
#line 461 "src/parser.y"
                                 { (yyval.type) = TYPE_UINT32; g_last_type_name = NULL; }
#line 2203 "src/parser.tab.c"
    break;

  case 106: /* base_type: UINT64_TYPE  */
#line 462 "src/parser.y"
                                 { (yyval.type) = TYPE_UINT64; g_last_type_name = NULL; }
#line 2209 "src/parser.tab.c"
    break;

  case 107: /* base_type: FLOAT_TYPE  */
#line 463 "src/parser.y"
                                 { (yyval.type) = TYPE_FLOAT; g_last_type_name = NULL; }
#line 2215 "src/parser.tab.c"
    break;

  case 108: /* base_type: DOUBLE_TYPE  */
#line 464 "src/parser.y"
                                 { (yyval.type) = TYPE_DOUBLE; g_last_type_name = NULL; }
#line 2221 "src/parser.tab.c"
    break;

  case 109: /* base_type: IDENT  */
#line 466 "src/parser.y"
        {
          /* M8: user-defined struct/enum type name */
          if (struct_find((yyvsp[0].str))) {
              (yyval.type) = TYPE_STRUCT;
          } else if (enum_find((yyvsp[0].str))) {
              (yyval.type) = TYPE_ENUM;
          } else {
              yyerror("unknown type name");
              (yyval.type) = TYPE_INT;
          }
          g_last_type_name = (yyvsp[0].str);
        }
#line 2238 "src/parser.tab.c"
    break;

  case 110: /* type_list_opt: %empty  */
#line 481 "src/parser.y"
                     { (yyval.param) = NULL; }
#line 2244 "src/parser.tab.c"
    break;

  case 111: /* type_list_opt: type_list  */
#line 482 "src/parser.y"
                       { (yyval.param) = (yyvsp[0].param); }
#line 2250 "src/parser.tab.c"
    break;

  case 112: /* type_list: type  */
#line 486 "src/parser.y"
                            { (yyval.param) = param_new(NULL, (yyvsp[0].type), NULL); }
#line 2256 "src/parser.tab.c"
    break;

  case 113: /* type_list: type COMMA type_list  */
#line 487 "src/parser.y"
                            { (yyval.param) = param_new(NULL, (yyvsp[-2].type), (yyvsp[0].param)); }
#line 2262 "src/parser.tab.c"
    break;

  case 114: /* expr_list_opt: %empty  */
#line 491 "src/parser.y"
                    { (yyval.exprlist) = NULL; }
#line 2268 "src/parser.tab.c"
    break;

  case 115: /* expr_list_opt: expr_list  */
#line 492 "src/parser.y"
                     { (yyval.exprlist) = (yyvsp[0].exprlist); }
#line 2274 "src/parser.tab.c"
    break;

  case 116: /* expr_list: expr  */
#line 496 "src/parser.y"
                               { (yyval.exprlist) = exprlist_new((yyvsp[0].expr), NULL); }
#line 2280 "src/parser.tab.c"
    break;

  case 117: /* expr_list: expr COMMA expr_list  */
#line 497 "src/parser.y"
                               { (yyval.exprlist) = exprlist_new((yyvsp[-2].expr), (yyvsp[0].exprlist)); }
#line 2286 "src/parser.tab.c"
    break;

  case 118: /* struct_literal_fields: %empty  */
#line 501 "src/parser.y"
                { (yyval.structlit) = NULL; }
#line 2292 "src/parser.tab.c"
    break;

  case 120: /* struct_literal_fields: struct_literal_fields COMMA struct_literal_field  */
#line 503 "src/parser.y"
                                                     { (yyval.structlit) = (yyvsp[-2].structlit); (yyvsp[-2].structlit)->next = (yyvsp[0].structlit); }
#line 2298 "src/parser.tab.c"
    break;

  case 121: /* struct_literal_field: IDENT COLON expr  */
#line 507 "src/parser.y"
                     { (yyval.structlit) = struct_literal_field_new((yyvsp[-2].str), (yyvsp[0].expr), NULL); }
#line 2304 "src/parser.tab.c"
    break;

  case 122: /* expr: INT_LITERAL  */
#line 511 "src/parser.y"
                               { (yyval.expr) = expr_new_int((yyvsp[0].num)); }
#line 2310 "src/parser.tab.c"
    break;

  case 123: /* expr: FLOAT_LITERAL  */
#line 512 "src/parser.y"
                               { (yyval.expr) = expr_new_float((yyvsp[0].fnum)); }
#line 2316 "src/parser.tab.c"
    break;

  case 124: /* expr: CHAR_LITERAL  */
#line 513 "src/parser.y"
                               { (yyval.expr) = expr_new_char((yyvsp[0].num)); }
#line 2322 "src/parser.tab.c"
    break;

  case 125: /* expr: STRING_LITERAL  */
#line 514 "src/parser.y"
                               { (yyval.expr) = expr_new_string((yyvsp[0].str)); }
#line 2328 "src/parser.tab.c"
    break;

  case 126: /* expr: IDENT  */
#line 515 "src/parser.y"
                               { (yyval.expr) = expr_new_ident((yyvsp[0].str)); }
#line 2334 "src/parser.tab.c"
    break;

  case 127: /* expr: IO COLONCOLON IDENT LPAREN expr_list_opt RPAREN  */
#line 517 "src/parser.y"
        {
            /* M16/M17/M18: io::out(...), io::in(), and the io:: file
               syscall wrappers (open/read/write/close).
               - io::out(x)        -> plain print (unchanged behavior)
               - io::out(fmt, ...) -> "{}"-templated formatted print
               - io::in()          -> read one line from stdin, as a string
               - io::open/read/write/close -> thin raw-syscall wrappers,
                 argument/type checking done in semantic.c like the
                 sysprog:: builtins. */
            if (strcmp((yyvsp[-3].str), "out") == 0) {
                int argc = 0;
                for (ExprList *l = (yyvsp[-1].exprlist); l; l = l->next) argc++;
                if (argc == 0) {
                    yyerror("io::out requires at least one argument");
                    (yyval.expr) = expr_new_io_out(expr_new_int(0));
                } else if (argc == 1) {
                    (yyval.expr) = expr_new_io_out((yyvsp[-1].exprlist)->expr);
                } else {
                    (yyval.expr) = expr_new_io_format((yyvsp[-1].exprlist)->expr, (yyvsp[-1].exprlist)->next);
                }
            } else if (strcmp((yyvsp[-3].str), "open") == 0 || strcmp((yyvsp[-3].str), "read") == 0 ||
                       strcmp((yyvsp[-3].str), "write") == 0 || strcmp((yyvsp[-3].str), "close") == 0) {
                (yyval.expr) = expr_new_io_syscall((yyvsp[-3].str), (yyvsp[-1].exprlist));
            } else if (strcmp((yyvsp[-3].str), "errno") == 0) {
                if ((yyvsp[-1].exprlist) != NULL) {
                    yyerror("io::errno takes no arguments");
                }
                (yyval.expr) = expr_new_io_syscall((yyvsp[-3].str), NULL);
            } else {
                yyerror("only io::out, io::in, io::open, io::read, io::write, io::close, io::errno are supported so far");
                (yyval.expr) = expr_new_io_in();
            }
        }
#line 2372 "src/parser.tab.c"
    break;

  case 128: /* expr: IO COLONCOLON IN LPAREN expr_list_opt RPAREN  */
#line 551 "src/parser.y"
        {
            /* `in` is a reserved keyword (for-in), so it can't come through
               as an IDENT here the way "out" does — needs its own rule. */
            if ((yyvsp[-1].exprlist) != NULL) {
                yyerror("io::in takes no arguments");
            }
            (yyval.expr) = expr_new_io_in();
        }
#line 2385 "src/parser.tab.c"
    break;

  case 129: /* expr: IDENT LPAREN expr_list_opt RPAREN  */
#line 560 "src/parser.y"
        { (yyval.expr) = expr_new_call((yyvsp[-3].str), (yyvsp[-1].exprlist)); }
#line 2391 "src/parser.tab.c"
    break;

  case 130: /* expr: LBRACKET expr_list_opt RBRACKET  */
#line 562 "src/parser.y"
        {
            int n = 0;
            for (ExprList *l = (yyvsp[-1].exprlist); l; l = l->next) n++;
            (yyval.expr) = expr_new_array_literal((yyvsp[-1].exprlist), n);
        }
#line 2401 "src/parser.tab.c"
    break;

  case 131: /* expr: expr LBRACKET expr RBRACKET  */
#line 568 "src/parser.y"
        { (yyval.expr) = expr_new_index((yyvsp[-3].expr), (yyvsp[-1].expr)); }
#line 2407 "src/parser.tab.c"
    break;

  case 132: /* expr: AMPERSAND expr  */
#line 569 "src/parser.y"
                                { (yyval.expr) = expr_new_addr_of((yyvsp[0].expr)); }
#line 2413 "src/parser.tab.c"
    break;

  case 133: /* expr: STAR expr  */
#line 570 "src/parser.y"
                                { (yyval.expr) = expr_new_deref((yyvsp[0].expr)); }
#line 2419 "src/parser.tab.c"
    break;

  case 134: /* expr: MINUS expr  */
#line 572 "src/parser.y"
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
            (yyval.expr) = expr_new_neg((yyvsp[0].expr));
        }
#line 2443 "src/parser.tab.c"
    break;

  case 135: /* expr: IDENT DOT IDENT  */
#line 591 "src/parser.y"
                      { (yyval.expr) = expr_new_field(expr_new_ident((yyvsp[-2].str)), (yyvsp[0].str)); }
#line 2449 "src/parser.tab.c"
    break;

  case 136: /* expr: IDENT COLONCOLON IDENT  */
#line 592 "src/parser.y"
                             { (yyval.expr) = expr_new_enum_literal((yyvsp[-2].str), (yyvsp[0].str)); }
#line 2455 "src/parser.tab.c"
    break;

  case 137: /* expr: IDENT LBRACE struct_literal_fields RBRACE  */
#line 593 "src/parser.y"
                                                { (yyval.expr) = expr_new_struct_literal((yyvsp[-3].str), (yyvsp[-1].structlit)); }
#line 2461 "src/parser.tab.c"
    break;

  case 138: /* expr: expr EQ expr  */
#line 594 "src/parser.y"
                     { (yyval.expr) = expr_new_binop(OP_EQ, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2467 "src/parser.tab.c"
    break;

  case 139: /* expr: expr NE expr  */
#line 595 "src/parser.y"
                     { (yyval.expr) = expr_new_binop(OP_NE, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2473 "src/parser.tab.c"
    break;

  case 140: /* expr: expr LT expr  */
#line 596 "src/parser.y"
                     { (yyval.expr) = expr_new_binop(OP_LT, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2479 "src/parser.tab.c"
    break;

  case 141: /* expr: expr LE expr  */
#line 597 "src/parser.y"
                     { (yyval.expr) = expr_new_binop(OP_LE, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2485 "src/parser.tab.c"
    break;

  case 142: /* expr: expr GT expr  */
#line 598 "src/parser.y"
                     { (yyval.expr) = expr_new_binop(OP_GT, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2491 "src/parser.tab.c"
    break;

  case 143: /* expr: expr GE expr  */
#line 599 "src/parser.y"
                     { (yyval.expr) = expr_new_binop(OP_GE, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2497 "src/parser.tab.c"
    break;

  case 144: /* expr: expr PLUS expr  */
#line 600 "src/parser.y"
                      { (yyval.expr) = expr_new_binop(OP_ADD, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2503 "src/parser.tab.c"
    break;

  case 145: /* expr: expr MINUS expr  */
#line 601 "src/parser.y"
                      { (yyval.expr) = expr_new_binop(OP_SUB, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2509 "src/parser.tab.c"
    break;

  case 146: /* expr: expr STAR expr  */
#line 602 "src/parser.y"
                      { (yyval.expr) = expr_new_binop(OP_MUL, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2515 "src/parser.tab.c"
    break;

  case 147: /* expr: expr SLASH expr  */
#line 603 "src/parser.y"
                      { (yyval.expr) = expr_new_binop(OP_DIV, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2521 "src/parser.tab.c"
    break;

  case 148: /* expr: LPAREN expr RPAREN  */
#line 604 "src/parser.y"
                         { (yyval.expr) = (yyvsp[-1].expr); }
#line 2527 "src/parser.tab.c"
    break;


#line 2531 "src/parser.tab.c"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      yyerror (YY_("syntax error"));
    }

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif

  return yyresult;
}

#line 607 "src/parser.y"


void yyerror(const char *msg) {
    fprintf(stderr, "parse error: %s\n", msg);
}
