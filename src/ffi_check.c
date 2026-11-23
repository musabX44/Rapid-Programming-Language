#include "ffi_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Coarse parameter class used for the cross-check. We deliberately do not
   try to distinguish int widths (int8 vs int64) or struct-vs-enum here --
   those never appear in a C shim's own prototype (a shim's whole purpose
   is to present a plain C ABI: int/long/double/pointer), so collapsing
   rapidc's many integer TypeKinds into one bucket loses nothing useful
   for this check and avoids false positives on width-only differences. */
typedef enum {
    FC_INT,      /* int/long/short/char and all rapidc int widths + bool + enum */
    FC_FLOAT,    /* float/double */
    FC_PTR,      /* any pointer, including `char *` i.e. rapidc `string` */
    FC_UNKNOWN   /* couldn't classify (e.g. a struct-by-value, va_list, ...) */
} FfiClass;

static FfiClass classify_rapidc_type(TypeKind t) {
    switch (t) {
        case TYPE_INT: case TYPE_BOOL: case TYPE_BYTE: case TYPE_ENUM:
        case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
        case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
            return FC_INT;
        case TYPE_FLOAT: case TYPE_DOUBLE:
            return FC_FLOAT;
        case TYPE_STRING: case TYPE_PTR:
            return FC_PTR;
        default:
            return FC_UNKNOWN;
    }
}

static const char *fc_name(FfiClass c) {
    switch (c) {
        case FC_INT: return "int-like";
        case FC_FLOAT: return "float/double";
        case FC_PTR: return "pointer/string";
        default: return "unknown";
    }
}

/* Classifies one C type as it appears written in a shim's own source
   (e.g. "const char *", "long", "double", "sqlite3 **"). Deliberately
   simple: strips qualifiers it knows about and then looks at what's
   left. Any trailing '*' makes it a pointer regardless of the base
   type, which is correct for C and is exactly the class rapidc's
   `string` and `T*` both map to. */
static FfiClass classify_c_type(const char *txt) {
    /* Count '*' anywhere in the fragment -- that alone decides pointer-ness. */
    for (const char *p = txt; *p; p++) {
        if (*p == '*') return FC_PTR;
    }
    /* No '*': strip common qualifiers/whitespace tokens and look at the
       base keyword. We only need to recognize the handful of spellings
       that show up in a shim's own int/float-returning or taking
       functions; anything else (a bare struct-by-value, "void", a
       typedef we don't know) is reported as unknown and skipped rather
       than guessed at, since a wrong guess here would be worse than no
       check at all. */
    char buf[128];
    size_t n = 0;
    int last_was_space = 1;
    for (const char *p = txt; *p && n + 1 < sizeof(buf); p++) {
        if (isspace((unsigned char)*p)) {
            if (!last_was_space && n > 0 && buf[n-1] != ' ') buf[n++] = ' ';
            last_was_space = 1;
        } else {
            buf[n++] = (char)tolower((unsigned char)*p);
            last_was_space = 0;
        }
    }
    while (n > 0 && buf[n-1] == ' ') n--;
    buf[n] = '\0';

    static const char *int_kw[] = {
        "int", "unsigned int", "unsigned", "signed int", "long", "unsigned long",
        "long long", "unsigned long long", "short", "unsigned short",
        "char", "unsigned char", "signed char", "size_t", "ssize_t",
        "int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t", "bool", "_bool", NULL
    };
    static const char *float_kw[] = { "float", "double", NULL };
    static const char *void_kw[] = { "void", NULL };

    for (int i = 0; int_kw[i]; i++) if (strcmp(buf, int_kw[i]) == 0) return FC_INT;
    for (int i = 0; float_kw[i]; i++) if (strcmp(buf, float_kw[i]) == 0) return FC_FLOAT;
    for (int i = 0; void_kw[i]; i++) if (strcmp(buf, void_kw[i]) == 0) return FC_UNKNOWN;
    return FC_UNKNOWN; /* struct-by-value, an unrecognized typedef, "..." etc. */
}

typedef struct {
    char *name;
    FfiClass param_classes[32];
    int param_count;
    int is_variadic; /* trailing "..." in the shim's own C prototype */
} ShimSig;

/* Splits a C parameter list ("const char *filename, sqlite3 **ppDb") on
   top-level commas (commas nested inside parens, e.g. a function-pointer
   parameter, are not split on) and classifies each piece. "void" alone
   (an explicit empty list) yields zero params. A bare "..." piece sets
   is_variadic and stops counting further (it must be last in valid C
   anyway). */
static void parse_c_param_list(const char *params, ShimSig *sig) {
    sig->param_count = 0;
    sig->is_variadic = 0;
    const char *p = params;
    while (isspace((unsigned char)*p)) p++;
    if (strcmp(p, "void") == 0 || *p == '\0') return;

    const char *seg_start = p;
    int depth = 0;
    while (1) {
        char c = *p;
        if (c == '(' ) depth++;
        else if (c == ')') depth--;
        if ((c == ',' && depth == 0) || c == '\0') {
            size_t len = (size_t)(p - seg_start);
            char seg[160];
            if (len >= sizeof(seg)) len = sizeof(seg) - 1;
            memcpy(seg, seg_start, len);
            seg[len] = '\0';
            /* trim */
            char *s = seg; while (isspace((unsigned char)*s)) s++;
            char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
            if (strcmp(s, "...") == 0) {
                sig->is_variadic = 1;
            } else if (*s && sig->param_count < 32) {
                /* Drop a trailing identifier (the parameter name) if present,
                   so "const char *filename" classifies on "const char *"
                   rather than tripping over the name. We find the last
                   identifier run; if what precedes it still contains a
                   recognizable type token or a '*', the name is dropped.
                   Simplification: if the fragment ends in an identifier
                   that is preceded by whitespace or '*', treat that last
                   identifier as the parameter name and strip it, UNLESS
                   the whole fragment IS just that identifier (an
                   unnamed/typedef-only parameter, e.g. bare "sqlite3*"
                   with no name -- rare in shim code but handled). */
                char *last_id_start = e;
                while (last_id_start > s &&
                       (isalnum((unsigned char)last_id_start[-1]) || last_id_start[-1] == '_'))
                    last_id_start--;
                if (last_id_start > s && last_id_start < e) {
                    char before = last_id_start[-1];
                    if (before == ' ' || before == '*') {
                        *last_id_start = '\0';
                        char *te = last_id_start; while (te > s && isspace((unsigned char)te[-1])) *--te = '\0';
                        if (*s) s = s; /* type fragment now in [s, te) */
                    }
                }
                sig->param_classes[sig->param_count++] = classify_c_type(s);
            }
            if (c == '\0') break;
            seg_start = p + 1;
        }
        p++;
    }
}

/* Very small textual scan for top-level C function DEFINITIONS (a
   prototype followed by `{`, not `;`) in a shim source file: looks for
   `<ret-type> <name> ( <params> ) {` patterns at brace-depth 0. This is
   intentionally not a C parser -- it skips comments and string literals
   so it doesn't misfire inside them, but it does not handle the
   preprocessor, multi-line macros that expand to function definitions,
   or K&R-style parameter lists. Good enough for the small, straight-line
   shim files this project writes by hand (see ffi/sqlite3_shim.c,
   std/net_shim/net_shim.c) -- the goal is to catch the common arity/
   class mistake, not to be a general C analyzer. */
static ShimSig *scan_shim_file(const char *path, int *out_count) {
    FILE *f = fopen(path, "rb");
    if (!f) { *out_count = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); *out_count = 0; return NULL; }
    char *src = malloc((size_t)sz + 1);
    size_t rd = fread(src, 1, (size_t)sz, f);
    src[rd] = '\0';
    fclose(f);

    /* Strip comments and string/char literals into spaces, preserving
       newlines so later diagnostics (not currently emitted with line
       numbers, but kept for future use) would still line up. */
    char *clean = malloc(rd + 1);
    size_t ci = 0;
    for (size_t i = 0; i < rd; ) {
        if (src[i] == '/' && i + 1 < rd && src[i+1] == '/') {
            while (i < rd && src[i] != '\n') { clean[ci++] = (src[i] == '\n' ? '\n' : ' '); i++; }
        } else if (src[i] == '/' && i + 1 < rd && src[i+1] == '*') {
            clean[ci++] = ' '; clean[ci++] = ' '; i += 2;
            while (i < rd && !(src[i] == '*' && i + 1 < rd && src[i+1] == '/')) {
                clean[ci++] = (src[i] == '\n' ? '\n' : ' '); i++;
            }
            if (i < rd) { clean[ci++] = ' '; clean[ci++] = ' '; i += 2; }
        } else if (src[i] == '"') {
            clean[ci++] = ' '; i++;
            while (i < rd && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < rd) i++;
                clean[ci++] = ' '; i++;
            }
            if (i < rd) { clean[ci++] = ' '; i++; }
        } else if (src[i] == '\'') {
            clean[ci++] = ' '; i++;
            while (i < rd && src[i] != '\'') {
                if (src[i] == '\\' && i + 1 < rd) i++;
                clean[ci++] = ' '; i++;
            }
            if (i < rd) { clean[ci++] = ' '; i++; }
        } else {
            clean[ci++] = src[i++];
        }
    }
    clean[ci] = '\0';
    free(src);

    int cap = 8, cnt = 0;
    ShimSig *sigs = malloc(sizeof(ShimSig) * (size_t)cap);

    const char *p = clean;
    int depth = 0;
    while (*p) {
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') { if (depth > 0) depth--; p++; continue; }
        if (depth == 0 && (isalpha((unsigned char)*p) || *p == '_')) {
            /* Try to match: <ident> ... <ident> '(' ... ')' <ws>* '{' */
            const char *line_start = p;
            const char *q = p;
            const char *name_start = NULL, *name_end = NULL;
            const char *paren_open = NULL;
            /* Scan forward collecting identifiers until we hit '(' or something
               that disqualifies this as a definition header (';', '{' before
               '(', or a newline-less run getting implausibly long). */
            int scanned = 0;
            while (*q && *q != ';' && scanned < 400) {
                if (*q == '(') { paren_open = q; break; }
                if (*q == '{') break; /* no '(' before '{': not a function header */
                if (isalpha((unsigned char)*q) || *q == '_') {
                    name_start = q;
                    while (isalnum((unsigned char)*q) || *q == '_') q++;
                    name_end = q;
                } else {
                    q++;
                }
                scanned++;
            }
            if (paren_open && name_start && name_end > name_start) {
                /* find matching ')' */
                const char *r = paren_open + 1;
                int pd = 1;
                while (*r && pd > 0) {
                    if (*r == '(') pd++;
                    else if (*r == ')') pd--;
                    if (pd > 0) r++;
                }
                if (*r == ')') {
                    const char *after = r + 1;
                    while (isspace((unsigned char)*after)) after++;
                    if (*after == '{') {
                        /* It's a definition. Extract name and params. */
                        size_t nlen = (size_t)(name_end - name_start);
                        if (nlen > 0 && nlen < 120) {
                            char namebuf[128];
                            memcpy(namebuf, name_start, nlen);
                            namebuf[nlen] = '\0';
                            if (strcmp(namebuf, "if") && strcmp(namebuf, "while") &&
                                strcmp(namebuf, "for") && strcmp(namebuf, "switch") &&
                                strcmp(namebuf, "return") && strcmp(namebuf, "sizeof")) {
                                size_t plen = (size_t)(r - (paren_open + 1));
                                char *pbuf = malloc(plen + 1);
                                memcpy(pbuf, paren_open + 1, plen);
                                pbuf[plen] = '\0';

                                if (cnt >= cap) { cap *= 2; sigs = realloc(sigs, sizeof(ShimSig) * (size_t)cap); }
                                sigs[cnt].name = strdup(namebuf);
                                parse_c_param_list(pbuf, &sigs[cnt]);
                                cnt++;
                                free(pbuf);
                            }
                        }
                        p = after + 1;
                        depth++; /* we're now inside the function body */
                        continue;
                    }
                }
            }
            p = line_start + 1;
            continue;
        }
        p++;
    }
    free(clean);
    *out_count = cnt;
    return sigs;
}

int ffi_check_shim_signatures(Program *prog) {
    if (!prog->link_objs) return 1; /* nothing to cross-check against */

    int ok = 1;
    for (LinkObj *obj = prog->link_objs; obj; obj = obj->next) {
        int shim_count = 0;
        ShimSig *shim_sigs = scan_shim_file(obj->path, &shim_count);
        if (!shim_sigs) continue;

        for (Function *f = prog->functions; f; f = f->next) {
            if (!f->is_extern) continue;
            for (int i = 0; i < shim_count; i++) {
                if (strcmp(f->name, shim_sigs[i].name) != 0) continue;

                ShimSig *ss = &shim_sigs[i];
                if (!ss->is_variadic && !f->is_variadic && ss->param_count != f->param_count) {
                    fprintf(stderr,
                        "rapidc: ffi uyarisi: extern fn %s(...) %d parametre bildiriyor, "
                        "ama shim \"%s\" icindeki C tanimi %d parametre aliyor\n",
                        f->name, f->param_count, obj->path, ss->param_count);
                    ok = 0;
                    continue;
                }
                int n = f->param_count < ss->param_count ? f->param_count : ss->param_count;
                Param *p = f->params;
                for (int k = 0; k < n && p; k++, p = p->next) {
                    FfiClass rc = classify_rapidc_type(p->type);
                    FfiClass cc = ss->param_classes[k];
                    if (cc == FC_UNKNOWN || rc == FC_UNKNOWN) continue; /* can't say, don't guess */
                    if (rc != cc) {
                        fprintf(stderr,
                            "rapidc: ffi uyarisi: extern fn %s parametre %d, rapidc tarafinda %s "
                            "olarak bildirilmis ama shim \"%s\" icindeki C tanimi %s bekliyor\n",
                            f->name, k + 1, fc_name(rc), obj->path, fc_name(cc));
                        ok = 0;
                    }
                }
            }
        }
        for (int i = 0; i < shim_count; i++) free(shim_sigs[i].name);
        free(shim_sigs);
    }
    return ok;
}
