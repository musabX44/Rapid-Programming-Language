#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "ast.h"

/* Returns 1 if program is semantically valid, 0 otherwise (errors printed to stderr). */
int semantic_check(Program *prog);

/* Resets all semantic-analysis global state (function table, error flag,
   current-function context, expr-depth guard) to its initial values.
   semantic_check() calls this itself, so a normal one-shot CLI run never
   needs to call it directly; an embedder (LSP, watch-mode, build-server)
   that reuses the process across multiple compiles may call it standalone
   between runs if it needs the state cleared without immediately
   re-checking. */
void semantic_reset(void);

/* Opt-in: if RAPIDC_DIAG_DEBUG is set in the environment, checks that
   diag.h's DIAG_HELP_TABLE needles still match wording actually present in
   semantic_c_path's source (pass NULL to default to "src/semantic.c").
   No-op otherwise. See diag.h's diag_selfcheck_help_table() for why this
   exists. Safe to call unconditionally -- it's the env var that gates it. */
void semantic_diag_selfcheck(const char *semantic_c_path);

#endif
