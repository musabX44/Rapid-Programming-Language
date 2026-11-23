/* rapidc runtime support: tiny helpers that are simplest to implement in C
   and link into every compiled program, rather than emitting inline QBE IL
   for them (M5: string concatenation via '+'). */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#ifndef _WIN32
#  include <unistd.h>
#  include <sys/syscall.h>
#endif

char *rapidc_strcat(const char *a, const char *b) {
    /* M37: `null` string, birlestirmede bos string gibi davranir (eskiden
       strlen(NULL) ile cokerdi). Sonuc her zaman yeni heap string'idir. */
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    r[la + lb] = '\0';
    return r;
}

/* M37: io::out(<string>) — tek argumanli string yazdirma. Eskiden dogrudan
   printf("%s\n", s) cagriliyordu; NULL icin "(null)" basmak glibc'ye ozgu,
   standart-disi bir davranistir (musl'da cokerdi). Artik acik ve tasinabilir. */
void rapidc_out_str(const char *s) {
    printf("%s\n", s ? s : "(null)");
}

/* Lexicographic string comparison for `<`/`<=`/`>`/`>=` on strings — thin
   wrapper over libc strcmp (returns <0/0/>0, same convention codegen's
   comparison-against-0 lowering expects). M36'dan beri `==`/`!=` de bunu
   kullanir (icerik karsilastirmasi). */
long rapidc_strcmp(const char *a, const char *b) {
    /* M37: NULL-guvenli. `null` her string'den KUCUK sayilir (bos string ""
       dahil): null < "" < "a". Iki null birbirine ESITtir. Boylece `==`/`!=`
       ve `<`/`>` runtime'da NULL olan bir string ile cokmez. */
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return (long)strcmp(a, b);
}

/* M9: dynamic int lists.
   A list value is a pointer to a header:
     [0]  int64 len   (element count)
     [8]  int64 cap   (allocated element capacity)
     [16] int64 data[cap]
   `data` is always contiguous right after the header, so codegen can get the
   element address for `xs[i]` as `hdr + 16 + i*8` without another indirection. */
#define RAPIDC_LIST_HEADER_BYTES 16

static long *rapidc_list_alloc(long cap) {
    long *hdr = (long *)malloc(RAPIDC_LIST_HEADER_BYTES + (cap > 0 ? cap : 1) * 8);
    hdr[0] = 0;   /* len */
    hdr[1] = cap; /* cap */
    return hdr;
}

/* Builds a new list from `n` elements already laid out contiguously at
   `elems` (codegen fills a scratch stack block for an array literal, then
   calls this once to turn it into a proper heap-backed list). */
long *rapidc_list_new(long *elems, long n) {
    long *hdr = rapidc_list_alloc(n);
    hdr[0] = n;
    if (n > 0) memcpy((char *)hdr + RAPIDC_LIST_HEADER_BYTES, elems, n * 8);
    return hdr;
}

/* Appends `val`, growing (doubling) if needed. Returns the (possibly new)
   header pointer — callers must reassign their list variable to this. */
long *rapidc_list_push(long *hdr, long val) {
    long len = hdr[0], cap = hdr[1];
    if (len >= cap) {
        long newcap = cap > 0 ? cap * 2 : 4;
        long *bigger = (long *)malloc(RAPIDC_LIST_HEADER_BYTES + newcap * 8);
        bigger[0] = len;
        bigger[1] = newcap;
        if (len > 0) memcpy((char *)bigger + RAPIDC_LIST_HEADER_BYTES,
                             (char *)hdr + RAPIDC_LIST_HEADER_BYTES, len * 8);
        free(hdr);
        hdr = bigger;
    }
    ((long *)((char *)hdr + RAPIDC_LIST_HEADER_BYTES))[len] = val;
    hdr[0] = len + 1;
    return hdr;
}

/* Deep copy: a fresh, independently-growable list with the same elements. */
long *rapidc_list_copy(long *hdr) {
    long len = hdr[0];
    long *out = rapidc_list_alloc(len);
    out[0] = len;
    if (len > 0) memcpy((char *)out + RAPIDC_LIST_HEADER_BYTES,
                         (char *)hdr + RAPIDC_LIST_HEADER_BYTES, len * 8);
    return out;
}

long rapidc_list_len(long *hdr) {
    return hdr[0];
}

/* Production-hardening fix: runtime bounds check for list<int>/list<byte>
   indexing (read or write). Previously `xs[i]` compiled straight to
   `header + 16 + i*stride` with no check at all, so an out-of-range or
   negative `i` was silent heap corruption (or a hard-to-diagnose segfault
   far from the actual bug) instead of a clean, deterministic failure.
   gen_elem_addr's callers now emit a call to this before every list
   element address computation. Deliberately NOT applied to raw T* pointer
   indexing or `string[i]`, which (like C) carry no length at the pointee
   and have no header to check against — those remain documented, unchecked
   pointer arithmetic (see RAPID-MANUAL.md "Bilinen sinirlar"). */
void rapidc_bounds_check(long idx, long len, const char *ctx) {
    if (idx < 0 || idx >= len) {
        fprintf(stderr, "rapidc: runtime error: index %ld out of bounds (len=%ld) in %s\n",
                idx, len, ctx ? ctx : "index expression");
        _exit(1);
    }
}

/* M20: dynamic byte lists (list<byte>). Same {len, cap, data} header shape
   as the int-list above, so codegen can reuse the same 16-byte header
   offset for both — but `data` here is a packed array of single bytes
   (1-byte stride) rather than 8-byte longs, since that's what makes this a
   real byte[] instead of an int list holding small ints. */
static char *rapidc_blist_alloc(long cap) {
    long *hdr = (long *)malloc(RAPIDC_LIST_HEADER_BYTES + (cap > 0 ? cap : 1));
    hdr[0] = 0;   /* len */
    hdr[1] = cap; /* cap */
    return (char *)hdr;
}

/* Builds a new byte list from `n` bytes already laid out contiguously at
   `elems` (mirrors rapidc_list_new for ints). */
char *rapidc_blist_new(char *elems, long n) {
    char *hdr = rapidc_blist_alloc(n);
    ((long *)hdr)[0] = n;
    if (n > 0) memcpy(hdr + RAPIDC_LIST_HEADER_BYTES, elems, n);
    return hdr;
}

/* Appends `val` (only the low 8 bits are stored), growing (doubling) if
   needed. Returns the (possibly new) header pointer, same push/reassign
   contract as rapidc_list_push. */
char *rapidc_blist_push(char *hdr, long val) {
    long len = ((long *)hdr)[0], cap = ((long *)hdr)[1];
    if (len >= cap) {
        long newcap = cap > 0 ? cap * 2 : 4;
        char *bigger = (char *)malloc(RAPIDC_LIST_HEADER_BYTES + newcap);
        ((long *)bigger)[0] = len;
        ((long *)bigger)[1] = newcap;
        if (len > 0) memcpy(bigger + RAPIDC_LIST_HEADER_BYTES,
                             hdr + RAPIDC_LIST_HEADER_BYTES, len);
        free(hdr);
        hdr = bigger;
    }
    (hdr + RAPIDC_LIST_HEADER_BYTES)[len] = (char)val;
    ((long *)hdr)[0] = len + 1;
    return hdr;
}

/* Deep copy: a fresh, independently-growable byte list with the same
   elements (mirrors rapidc_list_copy). */
char *rapidc_blist_copy(char *hdr) {
    long len = ((long *)hdr)[0];
    char *out = rapidc_blist_alloc(len);
    ((long *)out)[0] = len;
    if (len > 0) memcpy(out + RAPIDC_LIST_HEADER_BYTES,
                         hdr + RAPIDC_LIST_HEADER_BYTES, len);
    return out;
}

long rapidc_blist_len(char *hdr) {
    return ((long *)hdr)[0];
}

/* M12: sysprog:: module - raw system-programming primitives.
   alloc/free are thin wrappers over libc malloc/free (Rapid!'s `int*` is
   reused as a generic untyped pointer here, same as EXPR_ADDR_OF/DEREF's
   existing pointer type). `size` is a byte count, exactly like C's malloc. */
long *rapidc_alloc(long size) {
    return (long *)malloc((size_t)size);
}

void rapidc_free(long *ptr) {
    free(ptr);
}

/* M18: io:: file syscall wrappers - thin libc wrappers (not raw syscall(2),
   unlike sysprog::syscall) so errno-style negative return values and the
   usual open() flag/mode conventions behave exactly like they do in C.
   Rapid! has no varargs, so open() always takes a mode argument; it's
   simply ignored by the OS when O_CREAT isn't set. buf is a byte pointer
   (raw pointer, same representation as int-ptr/byte-ptr elsewhere),
   count/nbytes are in bytes, matching read(2)/write(2). */
#include <fcntl.h>
#include <errno.h>
#ifdef _WIN32
#  include <io.h>      /* _open, _read, _write, _close */
#  define open(p,f,m) _open((p),(f),(m))
#  define read(f,b,n) _read((f),(b),(unsigned int)(n))
#  define write(f,b,n) _write((f),(b),(unsigned int)(n))
#  define close(f)    _close(f)
#endif

/* M19: last errno observed from an io:: syscall wrapper. */
static long g_rapidc_errno = 0;

long rapidc_open(const char *path, long flags, long mode) {
    long r = open(path, (int)flags, (int)mode);
    g_rapidc_errno = errno;
    return r;
}

long rapidc_read(long fd, void *buf, long count) {
    long r = read((int)fd, buf, (size_t)count);
    g_rapidc_errno = errno;
    return r;
}

long rapidc_write(long fd, const void *buf, long count) {
    long r = write((int)fd, buf, (size_t)count);
    g_rapidc_errno = errno;
    return r;
}

long rapidc_close(long fd) {
    long r = close((int)fd);
    g_rapidc_errno = errno;
    return r;
}

/* M19: io::errno() - returns the errno value saved by the most recent
   io::open/read/write/close call (0 if that call succeeded). Only reflects
   io::'s own syscalls, not sysprog::syscall or anything else. */
long rapidc_errno(void) {
    return g_rapidc_errno;
}

/* Raw 6-argument Linux syscall, no libc wrapper in between (unlike e.g.
   read()/write() which validate/translate errno) - callers get back
   exactly what the kernel returned, including negative -errno values on
   failure, same as writing the `syscall` instruction directly would give. */
long rapidc_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
#ifdef _WIN32
    (void)nr; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    fprintf(stderr, "rapidc: sysprog::syscall is not supported on Windows\n");
    return -1;
#else
    return syscall((long)nr, a1, a2, a3, a4, a5, a6);
#endif
}

/* M16: io::in() - reads a single line from stdin and returns it as a fresh
   heap string (rapidc strings are just malloc'd char*, same as literals and
   rapidc_strcat's result, so no special-casing is needed elsewhere). The
   trailing '\n' (if any) is stripped, matching how io::out already appends
   its own '\n' on the way out. On EOF with nothing read, returns "". */
/* M16: io::format(fmt, args...) - prints `fmt` to stdout, replacing each
   "{}" placeholder in order with the corresponding argument, then a
   trailing '\n' (same convention as io::out). Each argument is tagged with
   a one-character kind code so this single varargs helper can print ints,
   bytes, and strings without the caller needing per-type overloads:
     'i' -> long          (printed as %ld)
     'c' -> long (0..255) (printed as %c)
     's' -> char*         (printed as %s)
   Codegen packs kinds/values into two parallel arrays and calls this once
   per io::format(...) call site. Unmatched "{}" beyond argc, or extra
   trailing args, are left/ignored respectively rather than erroring, to
   keep the runtime simple. */
void rapidc_format(const char *fmt, const char *kinds, long *vals, long argc) {
    long ai = 0;
    for (const char *p = fmt; *p; p++) {
        if (p[0] == '{' && p[1] == '}' && ai < argc) {
            switch (kinds[ai]) {
                case 's': {
                    /* M37: null string "(null)" basar (tek-arguman io::out(s)
                       yolu printf("%s") kullanir ve glibc de NULL icin ayni
                       metni basar; iki yol tutarli kalsin). */
                    const char *sv = (const char *)(intptr_t)vals[ai];
                    fputs(sv ? sv : "(null)", stdout);
                    break;
                }
                case 'c':
                    fputc((int)vals[ai], stdout);
                    break;
                default:
                    printf("%ld", vals[ai]);
                    break;
            }
            ai++;
            p++; /* skip the '}' too */
        } else {
            fputc(*p, stdout);
        }
    }
    fputc('\n', stdout);
}

/* M20: toInt(str) / toString(int) - string<->int conversion builtins.
   Wrap libc atol/sprintf so no other part of the language needs to reason
   about number<->text conversion by hand (e.g. after io::in()). */
long rapidc_str_to_int(const char *s) {
    if (!s) return 0; /* M37: null string -> 0 */
    return atol(s);
}

char *rapidc_int_to_str(long n) {
    /* 64-bit long: sign + up to 19 digits + NUL fits comfortably in 24. */
    char *buf = malloc(24);
    snprintf(buf, 24, "%ld", n);
    return buf;
}

/* M40: toString(float/double). %.17g gidis-donus icin yeterli; ama 2.5 gibi
   degerlerin "2.5" cikmasi icin once %.15g denenir, geri okununca ayni
   degilse %.17g'ye cikilir. */
char *rapidc_double_to_str(double d) {
    char *buf = malloc(32);
    snprintf(buf, 32, "%.15g", d);
    if (strtod(buf, NULL) != d) snprintf(buf, 32, "%.17g", d);
    return buf;
}

/* M20: len(string) - byte length of a string, for the same reason
   len(list)/len(array) exist: lets code iterate `s[i]` for i in
   0..len(s) instead of guessing a size. Thin wrapper over libc strlen. */
long rapidc_strlen(const char *s) {
    if (!s) return 0; /* M37: len(null string) == 0 */
    return (long)strlen(s);
}

/* Integer power (a**b). QBE has no exponentiation instruction, so `**`
   lowers to a call here for the int case (float case below uses libm's
   pow, which already handles fractional/negative exponents correctly —
   repeated multiplication does not). Negative b returns 0 (integer power
   with a negative exponent has no integer result); b == 0 is 1 for any a,
   matching normal exponentiation convention. */
long rapidc_ipow(long a, long b) {
    if (b < 0) return 0;
    long result = 1;
    while (b > 0) {
        if (b & 1) result *= a;
        a *= a;
        b >>= 1;
    }
    return result;
}

double rapidc_fpow(double a, double b) {
    return pow(a, b);
}

float rapidc_fpow_s(float a, float b) {
    return powf(a, b);
}

char *rapidc_readline(void) {
    size_t cap = 128;
    size_t len = 0;
    char *buf = malloc(cap);
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return buf;
}

/* M41: std/std.rapid'in kullandigi sayisal donusum yardimcilari. Eskiden bunlar
   codegen'de QBE komutlariydi (dtosi/stosi/sltof/ultof/exts/truncd); artik
   toInt/toFloat/toDouble tipe gore secilen normal Rapid! fonksiyonlari ve
   gercek isi bu kucuk C fonksiyonlari yapiyor. Aralik disi / NaN -> LONG_MIN,
   yani eski dtosi'nin (x86 cvttsd2si) davranisi korunur. */
#include <limits.h>
long rapidc_d_to_int(double d) {
    if (d != d || d >= 9223372036854775808.0 || d < -9223372036854775808.0) return LONG_MIN;
    return (long)d;
}
long rapidc_f_to_int(float f) { return rapidc_d_to_int((double)f); }
double rapidc_int_to_d(long n) { return (double)n; }
float rapidc_int_to_f(long n) { return (float)n; }
double rapidc_uint_to_d(unsigned long n) { return (double)n; }
float rapidc_uint_to_f(unsigned long n) { return (float)n; }
double rapidc_f_to_d(float f) { return (double)f; }
float rapidc_d_to_f(double d) { return (float)d; }

/* ---- M43: hata yonetimi ------------------------------------------------
   Hata donebilen (`: !T`) fonksiyonlar normal degerlerini normal sekilde
   dondurur; hata durumunda rapid_err'i sifirdan farkli bir kodla set edip
   bos deger dondurur. Cagiran taraf `try` (yukari yay) veya `catch`
   (temizle + varsayilan) ile bunu denetler; derleyici bu ikisi disinda
   hata donebilen bir cagriya izin vermez, yani rapid_err normal kodda
   her zaman 0'dir. Tek thread'li dil icin global yeterlidir. */
long rapid_err = 0;
long rapid_last = 0;
/* Tabloyu codegen uretir; weak: tabloyu uretmeyen eski bir derleyiciyle de baglanabilsin. */
extern long rapid_errcount __attribute__((weak));
extern const char *rapid_errnames[] __attribute__((weak));

void rapidc_fail(long code) { rapid_err = code ? code : 1; /* 0 = basari: Unknown'a (1) cevir */ }
long rapidc_lasterr(void) { return rapid_last; }
const char *rapidc_errname(long code) {
    if (!&rapid_errcount || !rapid_errnames || code < 1 || code > rapid_errcount) return "Unknown";
    return rapid_errnames[code];
}
