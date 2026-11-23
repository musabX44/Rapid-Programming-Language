#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"

/* Emits QBE IL for the program to the given file path. Returns 1 on success. */
int codegen_emit(Program *prog, const char *out_path);

/* Resets all codegen global state (counters, string table, function table,
   loop stack, error flag) to its initial values. codegen_emit() calls this
   itself, so a normal one-shot CLI run never needs to call it directly; an
   embedder (LSP, watch-mode, build-server) that reuses the process across
   multiple compiles should also call it before the *first* compile if any
   other reset/init order matters, and may call it standalone to release the
   string-table memory between runs without immediately re-emitting. */
void codegen_reset(void);

#endif
