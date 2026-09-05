#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"

/* Emits QBE IL for the program to the given file path. Returns 1 on success. */
int codegen_emit(Program *prog, const char *out_path);

#endif
