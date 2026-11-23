#ifndef RAPIDC_FFI_CHECK_H
#define RAPIDC_FFI_CHECK_H

#include "ast.h"

/* M44: for every `extern fn` declared in prog whose symbol is also
   DEFINED (not just forward-declared/used) in one of the `shim "x.c";`
   files collected on prog->link_objs, cross-checks the declared rapidc
   signature's PARAMETER COUNT and COARSE PARAMETER CLASS (int-like /
   float-like / pointer-or-string-like) against what a light textual scan
   of the shim's own C function definition finds. This is NOT a real C
   parser (no preprocessor, no typedef resolution beyond a short known
   list) -- it is a best-effort consistency check that catches the
   common, costly mistake class (wrong arity; int declared where the
   shim takes a pointer, or vice versa) without requiring a full C
   front end or the real system header. extern fn symbols with no
   matching shim definition (e.g. plain libc functions reached via
   `link "m";`) are silently skipped -- nothing to check them against.

   Returns 1 if every cross-checked signature agrees, 0 if at least one
   mismatch was found (each mismatch is reported via sem_error-style
   output on stderr, same format/exit convention as semantic_check). */
int ffi_check_shim_signatures(Program *prog);

#endif
