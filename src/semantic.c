#include "semantic.h"
#include <string.h>
#include "var_struct_map.h"
#include "diag.h" /* Rust-tarzi ayrintili tani ciktisi; bkz. sem_error() */
#include "rapidc_limits.h"

/* main.c, diag.h'yi dogrudan dahil etmeden RAPIDC_DIAG_DEBUG self-check'ini
   tetikleyebilsin diye ince bir sarmalayici (diag.h'nin kendisi sadece bu
   dosyaya, parser.y'ye ve parser.tab.c'ye dahil edilir -- bkz. diag.h'deki
   diag_selfcheck_help_table() yorumu). */
void semantic_diag_selfcheck(const char *semantic_c_path) {
    diag_selfcheck_help_table(semantic_c_path);
}

/* Sorun-5 fix: limits.h'tan; codegen.c'deki MAX_LOCALS/MAX_FUNCS ile ayni
   kaynaga baglanir, artik elle senkron tutulmalari gerekmiyor. */
#define MAX_VARS RAPIDC_MAX_LOCALS_PER_FUNC
#define MAX_FUNCS RAPIDC_MAX_FUNCS

typedef struct {
    char *names[MAX_VARS];
    TypeKind types[MAX_VARS];
    TypeKind elem_types[MAX_VARS]; /* M26: pointee type when types[i] == TYPE_PTR */
    TypeKind elem_elem_types[MAX_VARS]; /* M32: pointee of the pointee, valid only when elem_types[i] == TYPE_PTR */
    int is_const[MAX_VARS];
    char *struct_names[MAX_VARS];
    FnType *fn_types[MAX_VARS]; /* M40: TYPE_FN degiskenlerin imzasi (yoksa NULL) */
    int count;
} SymTable;

/* M6: global function signature table, built once before checking bodies so
   forward references / recursion work regardless of definition order. */
typedef struct {
    char *name;
    TypeKind ret_type;
    TypeKind ret_elem_type;               /* M26 */
    TypeKind ret_elem_elem_type;          /* M32 */
    TypeKind param_types[MAX_VARS];
    TypeKind param_elem_types[MAX_VARS];  /* M26 */
    TypeKind param_elem_elem_types[MAX_VARS]; /* M32 */
    int param_count;
    int is_variadic; /* M27: extern fn declared with trailing `...` */
    const char *ret_struct_name;                 /* M40 */
    FnType *param_fn_types[MAX_VARS];            /* M40 */
    const char *param_struct_names[MAX_VARS];    /* M40 */
    const char *base;     /* M41: overload kumesinin ortak adi (mangle'siz). Overload'siz fonksiyonda == name. */
    int is_extern;        /* M41: extern fn overload edilemez */
    int can_fail;         /* M43: `: !T` ile bildirilmis */
} FuncSig;

static FuncSig g_funcs[MAX_FUNCS];
static int g_func_count = 0;

static int had_error = 0;
static int semantic_loop_depth = 0; /* tracks nesting inside while/for/for-in, for break/continue validation */
static TypeKind current_fn_ret_type = TYPE_VOID; /* for checking `return <expr>;` matches declared type */
static TypeKind current_fn_ret_elem_type = TYPE_VOID; /* M26: pointee type when current_fn_ret_type == TYPE_PTR */
static TypeKind current_fn_ret_elem_elem_type = TYPE_VOID; /* M32: pointee of the pointee, valid only when current_fn_ret_elem_type == TYPE_PTR */
static const char *current_fn_ret_struct_name = NULL; /* M40 */
static int current_fn_can_fail = 0;                    /* M43 */
static Param *current_fn_params = NULL; /* kept for potential future param-shape checks; the M33 len()-on-fixed-array-param check that used to consult this via is_fn_param() no longer applies now that fixed-size arrays are gone (M35) */

/* M35-arraylib: no longer called (the M33 check that used it was removed
   along with TYPE_ARRAY), kept only so current_fn_params isn't flagged
   unused; silenced rather than deleted in case a future param-shape check
   needs it again. */
static int is_fn_param(const char *name) {
    for (Param *p = current_fn_params; p; p = p->next) {
        if (strcmp(p->name, name) == 0) return 1;
    }
    return 0;
}
static int (*unused_is_fn_param_ref)(const char *) __attribute__((unused)) = is_fn_param;

/* M42: hatanin konumu. check_function / check_stmt / check_expr girerken
   ilgili dugumun line/file degerini buraya yazar, cikarken eskisini geri
   koyar; boylece sem_error en icteki bilinen dugumun konumunu basar. */
static int g_cur_line = 0;
static const char *g_cur_file = NULL;

static void sem_error(const char *msg, const char *name) {
    /* Rust-tarzi ayrintili tani: baslik "error: <msg>: <name>" (eski
       formatla ayni metin), sonra konum varsa kaynak satirinin
       cercevelenmis hali + '^' isaretcisi + (varsa) bir 'help' satiri.
       diag.h'nin DIAG_HELP_TABLE'i `msg` icinde bilinen bir anahtar
       gorunce otomatik bir ipucu ekler; eslesme yoksa help NULL kalir. */
    char headline[512];
    if (name && *name) snprintf(headline, sizeof(headline), "%s: %s", msg, name);
    else snprintf(headline, sizeof(headline), "%s", msg);
    diag_emit("semantic error", g_cur_file, g_cur_line, 0, headline, name, diag_find_help(msg));
    had_error = 1;
}

static int sym_find(SymTable *st, const char *name) {
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->names[i], name) == 0) return i;
    }
    return -1;
}

static void sym_add(SymTable *st, char *name, TypeKind type, int is_const, const char *struct_name, TypeKind elem_type, TypeKind elem_elem_type) {
    if (st->count >= MAX_VARS) {
        sem_error("too many variables (milestone limit)", name);
        return;
    }
    if (sym_find(st, name) != -1) {
        sem_error("redeclaration of variable", name);
        return;
    }
    st->names[st->count] = name;
    st->types[st->count] = type;
    st->elem_types[st->count] = elem_type; /* M26 */
    st->elem_elem_types[st->count] = elem_elem_type; /* M32 */
    st->is_const[st->count] = is_const;
    st->struct_names[st->count] = (char *)struct_name;
    st->fn_types[st->count] = NULL;
    st->count++;
    /* M8: remember var -> struct name globally so codegen can resolve field
       offsets when it later sees `var.field` expressions. */
    if (struct_name) {
        var_struct_map_set(name, struct_name);
    }
}


/* M37: `null` string olarak kullanilabilir. `null` her zaman TYPE_PTR olarak
   tiplenir (M33); bir HEDEF tip TYPE_STRING ise ve kaynak bir `null` LITERAL'i
   ise uyumlu sayilir. Codegen tarafinda hicbir degisiklik gerekmez: null zaten
   calisma zamaninda 0'dir ve string ham `char*`'tir. Runtime yardimcilari
   (rapidc_strcmp/strcat/strlen/...) NULL-guvenlidir (bkz. runtime.c). */
static int null_fits_string(TypeKind target, const Expr *src) {
    return target == TYPE_STRING && src && src->kind == EXPR_NULL_LITERAL;
}

static FuncSig *func_find(const char *name) {
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, name) == 0) return &g_funcs[i];
    }
    return NULL;
}


/* ---- M41: fonksiyon overload'u ------------------------------------------
   Ayni ada, PARAMETRE TIPLERI FARKLI birden fazla `fn`/`efn` tanimlanabilir
   (std/std.rapid'deki toInt/toString/len/... bu sayede tek isimle birden
   fazla tipe hizmet eder). Ilk tanim duz adini korur; sonrakiler pass 1'de
   `ad__ovN` olarak yeniden adlandirilir (AST'deki f->name de degisir, yani
   codegen ekstra bir sey bilmek zorunda degildir). Cagri yerinde argumanlarin
   tipine gore en uygun aday secilir ve e->name o adaya cevrilir.
   Kurallar:
     - extern fn overload edilemez (sembol adi sabittir), ayni imza = redeclaration.
     - Tam tip eslesmesi, tamsayi/float "uyumlu" eslesmesinden once gelir.
     - Esitlikte ilk tanimlanan kazanir. Hic aday uymazsa ilk aday secilir ve
       normal "argument type mismatch" hatasi uretilir. */
static int overload_count(const char *name) {
    int n = 0;
    for (int i = 0; i < g_func_count; i++)
        if (strcmp(g_funcs[i].base, name) == 0) n++;
    return n;
}

static int sig_params_equal(const FuncSig *a, const FuncSig *b) {
    if (a->param_count != b->param_count) return 0;
    for (int i = 0; i < a->param_count; i++) {
        if (a->param_types[i] != b->param_types[i]) return 0;
        if (a->param_types[i] == TYPE_PTR) {
            if (a->param_elem_types[i] != b->param_elem_types[i]) return 0;
            if (a->param_elem_types[i] == TYPE_PTR &&
                a->param_elem_elem_types[i] != b->param_elem_elem_types[i]) return 0;
        }
        if (a->param_types[i] == TYPE_STRUCT || a->param_types[i] == TYPE_ENUM) {
            const char *x = a->param_struct_names[i], *y = b->param_struct_names[i];
            if ((x && y && strcmp(x, y) != 0) || (!x) != (!y)) return 0;
        }
    }
    return 1;
}

/* Element type produced by indexing a value of `arr_type`. Fixed-size
   TYPE_ARRAY/TYPE_INT_ARRAY were removed in M35 (see std/array.rapid); the
   list kinds are the only builtin container kind left here besides
   pointers/strings (handled separately by their callers). */
static TypeKind element_type_of(TypeKind arr_type) {
    if (arr_type == TYPE_INT_LIST) return TYPE_INT;
    if (arr_type == TYPE_BYTE_LIST) return TYPE_BYTE;
    return TYPE_VOID;
}

/* M26: recovers the pointee (elem_type) of an already-checked TYPE_PTR
   expression. Needed because check_expr only returns a bare TypeKind
   (TYPE_PTR), losing what it points to — this walks the same small set of
   "produces a pointer" expression shapes semantic.c already understands
   and pulls elem_type from wherever it was stashed when that expression
   was itself checked: EXPR_ADDR_OF/EXPR_DEREF/EXPR_CALL("alloc") stash it
   directly on the node (e->elem_type); EXPR_IDENT looks it up in the
   symbol table (a var/param declared as `T*` carries elem_type there);
   EXPR_FIELD looks it up on the struct field definition. Falls back to
   TYPE_INT (the historical int* default) if the shape isn't recognized —
   should not happen for a well-typed TYPE_PTR expression given the shapes
   the grammar/semantic pass actually produce. */
/* BUG-02/BUG-03 fix: resolves the StructDef of *any* struct-typed expr, not
   just a bare `EXPR_IDENT`. Before this fix, every field-access/assignment
   check site special-cased `e->lhs->kind == EXPR_IDENT` and looked the
   struct name up via var_struct_map (which only maps variable names, not
   expressions) — so `o.inner.a` had no way to determine what struct
   `o.inner` is an instance of, even once the grammar could parse the
   chain. This walks the same two struct-producing shapes the language
   actually has: EXPR_IDENT (a declared struct variable, looked up in
   var_struct_map as before) and EXPR_FIELD (a struct-typed field of some
   other struct-typed base, resolved recursively via this same function on
   the base, then looked up by field name in that struct's own field list).
   Any other expr shape can't be struct-typed in this language, so returns
   NULL — callers already treat NULL as "not a struct" and emit the
   existing sem_error. */
static StructDef *struct_def_of_expr(Expr *e) {
    if (!e) return NULL;
    if (e->kind == EXPR_IDENT) {
        const char *sname = var_struct_map_get(e->name);
        return sname ? struct_find(sname) : NULL;
    }
    if (e->kind == EXPR_FIELD) {
        StructDef *base_sd = struct_def_of_expr(e->lhs);
        if (!base_sd) return NULL;
        for (Field *f = base_sd->fields; f; f = f->next) {
            if (strcmp(f->name, e->name) == 0) {
                return (f->type == TYPE_STRUCT && f->struct_name) ? struct_find(f->struct_name) : NULL;
            }
        }
    }
    return NULL;
}

/* M40: struct/enum ADI. check_expr yalnizca TYPE_STRUCT/TYPE_ENUM doner;
   hangi struct/enum oldugu yan kanalda (struct_names[], Field.struct_name,
   FuncSig.ret_struct_name) tutulur. Bu fonksiyon bir ifadenin adini bulur;
   bilinmiyorsa NULL doner (NULL = "kontrol etme", yanlis red vermemek icin). */
static const char *named_type_of(SymTable *st, Expr *e) {
    if (!e) return NULL;
    switch (e->kind) {
        case EXPR_IDENT: {
            int idx = sym_find(st, e->name);
            return idx != -1 ? st->struct_names[idx] : NULL;
        }
        case EXPR_FIELD: {
            StructDef *sd = struct_def_of_expr(e->lhs);
            for (Field *f = sd ? sd->fields : NULL; f; f = f->next)
                if (strcmp(f->name, e->name) == 0) return f->struct_name;
            return NULL;
        }
        case EXPR_STRUCT_LITERAL: return e->name;
        case EXPR_ENUM_LITERAL:   return e->enum_name;
        case EXPR_CALL: {
            FuncSig *fs = func_find(e->name);
            return fs ? fs->ret_struct_name : NULL;
        }
        case EXPR_CALL_INDIRECT:
            return e->fn_sig ? e->fn_sig->ret_name : NULL;
        default: return NULL;
    }
}

/* M40: t TYPE_STRUCT/TYPE_ENUM ise ve iki tarafin adi da biliniyorsa adlar
   ayni olmali. 1 = uyumsuz. */
static int named_mismatch(SymTable *st, TypeKind t, const char *expected, Expr *actual) {
    if (t != TYPE_STRUCT && t != TYPE_ENUM) return 0;
    const char *an = named_type_of(st, actual);
    return expected && an && strcmp(expected, an) != 0;
}

static TypeKind expr_elem_type_of(SymTable *st, Expr *e) {
    switch (e->kind) {
        case EXPR_ADDR_OF:
        case EXPR_DEREF:
            return e->elem_type;
        case EXPR_IDENT: {
            int idx = sym_find(st, e->name);
            if (idx != -1) return st->elem_types[idx];
            return TYPE_INT;
        }
        case EXPR_CALL:
            /* alloc(n) and any user function returning T* — elem_type was
               stashed on the call node when it was checked. */
            return e->elem_type;
        case EXPR_FIELD: {
            /* BUG-02/BUG-03 fix: resolve the base's struct via
               struct_def_of_expr (handles nested EXPR_FIELD bases, not just
               EXPR_IDENT). */
            StructDef *sd = struct_def_of_expr(e->lhs);
            for (Field *f = sd ? sd->fields : NULL; f; f = f->next) {
                if (strcmp(f->name, e->name) == 0) return f->elem_type;
            }
            return TYPE_INT;
        }
         case EXPR_INDEX:
             /* arrays/lists of pointers aren't a supported element type
                (element_type_of only returns int/byte) — not reachable for
                a well-typed TYPE_PTR expression. */
             return TYPE_INT;
         case EXPR_ARRAY_LITERAL:
             /* M35-arraylib: only reachable now as a list<int>/list<byte>
                initializer; element type stashed on the literal node by
                check_expr. */
             return e->elem_type;
         case EXPR_BINOP:
             /* T* +/- int and int + T* → T*, with the pointee info stashed on
                the binop node itself by the EXPR_BINOP handler above. M32:
                T** +/- int → T** likewise stashed on elem_elem_type. */
             return e->elem_type;
         default:
             return TYPE_INT;
    }
}

/* M32: recovers the pointee-of-the-pointee (elem_elem_type) of an already-
   checked TYPE_PTR expression whose elem_type is itself TYPE_PTR (i.e. this
   expr is T**). Mirrors expr_elem_type_of() one level down — same shapes,
   same stash-and-fallback approach. Only meaningful when the caller has
   already established expr_elem_type_of(st, e) == TYPE_PTR; callers that
   haven't should not trust the result. */
static TypeKind expr_elem_elem_type_of(SymTable *st, Expr *e) {
    switch (e->kind) {
        case EXPR_ADDR_OF:
        case EXPR_DEREF:
            return e->elem_elem_type;
        case EXPR_IDENT: {
            int idx = sym_find(st, e->name);
            if (idx != -1) return st->elem_elem_types[idx];
            return TYPE_INT;
        }
        case EXPR_CALL:
            return e->elem_elem_type;
        case EXPR_FIELD: {
            /* BUG-02/BUG-03 fix: see expr_elem_type_of above. */
            StructDef *sd = struct_def_of_expr(e->lhs);
            for (Field *f = sd ? sd->fields : NULL; f; f = f->next) {
                if (strcmp(f->name, e->name) == 0) return f->elem_elem_type;
            }
            return TYPE_INT;
        }
        default:
            return TYPE_INT;
    }
}


/* ---- M40: fn(...) imzalari ---- */
static int types_int_compatible(TypeKind a, TypeKind b);
static int types_float_compatible(TypeKind a, TypeKind b);

static void sym_set_fn_type(SymTable *st, const char *name, FnType *ft) {
    int i = sym_find(st, name);
    if (i != -1) st->fn_types[i] = ft;
}

static FnType *fn_type_from_funcsig(FuncSig *fs) {
    FnType *ft = calloc(1, sizeof(FnType));
    ft->param_count = fs->param_count;
    if (fs->param_count > 8 || fs->is_variadic) ft->unchecked = 1;
    for (int i = 0; i < fs->param_count && i < 8; i++) {
        ft->pt[i] = fs->param_types[i];
        ft->pe[i] = fs->param_elem_types[i];
        ft->pn[i] = fs->param_struct_names[i];
    }
    ft->ret = fs->ret_type;
    ft->ret_elem = fs->ret_elem_type;
    ft->ret_name = fs->ret_struct_name;
    return ft;
}

/* Bir ifadenin fn imzasi: fn-tipli degisken/parametre -> bildirilen imza,
   bare fonksiyon adi -> o fonksiyonun gercek imzasi. Bilinmiyorsa NULL
   (NULL = kontrol etme). */
static FnType *fn_type_of_expr(SymTable *st, Expr *e) {
    if (!e || (e->kind != EXPR_IDENT && e->kind != EXPR_FUNC_REF)) return NULL;
    int idx = sym_find(st, e->name);
    if (idx != -1) return st->types[idx] == TYPE_FN ? st->fn_types[idx] : NULL;
    FuncSig *fs = func_find(e->name);
    return fs ? fn_type_from_funcsig(fs) : NULL;
}

/* Fonksiyon isaretcisi esitligi (atama/arguman): ABI'yi bozmayan tek
   gevsek: tamsayi aileleri (hepsi `l`). float != double (farkli sinif). */
static int fn_slot_ok(TypeKind a, TypeKind ae, const char *an, TypeKind b, TypeKind be, const char *bn) {
    if (a == b) {
        if (a == TYPE_PTR && ae != be) return 0;
        if ((a == TYPE_STRUCT || a == TYPE_ENUM) && an && bn && strcmp(an, bn) != 0) return 0;
        return 1;
    }
    return types_int_compatible(a, b);
}

static int fn_type_mismatch(FnType *d, FnType *a) {
    if (!d || !a || d->unchecked || a->unchecked) return 0;
    if (d->param_count != a->param_count) return 1;
    for (int i = 0; i < d->param_count; i++)
        if (!fn_slot_ok(d->pt[i], d->pe[i], d->pn[i], a->pt[i], a->pe[i], a->pn[i])) return 1;
    return !fn_slot_ok(d->ret, d->ret_elem, d->ret_name, a->ret, a->ret_elem, a->ret_name);
}

static TypeKind check_expr(SymTable *st, Expr *e);
static TypeKind check_indirect_call(SymTable *st, Expr *e) {
    FnType *ft = fn_type_of_expr(st, e->lhs);
    int argc = 0;
    for (ExprList *l = e->args; l; l = l->next, argc++) {
        TypeKind at = check_expr(st, l->expr);
        if (!ft || ft->unchecked || argc >= ft->param_count) continue;
        TypeKind pt = ft->pt[argc];
        int ok;
        if (at == pt) {
            ok = 1;
            if (at == TYPE_PTR && l->expr->kind != EXPR_NULL_LITERAL &&
                expr_elem_type_of(st, l->expr) != ft->pe[argc]) ok = 0;
            if ((at == TYPE_STRUCT || at == TYPE_ENUM) && named_mismatch(st, at, ft->pn[argc], l->expr)) ok = 0;
        } else {
            ok = types_int_compatible(pt, at) || types_float_compatible(pt, at) ||
                 null_fits_string(pt, l->expr);
        }
        if (!ok) sem_error("argument type mismatch in call through function value", e->lhs->name);
    }
    if (ft && !ft->unchecked && argc != ft->param_count) {
        sem_error("wrong number of arguments in call through function value", e->lhs->name);
    }
    if (ft) {
        e->fn_sig = ft;
        e->elem_type = ft->ret_elem;
        return ft->ret;
    }
    return TYPE_INT; /* imza bilinmiyor (or. struct alani): eski davranis */
}

/* M9: names reserved for the dynamic-list builtins. These are checked and
   codegen'd specially (like io::out) rather than being real user functions,
   so they aren't in the function-signature table and can't be redefined. */
static int is_list_builtin(const char *name) {
    /* M41: listCopy/len artik std/std.rapid'de (overload). Derleyicide yalnizca
       push kaldi: ilk argumanini (liste degiskeni) yeni isaretciyle guncellemesi
       gerektigi icin bir fonksiyon olarak yazilamaz. */
    return strcmp(name, "push") == 0;
}

/* M41: derleyiciden std/std.rapid'e tasinan eski builtin'ler. Artik rezerve
   degiller (fonksiyon olarak yazilabilirler); yalnizca "tanimsiz fonksiyon"
   hatasina ipucu eklemek ve builtin-golgeleme notunu uretmek icin tutulur. */
static int is_moved_to_std(const char *name) {
    static const char *names[] = { "toInt", "toString", "toFloat", "toDouble",
                                    "len", "listCopy", "syscall", NULL };
    for (int i = 0; names[i]; i++) if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}

/* M12: names reserved for the sysprog builtins (raw system-programming
   primitives) - same treatment as is_list_builtin above. */
static int is_sysprog_builtin(const char *name) {
    /* M41: syscall std/std.rapid'e tasindi (libc syscall(2), variadic extern).
       alloc/free kaldi: alloc'un donus pointee tipi baglamdan cikarilir,
       free hem pointer hem string alir; ikisi de bir fonksiyon imzasi ile
       ifade edilemez. */
    return strcmp(name, "alloc") == 0 || strcmp(name, "free") == 0;
}

/* M36: `alias__len` gibi bir isim, kullanicinin ayni dosyada `len` adinda kendi
   fonksiyonunu tanimlamasindan dogar: module.c dosyadaki HER `len(...)`
   cagrisini `alias__len(...)`'e cevirir, builtin `len(s)` cagrilarini da.
   Metin duzeyinde builtin/kendi-fonksiyon ayrimi yapilamaz (tip bilgisi yok),
   bu yuzden dogru cozum yazarin o cagriyi baska bir yolla yazmasidir. Bu
   yardimci, hata mesajinda nedeni gostermek icin ismi ayristirir. */
static int mangled_builtin_shadow(const char *name, const char **builtin_out) {
    const char *sep = strstr(name, "__");
    if (!sep || sep == name) return 0;
    const char *base = sep + 2;
    if (is_list_builtin(base) || is_sysprog_builtin(base) || is_moved_to_std(base)) {
        if (builtin_out) *builtin_out = base;
        return 1;
    }
    return 0;
}


/* M23: replaces the old hand-written pairwise `(a==TYPE_BYTE && b==TYPE_INT)
   || (a==TYPE_INT && b==TYPE_BYTE)` checks scattered across this file (call
   args, var-decl init, return, assignment, struct literal fields). Any two
   integer types (int/byte/int8..64/uint8..64) are now mutually compatible —
   narrowing/widening is implicit, matching this language's existing
   int<->byte behavior; a future milestone can add opt-in strictness
   (warnings on narrowing) without touching call sites that use this. */
static int types_int_compatible(TypeKind a, TypeKind b) {
    return type_is_integer(a) && type_is_integer(b);
}

/* M25: float and double are mutually compatible (implicit widen/narrow),
   mirroring the integer family's types_int_compatible above. Integer and
   float types are deliberately NOT compatible here — this language already
   treats int/byte/int8..64 as interchangeable, but mixing an integer and a
   floating type implicitly would hide a real representation change (and a
   real QBE register-class change); that conversion has to be explicit
   (toFloat()/toDouble()/toInt()-style builtins are left to a follow-up
   milestone) rather than silently inserted by binop/assignment/call-arg
   checks. */
static int types_float_compatible(TypeKind a, TypeKind b) {
    return type_is_float(a) && type_is_float(b);
}

/* M25: result type of combining two float-family operands of possibly
   different widths — mirrors type_int_promote (the wider one wins). Callers
   must check types_float_compatible() on both operands first. */
static TypeKind type_float_promote(TypeKind a, TypeKind b) {
    if (a == b) return a;
    return (a == TYPE_DOUBLE || b == TYPE_DOUBLE) ? TYPE_DOUBLE : TYPE_FLOAT;
}

static void check_stmt(SymTable *st, Stmt *s);
static void check_stmt_inner(SymTable *st, Stmt *s);
static TypeKind check_expr_inner(SymTable *st, Expr *e);

/* SECURITY/DoS: check_expr_inner recurses once per AST nesting level with
   no bound. A few thousand levels of parenthesized nesting
   ((((1+1)+1)+1)...) exhausts the C call stack and crashes rapidc with
   SIGSEGV instead of a clean diagnostic -- reachable from any (possibly
   third-party/untrusted) .rapid source file. This thin wrapper (every
   recursive call inside check_expr_inner still goes through the public
   check_expr name, so no call site needs to change) adds an explicit depth
   counter and reports it the same way every other semantic error is
   reported, via sem_error()/had_error, instead of recursing further. */
#define MAX_EXPR_DEPTH RAPIDC_MAX_EXPR_DEPTH
static int g_sem_expr_depth = 0;

static TypeKind check_expr(SymTable *st, Expr *e) {
    if (++g_sem_expr_depth > MAX_EXPR_DEPTH) {
        static int reported = 0;
        if (!reported) { sem_error("expression nested too deeply", NULL); reported = 1; }
        g_sem_expr_depth--;
        return TYPE_INT;
    }
    int sv_line = g_cur_line; const char *sv_file = g_cur_file;
    if (e && e->line > 0) { g_cur_line = e->line; g_cur_file = e->file; }
    TypeKind r = check_expr_inner(st, e);
    /* M43: hata donebilen bir fonksiyon cagrisi try/catch olmadan kullanilamaz
       (aksi halde hata sessizce yutulurdu). e->handled'i EXPR_TRY/EXPR_CATCH koyar. */
    if (e && e->kind == EXPR_CALL && !e->handled) {
        FuncSig *fs = func_find(e->name);
        if (fs && fs->can_fail)
            sem_error("call to fallible function must be wrapped in try or catch", e->name);
    }
    g_cur_line = sv_line; g_cur_file = sv_file;
    g_sem_expr_depth--;
    return r;
}


/* M41: overload cozumleyici (bkz. "fonksiyon overload'u" notu yukarida). */
static void resolve_overload(SymTable *st, Expr *e) {
    int argc = 0;
    TypeKind at[16];
    Expr *ax[16];
    for (ExprList *l = e->args; l && argc < 16; l = l->next, argc++) {
        at[argc] = check_expr(st, l->expr);
        ax[argc] = l->expr;
    }
    int best = -1, best_score = -1;
    for (int i = 0; i < g_func_count; i++) {
        FuncSig *fs = &g_funcs[i];
        if (strcmp(fs->base, e->name) != 0) continue;
        if (fs->param_count != argc && !(fs->is_variadic && argc >= fs->param_count)) continue;
        int score = 0, ok = 1;
        for (int j = 0; j < fs->param_count && ok; j++) {
            TypeKind pt = fs->param_types[j];
            if (at[j] == pt) {
                score += 2;
                if (pt == TYPE_PTR && ax[j]->kind != EXPR_NULL_LITERAL) {
                    if (expr_elem_type_of(st, ax[j]) != fs->param_elem_types[j]) ok = 0;
                } else if ((pt == TYPE_STRUCT || pt == TYPE_ENUM) &&
                           named_mismatch(st, pt, fs->param_struct_names[j], ax[j])) {
                    ok = 0;
                }
            } else if (types_int_compatible(pt, at[j]) || types_float_compatible(pt, at[j]) ||
                       null_fits_string(pt, ax[j])) {
                score += 1;
            } else {
                ok = 0;
            }
        }
        if (ok && score > best_score) { best = i; best_score = score; }
    }
    if (best >= 0) e->name = g_funcs[best].name;
}

static TypeKind check_expr_inner(SymTable *st, Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LITERAL:
            return TYPE_INT;
        case EXPR_FLOAT_LITERAL:
            /* M25: a bare `3.14` literal defaults to double (the language's
               general-purpose floating type), same spirit as EXPR_INT_LITERAL
               defaulting to TYPE_INT — a declaration like `var f: float =
               3.14;` still works because of types_float_compatible below. */
            return TYPE_DOUBLE;
        case EXPR_CHAR_LITERAL:
            return TYPE_BYTE;
        case EXPR_NULL_LITERAL:
            /* M33: `null` is always TYPE_PTR at the flat-enum level. Unlike
               every other pointer expression, it has no fixed elem_type of
               its own — expr_elem_type_of/expr_elem_elem_type_of below
               special-case it to instead report whatever elem_type it's
               being compared/assigned against, so `p == null` and
               `var p: Inner* = null;` both type-check regardless of what
               p points to. */
            return TYPE_PTR;
        case EXPR_BOOL_LITERAL:
            return TYPE_BOOL;
        case EXPR_STRING_LITERAL:
            return TYPE_STRING;
        case EXPR_IDENT: {
            int idx = sym_find(st, e->name);
            if (idx == -1) {
                /* M11: not a variable — if it names a known top-level
                   function, it's a first-class function value instead
                   (no closures: just the bare function reference). */
                if (fnsig_find(e->name)) {
                    e->kind = EXPR_FUNC_REF;
                    { /* M43: hata donebilen fonksiyon deger olarak alinamaz (try/catch atlanirdi) */
                        FuncSig *rfs0 = func_find(e->name);
                        if (rfs0 && rfs0->can_fail)
                            sem_error("cannot take a reference to a fallible function (not supported)", e->name);
                    }
                    return TYPE_FN;
                }
                sem_error("undefined identifier", e->name);
                return TYPE_INT; /* keep going */
            }
            return st->types[idx];
        }
        case EXPR_IO_OUT_CALL: {
            check_expr(st, e->arg);
            return TYPE_VOID;
        }
        case EXPR_IO_IN_CALL: {
            /* M17: io::in() takes no arguments and always yields a string */
            return TYPE_STRING;
        }
        case EXPR_IO_FORMAT_CALL: {
            /* M17: io::out(fmt, args...) with 2+ arguments - fmt must be a
               string; each further arg can be int/byte/string (the only
               printable types plain io::out already supports). */
            TypeKind fmt_type = check_expr(st, e->lhs);
            if (fmt_type != TYPE_STRING) {
                sem_error("io::out format string must be a string", NULL);
            }
            for (ExprList *l = e->args; l; l = l->next) {
                TypeKind at = check_expr(st, l->expr);
                if (at != TYPE_INT && at != TYPE_BYTE && at != TYPE_STRING) {
                    sem_error("io::out arguments must be int, byte, or string", NULL);
                }
            }
            return TYPE_VOID;
        }
        case EXPR_IO_SYSCALL_CALL: {
            /* M18: io::open/read/write/close - raw file syscall wrappers.
               Argument shapes:
                 io::open(path: string, flags: int, mode: int) -> int (fd)
                 io::read(fd: int, buf: byte*, count: int)     -> int (nread)
                 io::write(fd: int, buf: byte*, count: int)    -> int (nwritten)
                 io::close(fd: int)                            -> int (0 or -1)
               Checked here rather than via the function-signature table for
               the same reason as sysprog:: builtins: no single fixed
               signature shape is shared with ordinary user functions. */
            int argc = 0;
            TypeKind arg_types[8];
            for (ExprList *l = e->args; l && argc < 8; l = l->next, argc++) {
                arg_types[argc] = check_expr(st, l->expr);
            }
            for (ExprList *l = e->args; l && argc >= 8; l = l->next) {
                /* still typecheck any extras beyond the cap, just don't record them */
                check_expr(st, l->expr);
            }

            if (strcmp(e->name, "open") == 0) {
                if (argc != 3) {
                    sem_error("io::open takes exactly 3 arguments (path, flags, mode)", NULL);
                    return TYPE_INT;
                }
                if (arg_types[0] != TYPE_STRING) {
                    sem_error("io::open's path argument must be a string", NULL);
                }
                if (arg_types[1] != TYPE_INT || arg_types[2] != TYPE_INT) {
                    sem_error("io::open's flags and mode arguments must be int", NULL);
                }
                return TYPE_INT;
            }
            if (strcmp(e->name, "read") == 0 || strcmp(e->name, "write") == 0) {
                if (argc != 3) {
                    sem_error("io::read/io::write take exactly 3 arguments (fd, buf, count)", NULL);
                    return TYPE_INT;
                }
                if (arg_types[0] != TYPE_INT) {
                    sem_error("io::read/io::write's fd argument must be int", NULL);
                }
                if (arg_types[1] != TYPE_PTR && arg_types[1] != TYPE_STRING) {
                    sem_error("io::read/io::write's buf argument must be a pointer or a string", NULL);
                }
                if (arg_types[2] != TYPE_INT) {
                    sem_error("io::read/io::write's count argument must be int", NULL);
                }
                return TYPE_INT;
            }
            /* close */
            if (strcmp(e->name, "close") == 0) {
                if (argc != 1) {
                    sem_error("io::close takes exactly 1 argument (fd)", NULL);
                    return TYPE_INT;
                }
                if (arg_types[0] != TYPE_INT) {
                    sem_error("io::close's fd argument must be int", NULL);
                }
                return TYPE_INT;
            }
            /* errno */
            if (argc != 0) {
                sem_error("io::errno takes no arguments", NULL);
            }
            return TYPE_INT;
        }
        case EXPR_FUNC_REF: {
            FuncSig *rfs = func_find(e->name);
            if (rfs && rfs->can_fail)
                sem_error("cannot take a reference to a fallible function (not supported)", e->name);
            return TYPE_FN;
        }
        case EXPR_ERROR_LIT:
            return TYPE_INT; /* M43: hata kodu */
        case EXPR_TRY: {
            /* M43: try <cagri> -- hata varsa bu fonksiyondan da ayni hatayla cik. */
            if (!current_fn_can_fail)
                sem_error("try used in a function that cannot fail (declare it with `: !T`)", NULL);
            if (!e->arg || e->arg->kind != EXPR_CALL) {
                sem_error("try requires a call to a fallible function", NULL);
                return check_expr(st, e->arg);
            }
            e->arg->handled = 1;
            TypeKind tt = check_expr(st, e->arg);
            FuncSig *tfs = func_find(e->arg->name);
            if (!tfs || !tfs->can_fail)
                sem_error("try applied to a call that cannot fail", e->arg->name);
            return tt;
        }
        case EXPR_CATCH: {
            /* M43: <cagri> catch <varsayilan> -- hata varsa varsayilan deger. */
            if (!e->lhs || e->lhs->kind != EXPR_CALL) {
                sem_error("catch requires a call to a fallible function on its left", NULL);
                check_expr(st, e->rhs);
                return e->lhs ? check_expr(st, e->lhs) : TYPE_INT;
            }
            e->lhs->handled = 1;
            TypeKind lt2 = check_expr(st, e->lhs);
            TypeKind rt2 = check_expr(st, e->rhs);
            FuncSig *cfs = func_find(e->lhs->name);
            if (!cfs || !cfs->can_fail)
                sem_error("catch applied to a call that cannot fail", e->lhs->name);
            if (lt2 != TYPE_VOID && rt2 != lt2 &&
                !types_int_compatible(lt2, rt2) && !types_float_compatible(lt2, rt2) &&
                !null_fits_string(lt2, e->rhs))
                sem_error("catch default value type does not match the call's result type", NULL);
            return lt2; /* void ise sonuc de void: varsayilan yalniz yan etki icin */
        }
        case EXPR_CALL_INDIRECT: {
            check_expr(st, e->lhs);
            return check_indirect_call(st, e);
        }
        case EXPR_ARRAY_LITERAL: {
            /* M35-arraylib: `[e0, e1, ...]` no longer denotes a standalone
               fixed-size-array value. It's only valid as the initializer
               syntax for `list<int> xs = [1,2,3];` / `list<byte> bs = [];`
               (checked directly in STMT_VAR_DECL below, which calls
               check_expr on each element itself and never looks at this
               return value's TypeKind as anything but "was this an array
               literal"). If a bare `[...]` shows up anywhere else (passed
               as a call argument, used in an expression, etc.) that's now
               a type error: there's no more TYPE_ARRAY value type for it to
               produce. Elements are still checked for validity so errors
               inside the literal are still reported. */
            TypeKind elem = TYPE_VOID;
            int is_float = 0;
            for (ExprList *l = e->elements; l; l = l->next) {
                TypeKind et = check_expr(st, l->expr);
                if (type_is_float(et)) {
                    if (elem != TYPE_VOID && !type_is_float(elem)) {
                        sem_error("array literal cannot mix integer and float elements", NULL);
                    }
                    is_float = 1;
                    if (elem == TYPE_VOID) elem = et;
                    else if (elem != et) elem = (et == TYPE_DOUBLE || elem == TYPE_DOUBLE) ? TYPE_DOUBLE : TYPE_FLOAT;
                } else if (type_is_integer(et)) {
                    if (is_float) {
                        sem_error("array literal cannot mix integer and float elements", NULL);
                    }
                    if (elem == TYPE_VOID) elem = et;
                    else if (elem != et) elem = type_int_promote(elem, et);
                } else {
                    sem_error("array literal elements must be integers or floats", NULL);
                }
            }
            if (elem == TYPE_VOID) elem = TYPE_INT; /* empty `[]` */
            e->elem_type = elem;
            /* Sentinel-only: STMT_VAR_DECL below detects "was this literal"
               via e->kind == EXPR_ARRAY_LITERAL, not via this return type,
               so returning TYPE_INT_LIST here (rather than a removed
               TYPE_ARRAY) is safe and lets a stray bare `[1,2,3];`
               statement/argument still type-check as *some* concrete type
               instead of crashing, while being clearly wrong wherever an
               int/string/etc. was actually expected. */
            return TYPE_INT_LIST;
        }
        case EXPR_INDEX: {
            TypeKind arr_type = check_expr(st, e->lhs);
            TypeKind idx_type = check_expr(st, e->rhs);
            if (arr_type != TYPE_INT_LIST && arr_type != TYPE_BYTE_LIST && arr_type != TYPE_STRING) {
                /* M26-add: T*[i] — indexing a pointer itself, like C's p[i]. */
                if (arr_type == TYPE_PTR) {
                    if (idx_type != TYPE_INT) sem_error("index must be int", NULL);
                    return expr_elem_type_of(st, e->lhs);
                }
                /* M35-arraylib: fixed-size T[] indexing removed — use
                   array::get(a, i) / array::set(a, i, v) from
                   std/array.rapid instead. */
                sem_error("indexing requires a list, string, or pointer (fixed-size arrays were removed; use array::get/array::set from std/array.rapid)", NULL);
            }
            if (idx_type != TYPE_INT) {
                sem_error("index must be int", NULL);
            }
            if (arr_type == TYPE_STRING) return TYPE_BYTE;
            if (arr_type == TYPE_PTR) return expr_elem_type_of(st, e->lhs);
            return element_type_of(arr_type);
        }
        case EXPR_ADDR_OF: {
            TypeKind t = check_expr(st, e->arg);
            /* M26: any type_ptr_ok() operand may now have its address
               taken (previously only int/byte). elem_type is stashed on
               `e` itself (the EXPR_ADDR_OF node) so codegen and any
               enclosing var-decl/param/return check can recover exactly
               what this pointer points to — check_expr only returns a
               bare TypeKind, so this side field is how the extra info
               escapes this call. */
            if (!type_ptr_ok(t)) {
                sem_error("address-of operator requires an addressable lvalue of a pointer-capable type", NULL);
            }
            e->elem_type = t;
            /* M32: if the operand is itself a pointer (t == TYPE_PTR, e.g.
               `&p` where p: T*), this EXPR_ADDR_OF node is T** — stash what
               *that* inner pointer points to (p's own elem_type) so a
               later *pp double-deref, or a `var pp: T** = &p;` decl check,
               can recover the full two-level shape via
               expr_elem_elem_type_of(). Meaningless (left at whatever it
               already was, typically TYPE_VOID/TYPE_INT) when t != TYPE_PTR. */
            if (t == TYPE_PTR) {
                e->elem_elem_type = expr_elem_type_of(st, e->arg);
            }
            return TYPE_PTR;
        }
        case EXPR_DEREF: {
            TypeKind t = check_expr(st, e->arg);
            if (t != TYPE_PTR) {
                sem_error("dereference requires a pointer", NULL);
                e->elem_type = TYPE_INT;
                return TYPE_INT;
            }
            /* M26: the operand's elem_type was set when *it* was checked
               (either directly on an EXPR_ADDR_OF node, or propagated from
               a variable's declared elem_type via EXPR_IDENT below). */
            TypeKind arg_elem = expr_elem_type_of(st, e->arg);
            /* M32: arg_elem is *this expr's own static type* (e.g. for
               `*pp` where pp: T**, arg_elem == TYPE_PTR since *pp is T*).
               e->elem_type must hold *this expr's* pointee (what *this*
               expr points to), which is one level further down: if
               arg_elem is itself TYPE_PTR, that pointee is e->arg's
               elem_elem_type (pp's elem_elem_type, i.e. T); otherwise
               (plain T*) it's e->arg's own elem_type as before. */
            if (arg_elem == TYPE_PTR) {
                e->elem_type = expr_elem_elem_type_of(st, e->arg);
                e->elem_elem_type = TYPE_VOID; /* no T*** support */
            } else {
                e->elem_type = arg_elem;
            }
            return arg_elem;
        }
        case EXPR_NEG: {
            /* M29: unary minus. Result type is exactly the operand's type
               (int stays int, float/double stays float/double) — unlike
               the `0 - expr` desugar this replaces, there is no zero
               literal here to be incompatible with a float operand. */
            TypeKind t = check_expr(st, e->arg);
            if (!type_is_integer(t) && !type_is_float(t)) {
                sem_error("unary '-' requires an integer or float operand", NULL);
            }
            return t;
        }
        case EXPR_NOT: {
            /* Logical negation: bool -> bool only (not the C-style
               "any nonzero value" negation — this language already has a
               dedicated TYPE_BOOL, so !expr is restricted to it rather
               than silently accepting ints/pointers). */
            TypeKind t = check_expr(st, e->arg);
            if (t != TYPE_BOOL) {
                sem_error("'!' requires a bool operand", NULL);
            }
            return TYPE_BOOL;
        }
        case EXPR_LOGAND:
        case EXPR_LOGOR: {
            /* && / || are boolean-only and short-circuit (codegen branches
               around evaluating the rhs), so both operands must already be
               bool — no int/pointer truthiness coercion. */
            TypeKind lt = check_expr(st, e->lhs);
            TypeKind rt = check_expr(st, e->rhs);
            if (lt != TYPE_BOOL || rt != TYPE_BOOL) {
                sem_error(e->kind == EXPR_LOGAND ? "'&&' requires bool operands" : "'||' requires bool operands", NULL);
            }
            return TYPE_BOOL;
        }
        case EXPR_CALL: {
            /* M11: `f(args)` where `f` is a variable/param holding a
               function value (TYPE_FN), not a top-level function name —
               indirect call. Mark the node so codegen knows to call through
               a register instead of `$f_<name>`. Argument count/types
               aren't verified against the held function's signature in
               this milestone (TYPE_FN is a flat tag with no retained
               per-value signature). */
            {
                int idx = sym_find(st, e->name);
                if (idx != -1 && st->types[idx] == TYPE_FN) {
                    e->kind = EXPR_CALL_INDIRECT;
                    e->lhs = expr_new_ident(e->name);
                    return check_indirect_call(st, e); /* M40: imza biliniyorsa dogrular */
                }
            }
            /* M12: sysprog builtins (alloc/free/syscall) - raw
               system-programming primitives, checked here for the same
               reason as the list builtins above (no single fixed
               signature shape shared with user functions). */
            if (is_sysprog_builtin(e->name)) {
                int argc = 0;
                for (ExprList *l = e->args; l; l = l->next, argc++) {
                    TypeKind t = check_expr(st, l->expr);
                    if (strcmp(e->name, "free") == 0) {
                        /* M36: `free(string)` artik kabul edilir. string'ler
                           heap'te ham `char*` (rapidc_strcat malloc'lar) ve
                           codegen zaten yalnizca `rapidc_free(l <deger>)` uretir,
                           yani kod tarafinda hicbir engel yoktu. TEK istisna:
                           string LITERAL'i statik veri segmentindedir, free
                           edilirse cokerdi -> derleme zamaninda reddedilir. */
                        if (argc == 0 && t == TYPE_STRING) {
                            if (l->expr->kind == EXPR_STRING_LITERAL) {
                                sem_error("free() of a string literal (static data, not heap-allocated)", NULL);
                            }
                        } else if (argc == 0 && t != TYPE_PTR) {
                            sem_error("free's argument must be a pointer or string", NULL);
                        }
                    } else {
                        /* alloc's size and syscall's nr/args are all plain ints */
                        if (t != TYPE_INT) {
                            sem_error("sysprog builtin arguments must be int (or a pointer for free)", NULL);
                        }
                    }
                }
                if (strcmp(e->name, "alloc") == 0) {
                    if (argc != 1) sem_error("alloc takes exactly 1 argument (size)", NULL);
                    /* M26: elem_type is not known here — alloc() is
                       context-typed, inferred by whichever enclosing
                       var-decl/assign/param/return checked this call and
                       set e->elem_type accordingly (see STMT_VAR_DECL /
                       STMT_ASSIGN). Defaults to TYPE_INT if alloc() is used
                       somewhere that doesn't infer it (e.g. passed directly
                       as a call argument) — matches the historical
                       int*-by-default behavior. */
                    if (e->elem_type == TYPE_VOID) e->elem_type = TYPE_INT;
                    return TYPE_PTR;
                }
                if (strcmp(e->name, "free") == 0) {
                    if (argc != 1) sem_error("free takes exactly 1 argument (ptr)", NULL);
                    return TYPE_VOID;
                }
                return TYPE_VOID; /* unreachable: yalnizca alloc/free rezerve */
            }
            /* M9: list builtins are checked here instead of going through
               the user function-signature table (they have no single fixed
               signature: push/listCopy are generic over element type). */
            if (is_list_builtin(e->name)) {
                int argc = 0;
                ExprList *l = e->args;
                TypeKind arg_types[8];
                for (; l && argc < 8; l = l->next, argc++) arg_types[argc] = check_expr(st, l->expr);
                for (; l; l = l->next) check_expr(st, l->expr); /* still typecheck extras */
                if (strcmp(e->name, "push") == 0) {
                    if (argc != 2) {
                        sem_error("push takes exactly 2 arguments (list, value)", NULL);
                        return TYPE_VOID;
                    }
                    if (arg_types[0] != TYPE_INT_LIST && arg_types[0] != TYPE_BYTE_LIST) {
                        sem_error("push's first argument must be a list<int> or list<byte>", NULL);
                    } else if (arg_types[0] == TYPE_INT_LIST && arg_types[1] != TYPE_INT) {
                        sem_error("push's second argument must be an int", NULL);
                    } else if (arg_types[0] == TYPE_BYTE_LIST && arg_types[1] != TYPE_BYTE && arg_types[1] != TYPE_INT) {
                        sem_error("push's second argument must be a byte (or int)", NULL);
                    }
                    if (e->args->expr->kind != EXPR_IDENT) {
                        sem_error("push's first argument must be a list variable", NULL);
                    }
                    return TYPE_VOID;
                }
            }
            /* M41: toInt(enum) std'de ifade edilemez (her enum ayri bir tip);
               derleyici-icsel kalir. Diger her toInt cagrisi std/std.rapid'deki
               overload'lara gider. */
            if (strcmp(e->name, "toInt") == 0 && e->args && !e->args->next) {
                if (check_expr(st, e->args->expr) == TYPE_ENUM) {
                    e->name = strdup("__enum_to_int");
                    return TYPE_INT;
                }
            }
            if (overload_count(e->name) > 1) resolve_overload(st, e);
            FuncSig *fs = func_find(e->name);
            if (!fs) {
                if (is_moved_to_std(e->name)) {
                    fprintf(stderr, "semantic error: note: `%s` artik derleyici builtin'i degil, std/std.rapid'de. "
                                    "Dosyanin basina `use \"std/std.rapid\";` ekleyin.\n", e->name);
                }
                sem_error("call to undefined function", e->name);
                for (ExprList *l = e->args; l; l = l->next) check_expr(st, l->expr);
                return TYPE_VOID;
            }
            int i = 0;
            ExprList *l = e->args;
            for (; l && i < fs->param_count; l = l->next, i++) {
                TypeKind at = check_expr(st, l->expr);
                if (at != fs->param_types[i]) {
                    if (!types_int_compatible(fs->param_types[i], at) &&
                        !types_float_compatible(fs->param_types[i], at) &&
                        !null_fits_string(fs->param_types[i], l->expr)) { /* M37 */
                        {
                            const char *bi = NULL;
                            if (mangled_builtin_shadow(e->name, &bi)) {
                                fprintf(stderr, "semantic error: note: `%s` looks like a user function that shadows the builtin `%s` "
                                        "(same-file `%s(...)` calls are all renamed by the module system, "
                                        "including builtin ones). Rename your function or avoid calling the builtin `%s` in that file.\n",
                                        e->name, bi, bi, bi);
                            }
                        }
                        sem_error("argument type mismatch in call to", e->name);
                    }
                } else if (at == TYPE_FN) {
                    if (fn_type_mismatch(fs->param_fn_types[i], fn_type_of_expr(st, l->expr)))
                        sem_error("function signature mismatch in argument of call to", e->name);
                } else if (at == TYPE_STRUCT || at == TYPE_ENUM) {
                    if (named_mismatch(st, at, fs->param_struct_names[i], l->expr))
                        sem_error("struct/enum type mismatch in argument of call to", e->name);
                } else if (at == TYPE_PTR) {
                    /* M26: both sides are TYPE_PTR at the flat-enum level —
                       still need matching elem_type (int* passed where
                       float* expected shouldn't type-check just because
                       both are "a pointer").
                       M33: `f(null)` — null has no elem_type of its own,
                       so it's compatible with any T* parameter. */
                    if (l->expr->kind == EXPR_NULL_LITERAL) continue;
                    TypeKind arg_elem = expr_elem_type_of(st, l->expr);
                    if (arg_elem != fs->param_elem_types[i]) {
                        sem_error("argument pointer type mismatch (pointee types differ) in call to", e->name);
                    } else if (arg_elem == TYPE_PTR) {
                        /* M32: T** passed where U** expected — elem_type
                           matching alone isn't enough (both are TYPE_PTR),
                           need the pointee-of-the-pointee to match too
                           (int** passed where byte** expected shouldn't
                           type-check just because both are "pointer to
                           pointer"). */
                        if (expr_elem_elem_type_of(st, l->expr) != fs->param_elem_elem_types[i]) {
                            sem_error("argument pointer type mismatch (pointee types differ) in call to", e->name);
                        }
                    }
                }
            }
            /* leftover args or leftover params means arity mismatch —
               except for a variadic extern (M27: e.g. printf), where extra
               args beyond the declared fixed params are allowed; they're
               still type-checked (just not matched against a declared
               parameter type, since there isn't one). */
            int arg_count = 0;
            for (ExprList *c = e->args; c; c = c->next) arg_count++;
            for (; l; l = l->next) check_expr(st, l->expr); /* still typecheck extras */
            if (arg_count != fs->param_count) {
                if (!(fs->is_variadic && arg_count > fs->param_count)) {
                    sem_error("wrong number of arguments in call to", e->name);
                }
            }
            e->elem_type = fs->ret_elem_type; /* M26: propagate for `var p: T* = f(...);` */
            e->elem_elem_type = fs->ret_elem_elem_type; /* M32: propagate for `var pp: T** = f(...);` */
            return fs->ret_type;
        }
        case EXPR_FIELD: {
            /* M8: `base.field` — base must be struct-typed.
               BUG-02 fix: base used to be required to be a bare EXPR_IDENT
               (so only `p.f` worked, never `o.inner.a`); now base may be any
               struct-typed expression, including another EXPR_FIELD, and
               struct_def_of_expr resolves its struct definition recursively. */
            TypeKind base_type = check_expr(st, e->lhs);
            if (base_type != TYPE_STRUCT) {
                sem_error("field access requires a struct variable", NULL);
                return TYPE_INT;
            }
            StructDef *sd = struct_def_of_expr(e->lhs);
            if (!sd) {
                sem_error("cannot determine struct type of expression", e->lhs->kind == EXPR_IDENT ? e->lhs->name : e->name);
                return TYPE_INT;
            }
            for (Field *f = sd->fields; f; f = f->next) {
                if (strcmp(f->name, e->name) == 0) return f->type;
            }
            sem_error("no such field in struct", e->name);
            return TYPE_INT;
        }
        case EXPR_ENUM_LITERAL: {
            /* M8: `Color::GREEN` — enum and member must both exist. */
            EnumDef *ed = e->enum_name ? enum_find(e->enum_name) : NULL;
            if (!ed) {
                sem_error("undefined enum", e->enum_name);
                return TYPE_ENUM;
            }
            for (EnumMember *m = ed->members; m; m = m->next) {
                if (strcmp(m->name, e->member_name) == 0) return TYPE_ENUM;
            }
            sem_error("no such member in enum", e->member_name);
            return TYPE_ENUM;
        }
        case EXPR_STRUCT_LITERAL: {
            /* M8: `Point { x: 1, y: 2 }` — every listed field must exist in
               the struct, appear at most once, and type-match. */
            StructDef *sd = e->name ? struct_find(e->name) : NULL;
            if (!sd) {
                sem_error("undefined struct", e->name);
                return TYPE_STRUCT;
            }
            for (StructLiteralField *fl = e->struct_fields; fl; fl = fl->next) {
                Field *fd = NULL;
                for (Field *f = sd->fields; f; f = f->next) {
                    if (strcmp(f->name, fl->field) == 0) { fd = f; break; }
                }
                if (!fd) {
                    sem_error("no such field in struct literal", fl->field);
                    continue;
                }
                for (StructLiteralField *prev = e->struct_fields; prev != fl; prev = prev->next) {
                    if (strcmp(prev->field, fl->field) == 0) {
                        sem_error("duplicate field in struct literal", fl->field);
                    }
                }
                TypeKind vt = check_expr(st, fl->value);
                if (vt != fd->type) {
                    /* M40: diger yerlerdeki gibi int<->int orulu uyum (float alanlar struct'ta zaten desteklenmiyor: alanlar storel ile yazilir) */
                    if (!types_int_compatible(fd->type, vt) &&
                        !null_fits_string(fd->type, fl->value))
                        sem_error("type mismatch in struct literal field", fl->field);
                } else if (named_mismatch(st, vt, fd->struct_name, fl->value)) {
                    sem_error("struct/enum type mismatch in struct literal field", fl->field);
                }
            }
            return TYPE_STRUCT;
        }
        case EXPR_BINOP: {
            TypeKind lt = check_expr(st, e->lhs);
            TypeKind rt = check_expr(st, e->rhs);
            /* M37: `s == null`, `null == s`, `s != null`, `null != s` (s: string).
               `null` LITERAL'i ile yalniz esitlik/esitsizlik anlamlidir; sirali
               karsilastirma (`s < null`) ve aritmetik (`s + null`) reddedilir —
               bunlari degiskenler uzerinden yazin (runtime NULL-guvenli). Bu
               karsilastirma pointer karsilastirmasidir (codegen'de `ceql`/`cnel`,
               strcmp DEGIL: strcmp yalniz iki taraf da TYPE_STRING iken kullanilir). */
            if ((e->op == OP_EQ || e->op == OP_NE) &&
                ((lt == TYPE_STRING && e->rhs->kind == EXPR_NULL_LITERAL) ||
                 (rt == TYPE_STRING && e->lhs->kind == EXPR_NULL_LITERAL))) {
                return TYPE_BOOL;
            }
            /* M40: enum <op> enum. Iki taraf da AYNI enum olmali; yalnizca
               ==, != ve siralama anlamli (aritmetik/bit islemi reddedilir).
               enum <-> int icin acik toInt(enum) kullanilir. */
            if (lt == TYPE_ENUM && rt == TYPE_ENUM) {
                const char *ln = named_type_of(st, e->lhs), *rn = named_type_of(st, e->rhs);
                if (ln && rn && strcmp(ln, rn) != 0) {
                    sem_error("cannot compare values of different enum types", NULL);
                }
                switch (e->op) {
                    case OP_EQ: case OP_NE:
                    case OP_LT: case OP_LE: case OP_GT: case OP_GE:
                        return TYPE_BOOL;
                    default:
                        sem_error("only comparison operators are allowed on enum values (use toInt(x) for arithmetic)", NULL);
                        return TYPE_BOOL;
                }
            }
            if (lt != rt) {
                /* M23: any two integer types are compatible in binary
                   expressions (widened/narrowed implicitly, same as the
                   original byte<->int behavior). */
                int compat = types_int_compatible(lt, rt);
                /* M25: any two float-family types are likewise compatible
                   (float op double promotes to double). */
                int fcompat = types_float_compatible(lt, rt);
                 /* M26: pointer arithmetic: T* + <any integer> or T* - <any
                    integer>, and (C-style) <any integer> + T*. Subtraction
                    of an integer from a pointer (T* - int) advances backwards
                    by that many elements; int - T* is rejected (not valid
                    pointer arithmetic). generalized off TYPE_PTR instead of
                    the old two hardcoded pointer type names. */
                int ptr_arith = (lt == TYPE_PTR && type_is_integer(rt)) ||
                                 (rt == TYPE_PTR && type_is_integer(lt) && e->op == OP_ADD);
                if (!compat && !fcompat && !ptr_arith) {
                    sem_error("type mismatch in binary expression", NULL);
                    return TYPE_BOOL;
                }
                if (ptr_arith) {
                    /* only + and - are allowed; result is the same pointer
                       kind (elem_type carried on `e` itself so callers of
                       check_expr(e) on this binop can still recover it). */
                    if (e->op != OP_ADD && e->op != OP_SUB) {
                        sem_error("only + and - are allowed for pointer arithmetic", NULL);
                    }
                    /* The pointee info comes from whichever side is actually
                       the pointer (lhs for T* +/- int, rhs for int + T*). */
                    Expr *ptr_side = (lt == TYPE_PTR) ? e->lhs : e->rhs;
                    TypeKind lelem = expr_elem_type_of(st, ptr_side);
                    e->elem_type = lelem;
                    /* M32: T** +/- int is still T** — propagate the
                       pointee-of-the-pointee too, same as elem_type just
                       above, so a further *result or a decl/assign check
                       against this binop still sees the full two-level
                       shape via expr_elem_elem_type_of(). */
                    if (lelem == TYPE_PTR) {
                        e->elem_elem_type = expr_elem_elem_type_of(st, ptr_side);
                    }
                    return TYPE_PTR;
                }
                if (fcompat) {
                    lt = rt = type_float_promote(lt, rt);
                } else {
                    /* mixed integer widths → promote per usual-arithmetic-conversion rules */
                    lt = rt = type_int_promote(lt, rt);
                }
            }
            /* M26: pointer op pointer — comparison is always OK, only
               subtraction for arithmetic. Both sides must point to the
               same elem_type (a T* and a U* aren't comparable/subtractable
               even though both are TYPE_PTR at the flat-enum level).
               M33: `null` is exempt from the elem_type match — it has no
               fixed pointee type of its own, so `p == null` (any T*) and
               `null == null` both type-check without comparing elem_types. */
            if (lt == TYPE_PTR && rt == TYPE_PTR) {
                int lhs_null = (e->lhs->kind == EXPR_NULL_LITERAL);
                int rhs_null = (e->rhs->kind == EXPR_NULL_LITERAL);
                if (!lhs_null && !rhs_null) {
                    TypeKind lelem = expr_elem_type_of(st, e->lhs);
                    TypeKind relem = expr_elem_type_of(st, e->rhs);
                    if (lelem != relem) {
                        sem_error("pointer type mismatch (pointee types differ)", NULL);
                    } else if (lelem == TYPE_PTR) {
                        /* M32: T** vs U** — elem_type matching alone isn't
                           enough (both are TYPE_PTR at that level too); the
                           pointee-of-the-pointee must also match (int** vs
                           byte** shouldn't compare/subtract just because both
                           are "pointer to pointer"). */
                        if (expr_elem_elem_type_of(st, e->lhs) != expr_elem_elem_type_of(st, e->rhs)) {
                            sem_error("pointer type mismatch (pointee types differ)", NULL);
                        }
                    }
                }
                switch (e->op) {
                    case OP_EQ: case OP_NE:
                    case OP_LT: case OP_LE: case OP_GT: case OP_GE:
                        return TYPE_BOOL;
                    case OP_SUB:
                        return TYPE_INT;  /* pointer difference */
                    default:
                        sem_error("only comparison or subtraction allowed between two pointers", NULL);
                        return TYPE_INT;
                }
            }
            switch (e->op) {
                case OP_EQ: case OP_NE:
                    /* Allow comparison of pointers, any integer type, any
                       float type, strings, or bools (bool needed so
                       `flag == true`/`x == false` work now that `true`/
                       `false` exist as literals — previously unreachable
                       since there was no bool-typed expression that wasn't
                       already the result of a comparison). */
                    if (!type_is_integer(lt) && !type_is_float(lt) && lt != TYPE_STRING &&
                        lt != TYPE_PTR && lt != TYPE_BOOL) {
                        sem_error("equality comparison requires an integer, float, string, bool, or pointer operand", NULL);
                    }
                    return TYPE_BOOL;
                case OP_LT: case OP_LE: case OP_GT: case OP_GE:
                    if (!type_is_integer(lt) && !type_is_float(lt) && lt != TYPE_PTR && lt != TYPE_STRING) {
                        sem_error("relational comparison requires an integer, float, string, or pointer operand", NULL);
                    }
                    return TYPE_BOOL;
                case OP_ADD:
                    /* `+` works for any integer type (arithmetic), any float
                       type, and string (concatenation) */
                    if (!type_is_integer(lt) && !type_is_float(lt) && lt != TYPE_STRING) {
                        sem_error("'+' requires an integer, float, or string operand", NULL);
                    }
                    return lt;
                case OP_SUB: case OP_MUL: case OP_DIV:
                    if (!type_is_integer(lt) && !type_is_float(lt)) {
                        sem_error("arithmetic operator requires an integer or float operand", NULL);
                    }
                    return lt;
                case OP_POW:
                    /* ** : integer or float, same operand-type rule as
                       the arithmetic ops above; result is the
                       (already-promoted) operand type. */
                    if (!type_is_integer(lt) && !type_is_float(lt)) {
                        sem_error("'**' requires an integer or float operand", NULL);
                    }
                    return lt;
                case OP_MOD:
                    /* % : integer-only remainder, no float modulo support. */
                    if (!type_is_integer(lt)) {
                        sem_error("'%' requires an integer operand", NULL);
                    }
                    return lt;
                case OP_BAND: case OP_BOR: case OP_BXOR:
                case OP_SHL: case OP_SHR:
                    /* Bitwise/shift ops: integer-only, no float or pointer
                       bit-twiddling support. */
                    if (!type_is_integer(lt)) {
                        sem_error("bitwise/shift operator requires an integer operand", NULL);
                    }
                    return lt;
            }
            return TYPE_VOID;
        }
    }
    return TYPE_VOID;
}

static void check_block(SymTable *st, Stmt *body) {
    /* block scope: locals declared inside are not visible after the block */
    int saved_count = st->count;
    for (Stmt *s = body; s; s = s->next) {
        check_stmt(st, s);
    }
    st->count = saved_count;
}

static void check_stmt(SymTable *st, Stmt *s) {
    int sv_line = g_cur_line; const char *sv_file = g_cur_file;
    if (s && s->line > 0) { g_cur_line = s->line; g_cur_file = s->file; }
    check_stmt_inner(st, s);
    g_cur_line = sv_line; g_cur_file = sv_file;
}

static void check_stmt_inner(SymTable *st, Stmt *s) {
    switch (s->kind) {
        case STMT_VAR_DECL:
        case STMT_CONST_DECL: {
            if (s->type == TYPE_STRUCT && !s->struct_name) {
                sem_error("struct-typed variable is missing its struct name", s->name);
            }
            if (s->expr) {
                TypeKind init_type = check_expr(st, s->expr);
                if (init_type == s->type && named_mismatch(st, init_type, s->struct_name, s->expr))
                    sem_error("struct/enum type mismatch in declaration", s->name);
                if (init_type == TYPE_FN && s->type == TYPE_FN && fn_type_mismatch(s->fn_type, fn_type_of_expr(st, s->expr)))
                    sem_error("function signature mismatch in declaration", s->name);
                if (s->type == TYPE_STRUCT) {
                    if (init_type != TYPE_STRUCT) {
                        sem_error("type mismatch in declaration", s->name);
                    } else if (s->expr->kind == EXPR_STRUCT_LITERAL && s->struct_name &&
                               strcmp(s->expr->name, s->struct_name) != 0) {
                        sem_error("struct literal type mismatch in declaration", s->name);
                    }
                } else if (s->type == TYPE_INT_LIST) {
                    /* M9: `list<int> xs = [1, 2, 3];` (or `= [];`) is allowed —
                        an array literal initializes a dynamic list. Anything
                        else must already be TYPE_INT_LIST (e.g. listCopy(...)
                        or another list variable). (M35: the array-literal
                        branch here used to check against the now-removed
                        TYPE_ARRAY; check_expr's EXPR_ARRAY_LITERAL case
                        returns TYPE_INT_LIST directly now, so this
                        single check still covers both cases.) */
                    if (init_type != TYPE_INT_LIST) {
                        sem_error("type mismatch in declaration", s->name);
                    }
                } else if (s->type == TYPE_BYTE_LIST) {
                    /* M20: `list<byte> bs = [];` / `= listCopy(other);` /
                        `= other;` — mirrors the TYPE_INT_LIST case above. An
                        array literal here must be empty (byte array literal
                        syntax doesn't exist yet), otherwise the initializer
                        must already be a byte list. */
                    if (init_type != TYPE_BYTE_LIST &&
                        !(init_type == TYPE_INT_LIST && s->expr->kind == EXPR_ARRAY_LITERAL && s->expr->elem_count == 0)) {
                        sem_error("type mismatch in declaration", s->name);
                    }
                /* M35-arraylib: the `s->type == TYPE_ARRAY` branch that used
                   to live here (`T[] a = [1,2,3];`) is gone — TYPE_ARRAY can
                   no longer be a declared variable type (the grammar rule
                   that produced it was removed from `type`), so this branch
                   was unreachable dead code and has been deleted rather than
                   kept around. Use `var a: Array = array::new(5);` from
                   std/array.rapid instead (Array is an ordinary TYPE_STRUCT,
                   handled by the TYPE_STRUCT branch above). */
                } else if (s->type == TYPE_PTR) {
                    /* M26: `T* p = alloc(n);` — alloc() is a flat untyped
                       builtin (always statically TYPE_PTR with no elem_type
                       of its own), so its elem_type is inferred here from
                       the declared target type, same spot the old int
                       pointer / byte pointer alloc_ptr_compat carve-out
                       used to live.
                       Any other pointer-typed initializer (address-of,
                       another pointer variable, a function call returning
                       T*, a deref of a T**-shaped expr) must already carry
                       a matching elem_type. */
                    if (init_type != TYPE_PTR) {
                        sem_error("type mismatch in declaration", s->name);
                    } else if (s->expr->kind == EXPR_CALL && strcmp(s->expr->name, "alloc") == 0) {
                        s->expr->elem_type = s->elem_type; /* infer alloc's pointee from the target */
                        s->expr->elem_elem_type = s->elem_elem_type; /* M32: same, one level deeper */
                    } else if (s->expr->kind == EXPR_NULL_LITERAL) {
                        /* M33: `var p: T* = null;` — null has no elem_type
                           of its own, so it's compatible with any T*
                           declared target (mirrors the alloc() carve-out
                           above). */
                    } else {
                        TypeKind init_elem = expr_elem_type_of(st, s->expr);
                        if (init_elem != s->elem_type) {
                            sem_error("pointer type mismatch in declaration (pointee types differ)", s->name);
                        } else if (init_elem == TYPE_PTR && expr_elem_elem_type_of(st, s->expr) != s->elem_elem_type) {
                            /* M32: `var pp: int** = other_pp;` where
                               other_pp is a byte** — both sides are T*
                               pointing at "a pointer" (elem_type ==
                               TYPE_PTR matches), but the pointee-of-the-
                               pointee still needs to match. */
                            sem_error("pointer type mismatch in declaration (pointee types differ)", s->name);
                        }
                    }
                } else if (init_type != s->type) {
                    /* M23: any two integer types are interchangeable for
                       initialization (e.g. an int literal can initialize a
                       byte/int8/uint32/... variable and vice versa) — same
                       implicit-widen/narrow behavior the language already
                       had for byte<->int, generalized to every width.
                       M25: likewise, any two float-family types (float,
                       double) are interchangeable for initialization. */
                    int compat = types_int_compatible(s->type, init_type) ||
                                 types_float_compatible(s->type, init_type) ||
                                 null_fits_string(s->type, s->expr); /* M37 */
                    if (!compat) {
                        sem_error("type mismatch in declaration", s->name);
                    }
                }
            } else {
                /* M8: only struct declarations may omit an initializer
                   (`var p: Point;` — zero-initialized). */
                if (s->type != TYPE_STRUCT) {
                    sem_error("declaration requires an initializer", s->name);
                }
            }
            sym_add(st, s->name, s->type, s->kind == STMT_CONST_DECL, s->struct_name, s->elem_type, s->elem_elem_type);
            if (s->type == TYPE_FN) sym_set_fn_type(st, s->name, s->fn_type); /* M40 */
            break;
        }
        case STMT_RETURN:
            if (s->expr) {
                TypeKind rt = check_expr(st, s->expr);
                if (rt == current_fn_ret_type && named_mismatch(st, rt, current_fn_ret_struct_name, s->expr))
                    sem_error("struct/enum type mismatch in return", NULL);
                if (rt != current_fn_ret_type) {
                    if (!types_int_compatible(current_fn_ret_type, rt) &&
                        !types_float_compatible(current_fn_ret_type, rt) &&
                        !null_fits_string(current_fn_ret_type, s->expr)) { /* M37 */
                        sem_error("return type mismatch", NULL);
                    }
                } else if (rt == TYPE_PTR) {
                    /* M26: mirrors STMT_VAR_DECL/STMT_ASSIGN — infer
                       alloc()'s elem_type from the function's declared
                       return type, otherwise require a matching elem_type. */
                    if (s->expr->kind == EXPR_CALL && strcmp(s->expr->name, "alloc") == 0) {
                        s->expr->elem_type = current_fn_ret_elem_type;
                        s->expr->elem_elem_type = current_fn_ret_elem_elem_type; /* M32 */
                    } else if (s->expr->kind == EXPR_NULL_LITERAL) {
                        /* M33: `return null;` — see STMT_VAR_DECL's
                           carve-out above; null matches any T* return type. */
                    } else if (expr_elem_type_of(st, s->expr) != current_fn_ret_elem_type) {
                        sem_error("pointer return type mismatch (pointee types differ)", NULL);
                    } else if (current_fn_ret_elem_type == TYPE_PTR &&
                               expr_elem_elem_type_of(st, s->expr) != current_fn_ret_elem_elem_type) {
                        /* M32: `fn f(): int** { return pp; }` where pp is a
                           byte** — elem_type (TYPE_PTR) matches at the
                           first level, but the pointee-of-the-pointee still
                           needs to match. */
                        sem_error("pointer return type mismatch (pointee types differ)", NULL);
                    }
                }
            } else if (current_fn_ret_type != TYPE_VOID) {
                sem_error("missing return value for non-void function", NULL);
            }
            break;
        case STMT_EXPR:
            check_expr(st, s->expr);
            break;
        case STMT_IF: {
            TypeKind cond_type = check_expr(st, s->expr);
            if (cond_type != TYPE_BOOL) {
                sem_error("if condition must be bool", NULL);
            }
            check_block(st, s->then_body);
            if (s->else_body) {
                /* else_body is either a single STMT_IF (else-if chain) or a block's stmt list */
                if (s->else_body->kind == STMT_IF && s->else_body->next == NULL) {
                    check_stmt(st, s->else_body);
                } else {
                    check_block(st, s->else_body);
                }
            }
            break;
        }
        case STMT_ASSIGN: {
            int idx = sym_find(st, s->name);
            if (idx == -1) {
                sem_error("undefined identifier", s->name);
                break;
            }
            if (st->is_const[idx]) {
                sem_error("cannot assign to const", s->name);
                break;
            }
            TypeKind val_type = check_expr(st, s->expr);
            if (val_type == st->types[idx] && named_mismatch(st, val_type, st->struct_names[idx], s->expr))
                sem_error("struct/enum type mismatch in assignment", s->name);
            if (val_type == TYPE_FN && st->types[idx] == TYPE_FN && fn_type_mismatch(st->fn_types[idx], fn_type_of_expr(st, s->expr)))
                sem_error("function signature mismatch in assignment", s->name);
            if (st->types[idx] == TYPE_PTR) {
                /* M26: mirrors STMT_VAR_DECL's TYPE_PTR handling — infer
                   alloc()'s elem_type from the assignment target, otherwise
                   require the value's elem_type to already match. */
                if (val_type != TYPE_PTR) {
                    sem_error("type mismatch in assignment", s->name);
                } else if (s->expr->kind == EXPR_CALL && strcmp(s->expr->name, "alloc") == 0) {
                    s->expr->elem_type = st->elem_types[idx];
                    s->expr->elem_elem_type = st->elem_elem_types[idx]; /* M32 */
                } else if (s->expr->kind == EXPR_NULL_LITERAL) {
                    /* M33: `p = null;` — see STMT_VAR_DECL's carve-out above. */
                } else {
                    TypeKind val_elem = expr_elem_type_of(st, s->expr);
                    if (val_elem != st->elem_types[idx]) {
                        sem_error("pointer type mismatch in assignment (pointee types differ)", s->name);
                    } else if (val_elem == TYPE_PTR && expr_elem_elem_type_of(st, s->expr) != st->elem_elem_types[idx]) {
                        /* M32: `pp = other_pp;` where pp: int** and
                           other_pp: byte** — elem_type (TYPE_PTR) matches,
                           pointee-of-the-pointee doesn't. */
                        sem_error("pointer type mismatch in assignment (pointee types differ)", s->name);
                    }
                }
            } else if (val_type != st->types[idx]) {
                int compat = types_int_compatible(st->types[idx], val_type) ||
                             types_float_compatible(st->types[idx], val_type) ||
                             null_fits_string(st->types[idx], s->expr); /* M37 */
                if (!compat) {
                    sem_error("type mismatch in assignment", s->name);
                }
            }
            break;
        }
        case STMT_INDEX_ASSIGN: {
            TypeKind arr_type = check_expr(st, s->target);
            TypeKind idx_type = check_expr(st, s->index);
            TypeKind val_type = check_expr(st, s->value);
            if (idx_type != TYPE_INT) {
                sem_error("array index must be int", NULL);
            }
            if (arr_type == TYPE_STRING || arr_type == TYPE_BYTE_LIST) {
                /* M16/M20: s[i] = b / bs[i] = b — b must be a byte (int also
                   accepted, same byte/int compat rule used elsewhere). */
                if (!type_is_integer(val_type)) {
                    sem_error("string/byte-list index assignment requires a byte value", NULL);
                }
            } else if (arr_type == TYPE_PTR) {
                /* M26-add: p[i] = v (pointer indexed assign) — the value
                   must match the pointee type, checked with the same
                   width-aware rules as *p = v. (M35: `a[i] = v` on a
                   fixed-size array used to share this branch too; that
                   type no longer exists.) */
                TypeKind elem = expr_elem_type_of(st, s->target);
                if (type_is_integer(elem)) {
                    if (!type_is_integer(val_type)) sem_error("type mismatch in indexed assignment", NULL);
                } else if (type_is_float(elem)) {
                    if (!type_is_float(val_type)) sem_error("type mismatch in indexed assignment", NULL);
                } else if (elem == TYPE_PTR) {
                    if (val_type != TYPE_PTR || expr_elem_type_of(st, s->value) != expr_elem_elem_type_of(st, s->target)) {
                        sem_error("pointer type mismatch in indexed assignment", NULL);
                    }
                } else if (val_type != elem && !null_fits_string(elem, s->value)) { /* M37: `sp[i] = null` (sp: string*) */
                    sem_error("type mismatch in indexed assignment", NULL);
                }
            } else if (arr_type == TYPE_INT_LIST) {
                /* list<int>[i] = v handled structurally the same way lists
                   always have been: element type is fixed (TYPE_INT), so a
                   plain integer-compat check is enough (mirrors the old
                   "legacy TYPE_INT_ARRAY" comment this replaces — that
                   fixed-size-array type is gone in M35). */
                if (val_type != TYPE_INT) {
                    sem_error("type mismatch in indexed assignment", NULL);
                }
            } else {
                /* Not string/byte-list (handled above), not a pointer, not
                   list<int> — nothing else is an indexable lvalue anymore
                   now that fixed-size arrays are gone. Use
                   array::set(a, i, v) from std/array.rapid instead. */
                sem_error("indexed assignment requires a list, string, or pointer (fixed-size arrays were removed; use array::set from std/array.rapid)", NULL);
            }
            break;
        }
        case STMT_DEREF_ASSIGN: {
            TypeKind target_type = check_expr(st, s->target);
            TypeKind val_type = check_expr(st, s->value);
            if (target_type != TYPE_PTR) {
                sem_error("dereference assignment requires a pointer", NULL);
                break;
            }
            TypeKind elem = expr_elem_type_of(st, s->target);
            /* M26/M32: value must be compatible with the pointee type —
               any integer width for an integer-family pointee, any float
               width for a float-family pointee, matching elem_type (and,
               if that's itself TYPE_PTR, matching elem_elem_type too) for
               a pointer-family pointee — e.g. `*pp = q;` where pp: T**
               requires q: T* with the same T. */
            int ok;
            if (type_is_integer(elem)) {
                ok = type_is_integer(val_type);
            } else if (type_is_float(elem)) {
                ok = type_is_float(val_type);
            } else if (elem == TYPE_PTR) {
                TypeKind target_elem2 = expr_elem_elem_type_of(st, s->target);
                ok = (val_type == TYPE_PTR && expr_elem_type_of(st, s->value) == target_elem2);
            } else {
                ok = (val_type == elem);
            }
            if (!ok) {
                sem_error("type mismatch in dereference assignment", NULL);
            }
            break;
        }
        case STMT_FIELD_ASSIGN: {
            /* M8: `p.f = expr;` — p must be struct-typed, f must exist and
               the value must match the field's type.
               BUG-03 fix: p (s->target) used to be required to be a bare
               EXPR_IDENT (so only `p.f = ...` worked, never
               `o.inner.f = ...`); now it may be any struct-typed expression,
               resolved via struct_def_of_expr like the read path above. */
            TypeKind base_type = check_expr(st, s->target);
            if (base_type != TYPE_STRUCT) {
                sem_error("field assignment requires a struct variable", NULL);
                check_expr(st, s->value);
                break;
            }
            StructDef *sd = struct_def_of_expr(s->target);
            if (!sd) {
                sem_error("cannot determine struct type of expression", s->target->kind == EXPR_IDENT ? s->target->name : s->field);
                check_expr(st, s->value);
                break;
            }
            Field *fd = NULL;
            for (Field *f = sd->fields; f; f = f->next) {
                if (strcmp(f->name, s->field) == 0) { fd = f; break; }
            }
            if (!fd) {
                sem_error("no such field in struct", s->field);
                check_expr(st, s->value);
                break;
            }
            TypeKind val_type = check_expr(st, s->value);
            if (val_type != fd->type && !null_fits_string(fd->type, s->value)) { /* M37 */
                sem_error("type mismatch in field assignment", s->field);
            }
            break;
        }
        case STMT_INC:
        case STMT_DEC: {
            int idx = sym_find(st, s->name);
            if (idx == -1) {
                sem_error("undefined identifier", s->name);
                break;
            }
            if (st->is_const[idx]) {
                sem_error("cannot modify const", s->name);
                break;
            }
             if (!type_is_integer(st->types[idx]) && st->types[idx] != TYPE_PTR) {
                 sem_error("++/-- requires an integer or pointer type", s->name);
             }
            break;
        }
        case STMT_WHILE: {
            TypeKind cond_type = check_expr(st, s->expr);
            if (cond_type != TYPE_BOOL) {
                sem_error("while condition must be bool", NULL);
            }
            semantic_loop_depth++;
            check_block(st, s->body);
            semantic_loop_depth--;
            break;
        }
        case STMT_DO_WHILE: {
            /* M33: do { body } while (cond); — body runs first, then cond
               is checked (and re-checked, looping back) same as STMT_WHILE
               but with the check after the body instead of before it. Body
               is checked before the condition so a loop var declared in the
               body isn't visible to the condition (matches the body's own
               block scope, which check_block already closes on return). */
            semantic_loop_depth++;
            check_block(st, s->body);
            semantic_loop_depth--;
            TypeKind cond_type = check_expr(st, s->expr);
            if (cond_type != TYPE_BOOL) {
                sem_error("do-while condition must be bool", NULL);
            }
            break;
        }
        case STMT_FOR: {
            /* for's init variable is scoped to the whole for statement (init+cond+post+body) */
            int saved_count = st->count;
            check_stmt(st, s->init);
            TypeKind cond_type = check_expr(st, s->expr);
            if (cond_type != TYPE_BOOL) {
                sem_error("for condition must be bool", NULL);
            }
            check_stmt(st, s->post);
            semantic_loop_depth++;
            check_block(st, s->body);
            semantic_loop_depth--;
            st->count = saved_count;
            break;
        }
        case STMT_FOR_IN: {
            /* `for (x in xs)`: xs must be a list<int>/list<byte> now that
               fixed-size arrays (TYPE_ARRAY/TYPE_INT_ARRAY) are gone; x is
               bound to its element type, scoped to the loop body only. To
               iterate a std/array.rapid Array, use a classic counted
               `for (var i: int = 0; i < array::len(a); i++)` loop instead —
               Array is an ordinary struct, not something this core for-in
               special-cases. */
            TypeKind arr_type = check_expr(st, s->expr);
            if (arr_type != TYPE_INT_LIST && arr_type != TYPE_BYTE_LIST) {
                sem_error("for-in requires a list (fixed-size arrays were removed; use a counted for-loop with array::len/array::get from std/array.rapid)", NULL);
            }
            TypeKind elem = element_type_of(arr_type);
            int saved_count = st->count;
            sym_add(st, s->name, elem, 0 /* loop var is mutable */, NULL, TYPE_VOID, TYPE_VOID);
            semantic_loop_depth++;
            for (Stmt *bs = s->body; bs; bs = bs->next) {
                check_stmt(st, bs);
            }
            semantic_loop_depth--;
            st->count = saved_count;
            break;
        }
        case STMT_FAIL: {
            if (!current_fn_can_fail)
                sem_error("fail used in a function that cannot fail (declare it with `: !T`)", NULL);
            TypeKind ft = check_expr(st, s->expr);
            if (ft != TYPE_INT) sem_error("fail requires an error value (error::Name or an int)", NULL);
            break;
        }
        case STMT_BREAK:
            if (semantic_loop_depth == 0) {
                sem_error("break outside of loop", NULL);
            }
            break;
        case STMT_CONTINUE:
            if (semantic_loop_depth == 0) {
                sem_error("continue outside of loop", NULL);
            }
            break;
        case STMT_SWITCH: {
            TypeKind subj_type = check_expr(st, s->expr);
            int saw_default = 0;
            for (CaseClause *c = s->cases; c; c = c->next) {
                if (c->value) {
                    TypeKind vt = check_expr(st, c->value);
                    if (vt != subj_type) {
                        sem_error("case value type must match switch subject", NULL);
                    }
                } else {
                    if (saw_default) {
                        sem_error("multiple default clauses in switch", NULL);
                    }
                    saw_default = 1;
                }
                /* Each case body is its own block scope; break exits the switch,
                   so it's treated like being inside a loop for that purpose. */
                semantic_loop_depth++;
                check_block(st, c->body);
                semantic_loop_depth--;
            }
            break;
        }
    }
}

static void check_function_body(Function *f) {
    if (f->line > 0) { g_cur_line = f->line; g_cur_file = f->file; }
    SymTable st;
    st.count = 0;
    current_fn_ret_type = f->ret_type;
    current_fn_ret_elem_type = f->ret_elem_type; /* M26 */
    current_fn_ret_elem_elem_type = f->ret_elem_elem_type; /* M32 */
    current_fn_ret_struct_name = f->ret_struct_name; /* M40 */
    current_fn_can_fail = f->can_fail;               /* M43 */
    current_fn_params = f->params; /* M33 */
    var_struct_map_clear(); /* M36: harita artik fonksiyon-yerel (onceki fonksiyonun adlari sizmasin) */
    for (Param *p = f->params; p; p = p->next) {
        sym_add(&st, p->name, p->type, 0 /* params are mutable locals */, p->struct_name, p->elem_type, p->elem_elem_type);
        if (p->type == TYPE_FN) sym_set_fn_type(&st, p->name, p->fn_type); /* M40 */
    }
    for (Stmt *s = f->body; s; s = s->next) {
        check_stmt(&st, s);
    }
}

/* Resets all semantic-analysis global/static state to its initial values,
   so semantic_check() can be called repeatedly from the same process (e.g.
   an embedding library, LSP, watch-mode or build-server) without state
   leaking between runs. All FuncSig/current_fn_* fields here are borrowed
   from the AST (never strdup'd), so there is nothing to free -- only to
   zero. Must run before any other semantic call. */
void semantic_reset(void) {
    had_error = 0;
    g_cur_line = 0;
    g_cur_file = NULL;
    semantic_loop_depth = 0;
    g_func_count = 0;
    current_fn_ret_type = TYPE_VOID;
    current_fn_ret_elem_type = TYPE_VOID;
    current_fn_ret_elem_elem_type = TYPE_VOID;
    current_fn_ret_struct_name = NULL;
    current_fn_can_fail = 0;
    current_fn_params = NULL;
    g_sem_expr_depth = 0;
}

int semantic_check(Program *prog) {
    semantic_reset();

    if (!prog->main_fn) {
        sem_error("no main function found", NULL);
        return 0;
    }

    /* Pass 1: register every function's signature so calls (including
       forward references and recursion) can be checked regardless of
       definition order. `efn` functions are registered with a provisional
       TYPE_VOID ret_type here (their real declared type is unknown until
       their body expression is typed), then patched up to their inferred
       type below, before anything can observe/call them. This keeps EFN
       calling EFN (or regular functions, in any order) working exactly like
       regular function calls. */
    for (Function *f = prog->functions; f; f = f->next) {
        if (f->line > 0) { g_cur_line = f->line; g_cur_file = f->file; }
        if (g_func_count >= MAX_FUNCS) {
            sem_error("too many functions (milestone limit)", f->name);
            continue;
        }
        if (is_list_builtin(f->name) || is_sysprog_builtin(f->name)) {
            sem_error("cannot redefine builtin function", f->name);
            continue;
        }
        /* M41: imza dolduruldu ama kayit henuz yapilmadi (g_func_count artmadi):
           ayni adli onceki tanimlarla karsilastirip overload mu, redeclaration
           mu karar veriyoruz. */
        FuncSig *fs = &g_funcs[g_func_count];
        fs->name = f->name;
        fs->base = f->name;
        fs->is_extern = f->is_extern;
        fs->ret_type = f->ret_type;
        fs->ret_elem_type = f->ret_elem_type; /* M26 */
        fs->ret_elem_elem_type = f->ret_elem_elem_type; /* M32 */
        fs->param_count = f->param_count;
        fs->is_variadic = f->is_variadic; /* M27 */
        fs->can_fail = f->can_fail; /* M43 */
        if (f->can_fail && f->ret_type == TYPE_STRUCT)
            sem_error("fallible functions cannot return a struct by value yet", f->name);
        fs->ret_struct_name = f->ret_struct_name; /* M40 */
        int i = 0;
        for (Param *p = f->params; p; p = p->next, i++) {
            fs->param_types[i] = p->type;
            fs->param_elem_types[i] = p->elem_type; /* M26 */
            fs->param_elem_elem_types[i] = p->elem_elem_type; /* M32 */
            fs->param_struct_names[i] = p->struct_name; /* M40 */
            fs->param_fn_types[i] = p->fn_type;         /* M40 */
        }
        {
            int existing = overload_count(f->name);
            if (existing > 0) {
                int clash = f->is_extern;
                for (int k = 0; k < g_func_count && !clash; k++) {
                    if (strcmp(g_funcs[k].base, f->name) != 0) continue;
                    if (g_funcs[k].is_extern || sig_params_equal(&g_funcs[k], fs)) clash = 1;
                }
                if (clash) {
                    sem_error("redeclaration of function", f->name);
                    continue;
                }
                char mangled[300];
                snprintf(mangled, sizeof(mangled), "%s__ov%d", f->name, existing);
                f->name = strdup(mangled);
                fs->name = f->name;
            }
            g_func_count++;
        }
        /* M28: extern functions have no body to type-check in Pass 2 (that
           skip still applies below), but ARE now registered in the
           first-class-function registry same as user fn/efn — a bare
           reference to an extern's name (not immediately called) resolves
           as a TYPE_FN value and can be assigned/passed/called indirectly,
           same as any other function. Variadic externs (e.g. printf) can
           still be referenced this way, but an indirect call can only ever
           pass exactly the declared fixed parameters: TYPE_FN carries no
           signature payload once assigned to a variable, so there is no
           way for an indirect call site to also supply the `...` marker
           codegen needs for extra varargs (see EXPR_CALL_INDIRECT in
           codegen.c) — direct `printf(...)` calls are unaffected. */
        fnsig_register(f->name, f->ret_type, f->params, f->param_count);
        {
            FnSig *reg = fnsig_find(f->name);
            if (reg) {
                reg->ret_elem_type = f->ret_elem_type; /* M26 */
                reg->ret_elem_elem_type = f->ret_elem_elem_type; /* M32 */
            }
        }
        if (f->is_extern) continue;
    }

    /* Pass 1b: infer return types for `efn` functions now that every
       function's signature (including other EFNs) is visible, so an EFN's
       body expression can freely call other EFNs/functions regardless of
       source order. Patch both the AST node and its just-registered
       signature so pass 2's `return` check and any earlier/later callers
       agree on the real inferred type. */
    for (Function *f = prog->functions; f; f = f->next) {
        if (!f->is_efn) continue;
        SymTable st;
        st.count = 0;
        var_struct_map_clear(); /* M36 */
        for (Param *p = f->params; p; p = p->next) {
            sym_add(&st, p->name, p->type, 0, p->struct_name, p->elem_type, p->elem_elem_type);
            if (p->type == TYPE_FN) sym_set_fn_type(&st, p->name, p->fn_type); /* M40 */
        }
        TypeKind inferred = check_expr(&st, f->body->expr);
        f->ret_type = inferred;
        /* Pointer-returning efn (e.g. `efn cp(d: int*, s: int*, n: int) =>
           memcpy(d, s, n)`): inferring only the TypeKind left ret_elem_type
           at its default (VOID), so pass 2's `return` check then compared
           the body's real pointee (int) against VOID and rejected a
           perfectly well-typed function ("pointer return type mismatch").
           Recover the pointee levels from the body expression — the same
           helpers the return-statement check itself uses — and publish
           them everywhere the declared-return-type equivalents live (AST
           node, call-site signature table, first-class fn registry). */
        TypeKind inferred_elem = TYPE_VOID, inferred_elem_elem = TYPE_VOID;
        if (inferred == TYPE_PTR) {
            inferred_elem = expr_elem_type_of(&st, f->body->expr);
            if (inferred_elem == TYPE_PTR)
                inferred_elem_elem = expr_elem_elem_type_of(&st, f->body->expr);
        }
        f->ret_elem_type = inferred_elem;
        f->ret_elem_elem_type = inferred_elem_elem;
        FuncSig *fs = func_find(f->name);
        if (fs) {
            fs->ret_type = inferred;
            fs->ret_elem_type = inferred_elem;
            fs->ret_elem_elem_type = inferred_elem_elem;
        }
        FnSig *fnsig = fnsig_find(f->name);
        if (fnsig) {
            fnsig->ret_type = inferred;
            fnsig->ret_elem_type = inferred_elem;
            fnsig->ret_elem_elem_type = inferred_elem_elem;
        }
    }
    if (g_func_count > 0 && strcmp(g_funcs[0].name, "main") != 0) {
        /* not an error by itself; main can be defined anywhere */
    }
    {
        FuncSig *ms = func_find("main");
        if (ms && ms->can_fail) {
            if (prog->main_fn && prog->main_fn->line > 0) { g_cur_line = prog->main_fn->line; g_cur_file = prog->main_fn->file; }
            sem_error("main cannot be declared fallible (`: !T`); handle errors with catch", NULL);
            g_cur_line = 0; g_cur_file = NULL;
        }
    }
    FuncSig *main_sig = func_find("main");
    if (main_sig && (main_sig->ret_type != TYPE_VOID || main_sig->param_count != 0)) {
        if (prog->main_fn && prog->main_fn->line > 0) { g_cur_line = prog->main_fn->line; g_cur_file = prog->main_fn->file; }
        sem_error("main must take no parameters and return void", NULL);
        g_cur_line = 0; g_cur_file = NULL;
    }

    /* Pass 2: check each function body. M27: extern functions have no body
       (they're a foreign declaration) — nothing to check. */
    for (Function *f = prog->functions; f; f = f->next) {
        if (f->is_extern) continue;
        check_function_body(f);
    }

    return !had_error;
}
