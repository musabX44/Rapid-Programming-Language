#include "ast.h"
#include "semantic.h"
#include "codegen.h"
#include "module.h"
#include "compiler.h"
#include "ffi_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>       /* _mktemp_s, _unlink, _access */
#  include <direct.h>
#  ifndef PATH_MAX
#    define PATH_MAX MAX_PATH
#  endif
#  define access(p,m) _access((p),(m))
#  define unlink(p)   _unlink(p)
#  define F_OK 0
#  define X_OK 1
/* dirname: strip last path component, return static buf */
static char *win_dirname(char *path) {
    static char buf[MAX_PATH];
    strncpy(buf, path, MAX_PATH - 1);
    buf[MAX_PATH - 1] = '\0';
    char *last = NULL;
    for (char *p = buf; *p; p++)
        if (*p == '/' || *p == '\\') last = p;
    if (last) *last = '\0';
    else { buf[0] = '.'; buf[1] = '\0'; }
    return buf;
}
#  define dirname win_dirname
#else
#  include <unistd.h>
#  include <libgen.h>
#  include <limits.h>
#  include <sys/wait.h>
#endif

extern FILE *yyin;
int yyparse(void);

/* Convenience wrapper for an embedder (LSP, watch-mode, build-server) that
   wants to reset the whole compiler's per-compile state with one call
   instead of relying on each stage to reset itself at its own entry
   point. A normal one-shot CLI run (this file's own main(), below) never
   calls this: module_reset()/ast_reset()/semantic_reset()/codegen_reset()
   already run automatically at the top of resolve_modules(), just before
   yyparse(), semantic_check(), and codegen_emit() respectively, which is
   enough for a fresh process where every global starts zeroed anyway.

   Order follows the pipeline (modules -> parse -> semantic -> codegen) for
   readability; no stage's reset here actually depends on an earlier one
   having run first. Does NOT touch module.c's process-level config (home
   dir / auto-std / allow-shim) -- see module_reset()'s own comment for
   why; call module_set_home() etc. again if an embedder needs to change
   that between compiles. */
void compiler_reset(void) {
    module_reset();
    ast_reset();
    semantic_reset();
    codegen_reset();
}

/* M5: directory containing the running rapidc executable, so runtime.o can
   be located regardless of the caller's current working directory. */
static void get_exe_dir(char *buf, size_t bufsz) {
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0) { snprintf(buf, bufsz, "."); return; }
    snprintf(buf, bufsz, "%s", dirname(path));
#else
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) { snprintf(buf, bufsz, "."); return; }
    path[n] = '\0';
    snprintf(buf, bufsz, "%s", dirname(path));
#endif
}

/* SECURITY: external tools (qbe, cc) are run with fork()+execvp() on an
   explicit argv array, never through system()/sh -c. Anything that reaches
   this compiler from outside -- `link "..."` / `shim "..."` directives in a
   (possibly third-party) .rapid file, `-l`/`-obj`/`-o` on the command line --
   is therefore a single, inert argv element and can never be interpreted as
   shell syntax (`;`, `&&`, backticks, $(...), ...). */
typedef struct {
    char **v;
    int n, cap;
} Argv;

static void argv_push(Argv *a, const char *s) {
    if (a->n + 2 > a->cap) { /* room for this entry + the NULL terminator */
        a->cap = a->cap ? a->cap * 2 : 16;
        a->v = realloc(a->v, (size_t)a->cap * sizeof(char *));
        if (!a->v) { perror("rapidc"); exit(1); }
    }
    a->v[a->n] = strdup(s);
    if (!a->v[a->n]) { perror("rapidc"); exit(1); }
    a->n++;
    a->v[a->n] = NULL;
}

/* A path argument must never be mistaken for an option by cc/qbe: prefix
   "./" to anything starting with '-'. */
static void argv_push_path(Argv *a, const char *path) {
    if (path[0] == '-') {
        size_t n = strlen(path) + 3;
        char *tmp = malloc(n);
        if (!tmp) { perror("rapidc"); exit(1); }
        snprintf(tmp, n, "./%s", path);
        argv_push(a, tmp);
        free(tmp);
    } else {
        argv_push(a, path);
    }
}

static void argv_free(Argv *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    a->v = NULL;
    a->n = a->cap = 0;
}

/* Library names for -l<name>: letters, digits and _ . + - : only (':' allows
   the `-l:libfoo.so.1` form). Rejects whitespace and every shell
   metacharacter outright with a clear message instead of handing garbage to
   the linker. */
static int valid_lib_name(const char *n) {
    if (!n || !*n) return 0;
    for (const char *p = n; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '+' || c == '-' || c == ':'))
            return 0;
    }
    return 1;
}

static void argv_push_lib(Argv *a, const char *name) {
    size_t n = strlen(name) + 3;
    char *tmp = malloc(n);
    if (!tmp) { perror("rapidc"); exit(1); }
    snprintf(tmp, n, "-l%s", name);
    argv_push(a, tmp);
    free(tmp);
}

/* Echo the command the way a shell would need it quoted (for the "+ ..."
   trace only -- nothing is ever re-parsed from this string). */
static void print_quoted(const char *s) {
    int safe = *s != '\0';
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || strchr("_-./=:,+@%", c))) {
            safe = 0;
            break;
        }
    }
    if (safe) { fputs(s, stderr); return; }
    fputc('\'', stderr);
    for (const char *p = s; *p; p++) {
        if (*p == '\'') fputs("'\\''", stderr);
        else fputc(*p, stderr);
    }
    fputc('\'', stderr);
}

/* Checks each directory in $PATH for an executable file named `name`. */
static int command_exists_on_path(const char *name) {
#ifdef _WIN32
    /* SearchPathA scans PATH + current dir for the file */
    char found[MAX_PATH];
    /* try with and without .exe */
    if (SearchPathA(NULL, name, ".exe", MAX_PATH, found, NULL) > 0) return 1;
    if (SearchPathA(NULL, name, NULL,   MAX_PATH, found, NULL) > 0) return 1;
    return 0;
#else
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) path_env = "/usr/bin:/bin";
    char *path_copy = strdup(path_env);
    int found = 0;
    char *saveptr = NULL;
    for (char *dir = strtok_r(path_copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
        char candidate[PATH_MAX];
        if (snprintf(candidate, sizeof(candidate), "%s/%s", dir, name) >= (int)sizeof(candidate))
            continue;
        if (access(candidate, X_OK) == 0) { found = 1; break; }
    }
    free(path_copy);
    return found;
#endif
}

#ifdef _WIN32
/* Build a single quoted command-line string from argv for CreateProcessA.
   Each arg is wrapped in double-quotes; internal double-quotes are escaped
   per MSVC/MinGW conventions (backslash before each "). */
static char *build_cmdline(Argv *a) {
    size_t total = 3; /* room for at least "" + NUL */
    for (int i = 0; i < a->n; i++) total += strlen(a->v[i]) * 2 + 4;
    char *cmd = malloc(total);
    if (!cmd) { perror("rapidc"); exit(1); }
    char *p = cmd;
    for (int i = 0; i < a->n; i++) {
        if (i) *p++ = ' ';
        *p++ = '"';
        for (const char *s = a->v[i]; *s; s++) {
            if (*s == '"') *p++ = '\\';
            *p++ = *s;
        }
        *p++ = '"';
    }
    *p = '\0';
    return cmd;
}
#endif

static int run_argv(Argv *a) {
    fputs("+", stderr);
    for (int i = 0; i < a->n; i++) {
        fputc(' ', stderr);
        print_quoted(a->v[i]);
    }
    fputc('\n', stderr);
    fflush(stderr);

#ifdef _WIN32
    char *cmdline = build_cmdline(a);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "rapidc: cannot run %s: error %lu\n", a->v[0], GetLastError());
        free(cmdline);
        return 1;
    }
    free(cmdline);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0 ? 0 : 1;
#else
    pid_t pid = fork();
    if (pid < 0) { perror("rapidc: fork"); return 1; }
    if (pid == 0) {
        execvp(a->v[0], a->v);
        fprintf(stderr, "rapidc: cannot run %s: %s\n", a->v[0], strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { perror("rapidc: waitpid"); return 1; }
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
#endif
}

/* Creates a unique temp file. Suffix matters: cc/.s tells the assembler. */
static int make_tmp(char *buf, size_t bufsz, const char *suffix) {
#ifdef _WIN32
    char tmp_dir[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, tmp_dir)) { strcpy(tmp_dir, "."); }
    char base[MAX_PATH];
    if (!GetTempFileNameA(tmp_dir, "rpc", 0, base)) {
        fprintf(stderr, "rapidc: GetTempFileName failed\n");
        return 0;
    }
    /* GetTempFileNameA already created the file; rename to add the suffix */
    snprintf(buf, bufsz, "%s%s", base, suffix);
    if (rename(base, buf) != 0) {
        /* fallback: just use the generated name without suffix */
        strncpy(buf, base, bufsz - 1); buf[bufsz-1] = '\0';
    }
    return 1;
#else
    snprintf(buf, bufsz, "/tmp/rapidc_XXXXXX%s", suffix);
    int fd = mkstemps(buf, (int)strlen(suffix));
    if (fd < 0) { perror("rapidc: mkstemp"); return 0; }
    close(fd);
    return 1;
#endif
}

int main(int argc, char **argv) {
    /* Opt-in, zero-cost unless RAPIDC_DIAG_DEBUG is set -- catches
       diag.h's DIAG_HELP_TABLE going stale against semantic.c's actual
       error-message wording (see diag.h for why this can happen
       silently). */
    semantic_diag_selfcheck(NULL);

    if (argc < 2) {
        fprintf(stderr, "usage: rapidc <input.rapid> [-o output] [-nostd] [-allow-shim]\n");
        return 1;
    }

    const char *input_path = NULL;
#ifdef _WIN32
    const char *output_path = "a.exe";
#else
    const char *output_path = "a.out";
#endif
    /* Demo/deneme kolaylığı: `-l<lib>` (link "..."; ile aynı iş, komut
       satırından) ve `-obj <file.o>` (rapidc'nin derleyemediği bir C shim
       .o'sunu son link adımına eklemek için) argümanları. Kod üretimini
       etkilemez, yalnızca son `cc` komutuna eklenir. Her biri ayrı bir argv
       elemanıdır (kabuk yok), bu yüzden metakarakterler etkisizdir. */
    Argv extra = {0};

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            const char *name = argv[++i];
            if (!valid_lib_name(name)) {
                fprintf(stderr, "rapidc: invalid library name for -l: '%s'\n", name);
                return 1;
            }
            argv_push_lib(&extra, name);
        } else if (strcmp(argv[i], "-nostd") == 0) {
            module_set_auto_std(0); /* M41: std/std.rapid otomatik dahil edilmesin */
        } else if (strcmp(argv[i], "-allow-shim") == 0) {
            module_set_allow_shim(1); /* guvenlik sertlestirmesi: bkz. module.h */
        } else if (strcmp(argv[i], "-obj") == 0 && i + 1 < argc) {
            argv_push_path(&extra, argv[++i]);
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
    {
        char home[PATH_MAX];
        get_exe_dir(home, sizeof(home));
        module_set_home(home); /* M41: `use "std/std.rapid";` yedek arama dizini */
    }
    char *merged_source = resolve_modules(input_path);
    if (!merged_source) {
        fprintf(stderr, "rapidc: module resolution failed\n");
        return 1;
    }
    if (getenv("RAPIDC_DUMP_MERGED")) {
        fprintf(stderr, "----- merged source -----\n%s\n----- end -----\n", merged_source);
    }

    /* fmemopen yoktur Windows'ta; tmpfile() her platformda var */
    yyin = tmpfile();
    if (!yyin) {
        fprintf(stderr, "rapidc: cannot create temp file for source\n");
        free(merged_source);
        return 1;
    }
    fputs(merged_source, yyin);
    rewind(yyin);

    /* M44 fix: resolve_modules() (above) already populated
       g_program.link_libs/link_objs from every `link "...";` and
       `shim "...";` directive found while merging sources -- but
       ast_reset() below unconditionally memset()s the whole of
       g_program, including those two fields, right before yyparse()
       builds the function/struct/enum lists into it. Every `link`/`shim`
       directive was silently discarded by the time semantic_check() or
       codegen ever saw g_program: `link "m";` never produced `-lm` on
       the final cc command, and no `shim "x.c";` file was ever actually
       compiled/linked in main.c's post-codegen step (see the loop at
       the `shim`-consuming site below) -- except this went unnoticed
       because the only shim in this repo (ffi/sqlite3_shim.c) is never
       exercised by run_tests.sh (sqlite3 is one of run_tests.sh's "harici
       bagimlilik" skips). We stash both lists here, before ast_reset(),
       and splice them back onto the freshly-reset g_program right after,
       since yyparse()/the grammar's link_decl/shim_decl rules do not
       themselves repopulate these fields (module.c's own pre-pass is the
       only thing that ever calls link_lib_add/link_obj_add). */
    LinkLib *saved_link_libs = g_program.link_libs;
    LinkObj *saved_link_objs = g_program.link_objs;

    /* yyparse() has no entry point of its own to reset itself from (unlike
       resolve_modules/semantic_check/codegen_emit, which call their own
       module_reset/semantic_reset/codegen_reset at the top), so the caller
       resets the AST state it's about to populate here instead. */
    ast_reset();
    g_program.link_libs = saved_link_libs;
    g_program.link_objs = saved_link_objs;
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

    /* M44: `extern fn` declarations that correspond to a function actually
       DEFINED in one of this program's own `shim "x.c";` files get a
       best-effort arity/class cross-check against that C definition (see
       ffi_check.c). This is a warning-turned-hard-error (matches how
       other M44 checks behave): a mismatch here is exactly the silent,
       runtime-only corruption class described in BUG_REPORT's "FFI tip
       guvenligi yok" item, so it is caught at compile time instead. */
    if (!ffi_check_shim_signatures(&g_program)) {
        fprintf(stderr, "rapidc: ffi signature check failed\n");
        return 1;
    }

    char il_path[64], asm_path[64];
    if (!make_tmp(il_path, sizeof(il_path), ".ssa")) return 1;
    if (!make_tmp(asm_path, sizeof(asm_path), ".s")) {
        unlink(il_path);
        return 1;
    }
    if (!codegen_emit(&g_program, il_path)) {
        fprintf(stderr, "rapidc: codegen failed\n");
        /* codegen_emit may have created/partially written il_path before
           failing; clean up rather than leaving temp files behind in /tmp. */
        unlink(il_path);
        unlink(asm_path);
        return 1;
    }

    /* M44: tell a missing `qbe` binary (very common in a fresh checkout --
       qbe is a separate project, not vendored) apart from qbe actually
       running and rejecting the generated IL. Before, both cases printed
       the same flat "qbe backend failed" and a CI/dev box without qbe on
       PATH got no hint that installing qbe (rather than debugging codegen)
       was the fix. execvp()'s own failure path in run_argv() always
       prints "cannot run qbe: ..." to stderr first when this is the
       no-such-file case, so we check PATH ourselves up front instead of
       trying to distinguish exit codes after the fact (a real qbe IL
       rejection also exits non-zero, with no reliable way to tell the
       two apart from the wait status alone). */
    if (!command_exists_on_path("qbe")) {
        fprintf(stderr,
            "rapidc: qbe backend failed: binary not found\n"
            "  'qbe' PATH uzerinde bulunamadi. Kurulum icin:\n"
            "  https://c9x.me/compile/\n"
            "  klonlayip derleyin ve olusan 'qbe' binary'sini PATH'e ekleyin.\n");
        unlink(il_path);
        unlink(asm_path);
        return 1;
    }

    Argv qbe = {0};
    argv_push(&qbe, "qbe");
#ifdef _WIN32
    argv_push(&qbe, "-t");
    argv_push(&qbe, "amd64_win");
#endif
    argv_push(&qbe, "-o");
    argv_push(&qbe, asm_path);
    argv_push(&qbe, il_path);
    int qbe_rc = run_argv(&qbe);
    argv_free(&qbe);
    if (qbe_rc != 0) {
        fprintf(stderr, "rapidc: qbe backend failed\n");
        /* Every exit path past this point must clean up both temp files
            (previously only the success path at the end did, so a failing
            qbe/link step left .ssa/.s files behind in /tmp on every
            failed build).             */
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
#ifdef _WIN32
    snprintf(runtime_path, sizeof(runtime_path), "%s\\runtime.o", exe_dir);
#else
    snprintf(runtime_path, sizeof(runtime_path), "%s/runtime.o", exe_dir);
#endif

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
    Argv shim_objs = {0};
    for (LinkObj *obj = g_program.link_objs; obj; obj = obj->next) {
        char obj_path[PATH_MAX + 4];
        snprintf(obj_path, sizeof(obj_path), "%s.o", obj->path);

        struct stat src_st, obj_st;
        int need_build = 1;
        if (stat(obj->path, &src_st) == 0 && stat(obj_path, &obj_st) == 0) {
            if (obj_st.st_mtime >= src_st.st_mtime) need_build = 0;
        }

        if (need_build) {
            Argv sc = {0};
            argv_push(&sc, "cc");
            argv_push(&sc, "-c");
            argv_push(&sc, "-o");
            argv_push(&sc, obj_path);
            argv_push_path(&sc, obj->path);
            int sc_rc = run_argv(&sc);
            argv_free(&sc);
            if (sc_rc != 0) {
                fprintf(stderr, "rapidc: failed to compile shim %s\n", obj->path);
                unlink(il_path);
                unlink(asm_path);
                return 1;
            }
        }

        argv_push_path(&shim_objs, obj_path);
    }

    /* M28: `link "name";` directives collected during parsing become
       trailing `-l<name>` flags on the link command, in source order.
       Appended (not prepended) so they follow the normal `cc` convention
       of listing libraries after the objects that need their symbols. */
    Argv link_flags = {0};
    for (LinkLib *lib = g_program.link_libs; lib; lib = lib->next) {
        if (!valid_lib_name(lib->name)) {
            fprintf(stderr, "rapidc: invalid library name in link directive: '%s'\n", lib->name);
            unlink(il_path);
            unlink(asm_path);
            return 1;
        }
        argv_push_lib(&link_flags, lib->name);
    }

    /* runtime.o unconditionally calls libm's pow/powf (for the `**`
       operator's float case), so every rapidc-compiled program needs -lm
       at final link time now, regardless of whether the source itself
       used `link "m";` — appended last (after any link_flags/-l<name>
       directives) same -l-after-consumers convention as those. */
    Argv cc = {0};
#ifdef _WIN32
    /* MinGW gcc: use AT&T asm output from qbe -t amd64_win.
       -lm is part of msvcrt on Windows; keep for math functions. */
    argv_push(&cc, "gcc");
#else
    argv_push(&cc, "cc");
    argv_push(&cc, "-no-pie");
#endif
    argv_push(&cc, asm_path);
    argv_push(&cc, runtime_path);
    for (int i = 0; i < shim_objs.n; i++) argv_push(&cc, shim_objs.v[i]);
    for (int i = 0; i < link_flags.n; i++) argv_push(&cc, link_flags.v[i]);
    for (int i = 0; i < extra.n; i++) argv_push(&cc, extra.v[i]);
#ifndef _WIN32
    /* Dead-code elimination: only supported on Linux/ELF linkers. */
    argv_push(&cc, "-Wl,--gc-sections");
#endif
    argv_push(&cc, "-lm");
    argv_push(&cc, "-o");
    argv_push_path(&cc, output_path);
    int cc_rc = run_argv(&cc);
    argv_free(&cc);
    argv_free(&shim_objs);
    argv_free(&link_flags);
    argv_free(&extra);
    if (cc_rc != 0) {
        fprintf(stderr, "rapidc: assemble/link failed\n");
        unlink(il_path);
        unlink(asm_path);
        return 1;
    }

    unlink(il_path);
    unlink(asm_path);

    return 0;
}
