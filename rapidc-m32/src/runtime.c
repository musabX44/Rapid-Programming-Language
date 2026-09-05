/* rapidc runtime support: tiny helpers that are simplest to implement in C
   and link into every compiled program, rather than emitting inline QBE IL
   for them (M5: string concatenation via '+'). */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

char *rapidc_strcat(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    r[la + lb] = '\0';
    return r;
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

/* M19: last errno observed from an io:: syscall wrapper. Saved right after
   the underlying libc call, before anything else can clobber `errno`, so
   io::errno() can be called afterwards to inspect *why* the previous
   io::open/read/write/close failed (their own return value only says
   whether it failed, same as raw POSIX). Not thread-safe, but neither is
   anything else in this single-threaded runtime. */
static long g_rapidc_errno = 0;

long rapidc_open(const char *path, long flags, long mode) {
    long r = open(path, (int)flags, (mode_t)mode);
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
    return syscall((long)nr, a1, a2, a3, a4, a5, a6);
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
                case 's':
                    fputs((const char *)(long)vals[ai], stdout);
                    break;
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
    return atol(s);
}

char *rapidc_int_to_str(long n) {
    /* 64-bit long: sign + up to 19 digits + NUL fits comfortably in 24. */
    char *buf = malloc(24);
    snprintf(buf, 24, "%ld", n);
    return buf;
}

/* M20: len(string) - byte length of a string, for the same reason
   len(list)/len(array) exist: lets code iterate `s[i]` for i in
   0..len(s) instead of guessing a size. Thin wrapper over libc strlen. */
long rapidc_strlen(const char *s) {
    return (long)strlen(s);
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
