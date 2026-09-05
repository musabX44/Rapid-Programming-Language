#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "ast.h"

/* Returns 1 if program is semantically valid, 0 otherwise (errors printed to stderr). */
int semantic_check(Program *prog);

#endif
