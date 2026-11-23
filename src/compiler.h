#ifndef COMPILER_H
#define COMPILER_H

/* Single-call convenience wrapper for an embedder (LSP, watch-mode,
   build-server) that keeps rapidc's compile pipeline loaded in the same
   process across multiple compiles and wants to reset ALL of it (module
   resolution, AST, semantic analysis, codegen) between runs with one call,
   rather than relying on each stage's own reset running automatically at
   its entry point (module_reset() in resolve_modules(), ast_reset() just
   before yyparse(), semantic_reset() in semantic_check(), codegen_reset()
   in codegen_emit()).

   A normal one-shot CLI run (rapidc's own main()) never needs this -- each
   stage already resets itself, and a fresh process starts with every
   global zeroed regardless. This exists for the embedding case the reset
   work as a whole is for: call it once before reusing the pipeline for a
   new compile.

   Does NOT touch module.c's process-level configuration (home directory /
   auto-std / allow-shim, set via module_set_home() / module_set_auto_std()
   / module_set_allow_shim()) -- that's caller configuration, not
   per-compile state. See module_reset()'s own doc comment in module.h for
   the full reasoning. */
void compiler_reset(void);

#endif
