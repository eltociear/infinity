%{

#include "parser.h"
#include "parser_result.h"
#include <sstream>

static thread_local std::stringstream string_buffer;

%}

%option reentrant
%option bison-bridge

%option noyywrap nounput
%option warn
%option never-interactive
%option batch
%option case-insensitive
%option bison-locations
%option verbose
%option debug

%option prefix="sql"

%x SINGLE_QUOTED_STRING

%%

[ \t\n]+        /* ignore \t\n and space */;

ALTER           { return ALTER; }
BIGINT          { return BIGINT; }
BOOLEAN         { return BOOLEAN; }
BOX             { return BOX; }
CHAR            { return CHAR; }
CIRCLE          { return CIRCLE; }
COLLECTION      { return COLLECTION; }
COPY            { return COPY; }
CREATE          { return CREATE; }
DATE            { return DATE; }
DATETIME        { return DATETIME; }
DOUBLE          { return DOUBLE; }
DECIMAL         { return DECIMAL; }
DELETE          { return DELETE; }
DELIMITER       { return DELIMITER; }
DESCRIBE        { return DESCRIBE; }
DROP            { return DROP; }
EXECUTE         { return EXECUTE; }
EXISTS          { return EXISTS; }
EXPLAIN         { return EXPLAIN; }
FLOAT           { return FLOAT; }
FORMAT          { return FORMAT; }
FROM            { return FROM; }
HEADER          { return HEADER; }
HUGEINT         { return HUGEINT; }
IF              { return IF; }
INSERT          { return INSERT; }
INTEGER         { return INTEGER; }
KEY             { return KEY; }
LINE            { return LINE; }
LSEG            { return LSEG; }
NULL            { return NULLABLE; }
NOT             { return NOT; }
PATH            { return PATH; }
POINT           { return POINT; }
POLYGON         { return POLYGON; }
PREPARE         { return PREPARE; }
PRIMARY         { return PRIMARY; }
REAL            { return REAL; }
SCHEMA          { return SCHEMA; }
SELECT          { return SELECT; }
SET             { return SET; }
SHOW            { return SHOW; }
SMALLINT        { return SMALLINT; }
TIME            { return TIME; }
TIMESTAMP       { return TIMESTAMP; }
TO              { return TO; }
TABLE           { return TABLE; }
TABLES          { return TABLES; }
TINYINT         { return TINYINT; }
UNIQUE          { return UNIQUE; }
UPDATE          { return UPDATE; }
UUID            { return UUID; }
VARCHAR         { return VARCHAR; }
WITH            { return WITH; }

[-+*/(){},.;<>=^%:?[\]|]    { return yytext[0]; }

[0-9]+"."[0-9]* |
"."[0-9]*  {
    yylval->double_value = atof(yytext);
    return DOUBLE_VALUE;
}

[0-9]+ {
    errno = 0;
    yylval->long_value = strtoll(yytext, nullptr, 0);
    if (errno) {
        return fprintf(stderr, "[SQL-Lexer-Error] Integer cannot be parsed - is it out of range?");
        return 0;
    }
    return LONG_VALUE;
}

[A-Za-z][A-Za-z0-9_]* {
    yylval->str_value = strdup(yytext);
    return IDENTIFIER;
}

\'                            { BEGIN SINGLE_QUOTED_STRING; string_buffer.clear(); string_buffer.str(""); }  // Clear strbuf manually, see #170
<SINGLE_QUOTED_STRING>\'\'    { string_buffer << '\''; }
<SINGLE_QUOTED_STRING>[^']*   { string_buffer << yytext; }
<SINGLE_QUOTED_STRING>\'      { BEGIN INITIAL; yylval->str_value = strdup(string_buffer.str().c_str()); return STRING; }
<SINGLE_QUOTED_STRING><<EOF>> { fprintf(stderr, "[SQL-Lexer-Error] Unterminated string\n"); return 0; }

. { fprintf(stderr, "[SQL-Lexer-Error] Unknown Character: %c\n", yytext[0]); return 0; }

%%

int yyerror(const char *msg) {
    fprintf(stderr, "[Why here?] %s\n",msg); return 0;
}