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

/* Resets per-compile module-resolution state (module table, extern-sig and
   variadic-alias dedup tables, global-alias list) to its initial values.
   resolve_modules() calls this itself, so a normal one-shot CLI run never
   needs to call it directly; an embedder (LSP, watch-mode, build-server)
   that reuses the process across multiple compiles may call it standalone
   between runs. Does NOT reset the module_set_home/auto_std/allow_shim
   configuration below -- that is process-level config, not per-compile
   state; call those setters again if it needs to change. */
void module_reset(void);

/* M41: yerlesik kutuphaneler (std/std.rapid) icin yedek kok dizin. */
void module_set_home(const char *dir);

/* M41: std/std.rapid her programa otomatik dahil edilir (varsayilan 1); `-nostd` ile kapatilir. */
void module_set_auto_std(int on);

/* Production-hardening fix: `shim "path.c";` derleme zamaninda o C
   dosyasini otomatik `cc -c` ile derleyip son linke ekler (bkz. module.c
   scan_shim_decls). main.c'nin command-injection sertlestirmesi (M38-A)
   bunu etkilemez: shim orada bir shell komutu degil, gercek, derlenip
   PROGRAMA GOMULEN bir C kaynak dosyasidir — yani "bu .rapid dosyasini
   derle" isteginin kendisi, guvenmediginiz bir .rapid dosyasindan gelen
   keyfi C kodunu derleyip calistirilabilir hale getirmenin bir yolu olur.
   Varsayilan olarak KAPALI: bir shim bulunursa resolve_modules/scan_shim_decls
   acik bir hata basar ve derleme durur, `-allow-shim` bayragi verilmedikce.
   Boylece "üçüncü taraftan gelen bir .rapid dosyasini hic şüphelenmeden
   derle" senaryosu guvenli varsayilana sahip olur; shim'e gercekten
   ihtiyac duyan (kendi FFI shim'ini yazan) kullanici bunu bilerek ac,ar. */
void module_set_allow_shim(int on);

#endif
