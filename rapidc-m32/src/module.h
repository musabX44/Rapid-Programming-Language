#ifndef MODULE_H
#define MODULE_H

/* M20: module system.
   resolve_modules() reads `entry_path`, follows every top-level
   `use "file.rapid";` / `use "file.rapid" as alias;` directive it finds
   (recursively, each imported file relative to the *importing* file's own
   directory), and returns a single freshly-malloc'd buffer containing the
   fully merged/mangled source text, ready to be fed to the lexer/parser as
   if it had always been one file ("basit dosya-bazlı derleme birleştirme").

   Returns NULL on error (missing file, import cycle, etc — message already
   printed to stderr).
*/
char *resolve_modules(const char *entry_path);

#endif
