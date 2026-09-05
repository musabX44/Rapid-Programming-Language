#include "ast.h"
#include "semantic.h"
#include "codegen.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>

extern FILE *yyin;
int yyparse(void);

/* M5: directory containing the running rapidc executable, so runtime.o can
   be located regardless of the caller's current working directory. */
static void get_exe_dir(char *buf, size_t bufsz) {
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) {
        snprintf(buf, bufsz, ".");
        return;
    }
    path[n] = '\0';
    snprintf(buf, bufsz, "%s", dirname(path));
}

static int run_cmd(const char *cmd) {
    fprintf(stderr, "+ %s\n", cmd);
    int rc = system(cmd);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: rapidc <input.rapid> [-o output]\n");
        return 1;
    }

    const char *input_path = NULL;
    const char *output_path = "a.out";
    /* Demo/deneme kolaylığı: `-l<lib>` (link "..."; ile aynı iş, komut
       satırından) ve `-obj <file.o>` (rapidc'nin derleyemediği bir C shim
       .o'sunu son link adımına eklemek için) argümanları. En fazla 8 tane
       kabul edilir. Kod üretimini etkilemez, yalnızca son `cc` komutuna
       eklenir. */
    char extra_flags[PATH_MAX];
    extra_flags[0] = '\0';

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            size_t used = strlen(extra_flags);
            snprintf(extra_flags + used, sizeof(extra_flags) - used, " -l%s", argv[++i]);
        } else if (strcmp(argv[i], "-obj") == 0 && i + 1 < argc) {
            size_t used = strlen(extra_flags);
            snprintf(extra_flags + used, sizeof(extra_flags) - used, " %s", argv[++i]);
        } else {
            input_path = argv[i];
        }
    }

    if (!input_path) {
        fprintf(stderr, "rapidc: no input file\n");
        return 1;
    }

    /* M20: resolve `use "file.rapid";` imports (recursively, with alias
       renaming and private-symbol mangling) into one merged source blob
       before handing anything to the lexer — the rest of the pipeline
       (parser, semantic check, codegen) is unchanged and simply sees what
       looks like a single, larger .rapid file ("basit dosya-bazlı derleme
       birleştirme": tek IR dosyasına linkleme happens implicitly because
       there's only ever one AST/codegen pass over the merged text). */
    char *merged_source = resolve_modules(input_path);
    if (!merged_source) {
        fprintf(stderr, "rapidc: module resolution failed\n");
        return 1;
    }
    if (getenv("RAPIDC_DUMP_MERGED")) {
        fprintf(stderr, "----- merged source -----\n%s\n----- end -----\n", merged_source);
    }

    yyin = fmemopen(merged_source, strlen(merged_source), "r");
    if (!yyin) {
        fprintf(stderr, "rapidc: cannot open input file %s\n", input_path);
        free(merged_source);
        return 1;
    }

    if (yyparse() != 0) {
        fprintf(stderr, "rapidc: parse failed\n");
        free(merged_source);
        return 1;
    }
    free(merged_source); /* flex has already strdup'd every token it kept */

    if (!semantic_check(&g_program)) {
        fprintf(stderr, "rapidc: semantic check failed\n");
        return 1;
    }

    char il_path[256];
    snprintf(il_path, sizeof(il_path), "/tmp/rapidc_%d.ssa", getpid());
    if (!codegen_emit(&g_program, il_path)) {
        fprintf(stderr, "rapidc: codegen failed\n");
        /* codegen_emit may have created/partially written il_path before
           failing; clean it up rather than leaving it behind in /tmp. */
        unlink(il_path);
        return 1;
    }

    char asm_path[256];
    snprintf(asm_path, sizeof(asm_path), "/tmp/rapidc_%d.s", getpid());

    char cmd[PATH_MAX * 3];
    snprintf(cmd, sizeof(cmd), "qbe -o %s %s", asm_path, il_path);
    if (run_cmd(cmd) != 0) {
        fprintf(stderr, "rapidc: qbe backend failed\n");
        /* Every exit path past this point must clean up both temp files
           (previously only the success path at the end did, so a failing
           qbe/link step left .ssa/.s files behind in /tmp on every
           failed build). */
        unlink(il_path);
        unlink(asm_path);
        return 1;
    }

    /* M5: link in the small C runtime helper (string concatenation, etc).
       runtime.o is built alongside rapidc itself (see Makefile) and lives
       next to the rapidc binary. */
    char exe_dir[PATH_MAX];
    get_exe_dir(exe_dir, sizeof(exe_dir));
    char runtime_path[PATH_MAX + 32];
    snprintf(runtime_path, sizeof(runtime_path), "%s/runtime.o", exe_dir);

    /* M31: `shim "path.c";` directives collected during module resolution
       (module.c's scan_shim_decls) become compiled `.o` files added to the
       final link command, exactly like runtime.o already is — this is
       what lets shim-backed FFI code (ffi/sqlite3_shim.c, std/net_shim/
       net_shim.c, ...) build with a single `rapidc foo.rapid -o foo`
       instead of a separate manual `cc -c shim.c` step.

       Each shim is compiled to a `.o` sitting next to its own `.c` source
       (not /tmp) so repeated builds of the same program can skip
       recompiling a shim whose `.o` is already newer than its `.c` — the
       same "compile once, reuse if unchanged" rationale runtime.o already
       gets from being built once by the Makefile rather than on every
       rapidc invocation. Unlike runtime.o (built ahead of time by `make`),
       shims are arbitrary user-provided files discovered at compile time,
       so rapidc compiles them itself, on demand, right here. */
    char shim_obj_flags[PATH_MAX];
    shim_obj_flags[0] = '\0';
    for (LinkObj *obj = g_program.link_objs; obj; obj = obj->next) {
        char obj_path[PATH_MAX + 4];
        snprintf(obj_path, sizeof(obj_path), "%s.o", obj->path);

        struct stat src_st, obj_st;
        int need_build = 1;
        if (stat(obj->path, &src_st) == 0 && stat(obj_path, &obj_st) == 0) {
            if (obj_st.st_mtime >= src_st.st_mtime) need_build = 0;
        }

        if (need_build) {
            char shim_cmd[PATH_MAX * 2];
            snprintf(shim_cmd, sizeof(shim_cmd), "cc -c -o %s %s", obj_path, obj->path);
            if (run_cmd(shim_cmd) != 0) {
                fprintf(stderr, "rapidc: failed to compile shim %s\n", obj->path);
                unlink(il_path);
                unlink(asm_path);
                return 1;
            }
        }

        size_t used = strlen(shim_obj_flags);
        snprintf(shim_obj_flags + used, sizeof(shim_obj_flags) - used, " %s", obj_path);
    }

    /* M28: `link "name";` directives collected during parsing become
       trailing `-l<name>` flags on the link command, in source order.
       Appended (not prepended) so they follow the normal `cc` convention
       of listing libraries after the objects that need their symbols. */
    char link_flags[PATH_MAX];
    link_flags[0] = '\0';
    for (LinkLib *lib = g_program.link_libs; lib; lib = lib->next) {
        size_t used = strlen(link_flags);
        snprintf(link_flags + used, sizeof(link_flags) - used, " -l%s", lib->name);
    }

    snprintf(cmd, sizeof(cmd), "cc -no-pie %s %s%s%s%s -o %s", asm_path, runtime_path, shim_obj_flags, link_flags, extra_flags, output_path);
    if (run_cmd(cmd) != 0) {
        fprintf(stderr, "rapidc: assemble/link failed\n");
        unlink(il_path);
        unlink(asm_path);
        return 1;
    }

    unlink(il_path);
    unlink(asm_path);

    return 0;
}
