/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

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

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_SRC_PARSER_TAB_H_INCLUDED
# define YY_YY_SRC_PARSER_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    EXTERN = 258,                  /* EXTERN  */
    ELLIPSIS = 259,                /* ELLIPSIS  */
    LINK = 260,                    /* LINK  */
    SHIM = 261,                    /* SHIM  */
    FN = 262,                      /* FN  */
    EFN = 263,                     /* EFN  */
    VAR = 264,                     /* VAR  */
    CONST = 265,                   /* CONST  */
    RETURN = 266,                  /* RETURN  */
    VOID = 267,                    /* VOID  */
    INT_TYPE = 268,                /* INT_TYPE  */
    BOOL_TYPE = 269,               /* BOOL_TYPE  */
    STRING_TYPE = 270,             /* STRING_TYPE  */
    BYTE_TYPE = 271,               /* BYTE_TYPE  */
    CHAR_TYPE = 272,               /* CHAR_TYPE  */
    INT8_TYPE = 273,               /* INT8_TYPE  */
    INT16_TYPE = 274,              /* INT16_TYPE  */
    INT32_TYPE = 275,              /* INT32_TYPE  */
    INT64_TYPE = 276,              /* INT64_TYPE  */
    UINT8_TYPE = 277,              /* UINT8_TYPE  */
    UINT16_TYPE = 278,             /* UINT16_TYPE  */
    UINT32_TYPE = 279,             /* UINT32_TYPE  */
    UINT64_TYPE = 280,             /* UINT64_TYPE  */
    FLOAT_TYPE = 281,              /* FLOAT_TYPE  */
    DOUBLE_TYPE = 282,             /* DOUBLE_TYPE  */
    STRUCT = 283,                  /* STRUCT  */
    ENUM = 284,                    /* ENUM  */
    DOT = 285,                     /* DOT  */
    LIST = 286,                    /* LIST  */
    IF = 287,                      /* IF  */
    ELSE = 288,                    /* ELSE  */
    WHILE = 289,                   /* WHILE  */
    FOR = 290,                     /* FOR  */
    IN = 291,                      /* IN  */
    BREAK = 292,                   /* BREAK  */
    CONTINUE = 293,                /* CONTINUE  */
    SWITCH = 294,                  /* SWITCH  */
    CASE = 295,                    /* CASE  */
    DEFAULT = 296,                 /* DEFAULT  */
    IO = 297,                      /* IO  */
    COLONCOLON = 298,              /* COLONCOLON  */
    USE = 299,                     /* USE  */
    AS = 300,                      /* AS  */
    PRIVATE = 301,                 /* PRIVATE  */
    EQ = 302,                      /* EQ  */
    NE = 303,                      /* NE  */
    LT = 304,                      /* LT  */
    LE = 305,                      /* LE  */
    GT = 306,                      /* GT  */
    GE = 307,                      /* GE  */
    INC = 308,                     /* INC  */
    DEC = 309,                     /* DEC  */
    PLUS = 310,                    /* PLUS  */
    MINUS = 311,                   /* MINUS  */
    STAR = 312,                    /* STAR  */
    SLASH = 313,                   /* SLASH  */
    AMPERSAND = 314,               /* AMPERSAND  */
    UMINUS = 315,                  /* UMINUS  */
    LPAREN = 316,                  /* LPAREN  */
    RPAREN = 317,                  /* RPAREN  */
    LBRACE = 318,                  /* LBRACE  */
    RBRACE = 319,                  /* RBRACE  */
    LBRACKET = 320,                /* LBRACKET  */
    RBRACKET = 321,                /* RBRACKET  */
    COLON = 322,                   /* COLON  */
    SEMICOLON = 323,               /* SEMICOLON  */
    ASSIGN = 324,                  /* ASSIGN  */
    COMMA = 325,                   /* COMMA  */
    FATARROW = 326,                /* FATARROW  */
    INT_LITERAL = 327,             /* INT_LITERAL  */
    CHAR_LITERAL = 328,            /* CHAR_LITERAL  */
    FLOAT_LITERAL = 329,           /* FLOAT_LITERAL  */
    STRING_LITERAL = 330,          /* STRING_LITERAL  */
    IDENT = 331                    /* IDENT  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 37 "src/parser.y"

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

#line 156 "src/parser.tab.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_SRC_PARSER_TAB_H_INCLUDED  */
