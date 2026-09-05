%{
#include "ast.h"
#include "parser.tab.h"
#include <string.h>

/* M5: decode a raw string literal's escape sequences (\n \t \" \\) into a
   freshly-allocated buffer. `raw` points at the char right after the opening
   quote, `len` is the length up to (not including) the closing quote. */
static char *decode_string_literal(const char *raw, int len) {
    char *buf = malloc(len + 1);
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (raw[i] == '\\' && i + 1 < len) {
            i++;
            switch (raw[i]) {
                case 'n': buf[j++] = '\n'; break;
                case 't': buf[j++] = '\t'; break;
                case '"': buf[j++] = '"'; break;
                case '\\': buf[j++] = '\\'; break;
                default: buf[j++] = raw[i]; break;
            }
        } else {
            buf[j++] = raw[i];
        }
    }
    buf[j] = '\0';
    return buf;
}
%}

%option noyywrap
%option yylineno

DIGIT   [0-9]
ID      [a-zA-Z_][a-zA-Z0-9_]*

%%

[ \t\r\n]+          { /* skip whitespace */ }
"//".*              { /* line comment */ }

"extern"            { return EXTERN; }
"link"              { return LINK; }
"shim"              { return SHIM; }
"efn"               { return EFN; }
"fn"                { return FN; }
"var"               { return VAR; }
"const"             { return CONST; }
"return"            { return RETURN; }
"void"              { return VOID; }
"int"               { return INT_TYPE; }
"bool"              { return BOOL_TYPE; }
"string"            { return STRING_TYPE; }
"byte"               { return BYTE_TYPE; }
"char"               { return CHAR_TYPE; }
"int8"               { return INT8_TYPE; }
"int16"              { return INT16_TYPE; }
"int32"              { return INT32_TYPE; }
"int64"              { return INT64_TYPE; }
"uint8"              { return UINT8_TYPE; }
"uint16"             { return UINT16_TYPE; }
"uint32"             { return UINT32_TYPE; }
"uint64"             { return UINT64_TYPE; }
"float"              { return FLOAT_TYPE; }
"double"             { return DOUBLE_TYPE; }
"struct"            { return STRUCT; }
"enum"              { return ENUM; }
"list"              { return LIST; }
"if"                { return IF; }
"else"              { return ELSE; }
"while"             { return WHILE; }
"for"               { return FOR; }
"in"                { return IN; }
"break"             { return BREAK; }
"continue"          { return CONTINUE; }
"switch"            { return SWITCH; }
"case"              { return CASE; }
"default"           { return DEFAULT; }

"use"               { return USE; }
"as"                { return AS; }
"private"           { return PRIVATE; }

"io"                { return IO; }
"::"                { return COLONCOLON; }
"=>"                { return FATARROW; }
"."                 { return DOT; }

"=="                { return EQ; }
"!="                { return NE; }
"<="                { return LE; }
">="                { return GE; }
"<"                 { return LT; }
">"                 { return GT; }
"++"                { return INC; }
"--"                { return DEC; }
"..."               { return ELLIPSIS; }
"+"                 { return PLUS; }
"-"                 { return MINUS; }
"*"                 { return STAR; }
"/"                 { return SLASH; }
"&"                 { return AMPERSAND; }

'\\n'               { yylval.num = '\n'; return CHAR_LITERAL; }
'\\t'               { yylval.num = '\t'; return CHAR_LITERAL; }
'\\''               { yylval.num = '\''; return CHAR_LITERAL; }
'\\\\'              { yylval.num = '\\'; return CHAR_LITERAL; }
'[^\\']'            { yylval.num = yytext[1]; return CHAR_LITERAL; }
{DIGIT}+"."{DIGIT}+  { yylval.fnum = atof(yytext); return FLOAT_LITERAL; }
{DIGIT}+            { yylval.num = atoll(yytext); return INT_LITERAL; }
\"([^"\\]|\\.)*\"   {
                        yylval.str = decode_string_literal(yytext + 1, yyleng - 2);
                        return STRING_LITERAL;
                    }
{ID}                { yylval.str = strdup(yytext); return IDENT; }

"("                 { return LPAREN; }
")"                 { return RPAREN; }
"{"                 { return LBRACE; }
"}"                 { return RBRACE; }
"["                 { return LBRACKET; }
"]"                 { return RBRACKET; }
":"                 { return COLON; }
";"                 { return SEMICOLON; }
"="                 { return ASSIGN; }
","                 { return COMMA; }

.                   { fprintf(stderr, "lexer: unexpected character '%s' at line %d\n", yytext, yylineno); }

%%
