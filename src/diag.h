/* diag.h — rapidc icin Rust-tarzi ayrintili tani (diagnostic) yazdirici.
   ---------------------------------------------------------------------
   TEK DOSYALIK, header-only bir yardimci: hicbir .c dosyasina eklenmez,
   sadece ihtiyaç duyan yerde #include "diag.h" ile dahil edilir ve
   compiler zaten var olan `file:line` + mesaj bilgisinden, Rust/clang
   tarzi bir hata bloğu üretir:

       test.rapid:4:9
         |
       4 |     var x: int = y;
         |         ^ undefined identifier: y
         |
         = help: 'y' tanimli degil; bir 'var'/'const' ile bildirildi mi
                 kontrol edin, yoksa yazim hatasi olabilir

   Neden bu şekilde: rapidc'nin TÜM semantic hatalari zaten tek bir
   fonksiyondan (semantic.c: sem_error) ve tüm parse hatalari zaten tek
   bir fonksiyondan (parser.y: yyerror) geçiyor. Bu iki fonksiyonun
   gövdesini bu header'daki tek bir çağrıya yönlendirmek, derleyicinin
   geri kalanina (lexer, parser grameri, semantic kurallari, codegen)
   HİÇ dokunmadan tüm hata çıktısını zenginleştirir.

   Kaynak dosyayı ikinci kez (hata anında) açıp ilgili satırı okur --
   derleyicinin normal akışını etkilemez, sadece tani yazdirilirken bir
   kez calisir. Dosya okunamazsa (ör. uretilmis/gecici kaynak) sessizce
   eski "duz" formata geriler.

   Sütun numarası: rapidc'nin lexer'i şu an sütun izlemiyor (sadece
   `yylineno`), bu yüzden col=0 verilebilir; bu durumda işaretçi (^)
   satırın başına değil, ilk boşluk-olmayan karaktere konur (en azından
   bir şeyi işaret eder, "burada bir yerde" yerine "muhtemelen burada").
   Bir çağıran gerçek bir sütun biliyorsa onu verebilir (1-indexed).
*/
#ifndef RAPIDC_DIAG_H
#define RAPIDC_DIAG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

/* Terminal renkli çıktıyı destekliyor mu? stderr bir TTY ise ve
   NO_COLOR ortam değişkeni set değilse renk kullanılır (bkz.
   https://no-color.org) -- bir CI/log dosyasına yönlendirilen çıktı
   kaçış kodlarıyla kirlenmez. */
static int diag__use_color(void) {
    static int cached = -1;
    if (cached == -1) {
        cached = isatty(fileno(stderr)) && !getenv("NO_COLOR");
    }
    return cached;
}

#define DIAG_C_RESET  (diag__use_color() ? "\x1b[0m"  : "")
#define DIAG_C_BOLD   (diag__use_color() ? "\x1b[1m"  : "")
#define DIAG_C_RED    (diag__use_color() ? "\x1b[1;31m" : "")
#define DIAG_C_BLUE   (diag__use_color() ? "\x1b[1;34m" : "")
#define DIAG_C_YELLOW (diag__use_color() ? "\x1b[1;33m" : "")

/* Kaynak dosyadan `line_no` numarali satiri okur (1-indexed). Bulunursa
   1, bulunamazsa 0 döner; `out`a en fazla outsz-1 karakter yazar, sondaki
   '\n'i kirpar.

   NOT: fgets() bir satiri BUFF boyutu sinirinda, '\n' gormeden de
   kesebilir. Bunu "bir sonraki satira gectik" sanip 'cur'u erken
   artirmak, buffer'dan uzun herhangi bir satirdan SONRAKI tum satir
   numaralarini kaydirirdi -- sessiz, takibi zor bir hata. Bu yuzden her
   fgets parcasi gercek '\n'i gorene (ya da dosya bitene) kadar 'cur'
   artirilmaz; hedef satir buffer'dan uzunsa, cikan metin gorunur bir
   "..." ile isaretlenerek kirpilir (sessiz kirpma yerine). */
static int diag__read_line(const char *path, int line_no, char *out, size_t outsz) {
    if (!path || line_no <= 0) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096];
    int cur = 1;
    int found = 0;
    int out_len = 0;       /* kac karakter out'a yazildi, hedef satirdayken */
    int out_truncated = 0; /* hedef satir buffer sinirini asti mi */
    for (;;) {
        if (!fgets(buf, sizeof(buf), f)) break;
        size_t blen = strlen(buf);
        int chunk_has_newline = (blen > 0 && buf[blen - 1] == '\n');
        if (chunk_has_newline) {
            blen--;
            if (blen > 0 && buf[blen - 1] == '\r') blen--;
        }

        if (cur == line_no) {
            found = 1;
            if ((size_t)out_len < outsz - 1) {
                size_t room = outsz - 1 - (size_t)out_len;
                size_t take = (blen < room) ? blen : room;
                memcpy(out + out_len, buf, take);
                out_len += (int)take;
                if (take < blen) out_truncated = 1;
            } else {
                out_truncated = 1;
            }
            out[out_len] = '\0';
            if (chunk_has_newline) break; /* satirin gercek sonuna ulasildi */
            /* Bu fgets parcasi satiri bitirmedi (buffer sinirina
               takildi) -- ayni satirin devamini okumaya devam et,
               'cur'u henuz artirma. */
            continue;
        }

        if (chunk_has_newline) cur++;
        /* chunk_has_newline false ve cur != line_no ise: ilgilenmedigimiz
           cok uzun bir satirin ortasindayiz, sadece atlamaya devam. */
    }
    fclose(f);

    if (found && out_truncated) {
        /* Sessiz kirpma yerine gorunur bir isaret: kullanici satirin tam
           olmadigini anlasin. "..." icin yer ac (gerekirse mevcut sonu
           biraz geri al), outsz en az birkac karakterse sigdirilabilir;
           asiri kucuk bir outsz (pratikte olmaz, 4096 cagriliyor) icin
           bile tasma yapmaz. */
        const char *mark = "...";
        size_t marklen = strlen(mark);
        size_t cap = outsz - 1;
        size_t keep = (out_len >= 0 && (size_t)out_len + marklen <= cap)
                          ? (size_t)out_len
                          : (cap > marklen ? cap - marklen : 0);
        memcpy(out + keep, mark, (marklen <= cap - keep) ? marklen : (cap - keep));
        out[keep + ((marklen <= cap - keep) ? marklen : (cap - keep))] = '\0';
    }

    return found;
}

/* İlk boşluk-olmayan karakterin 0-indexed konumu (gerçek sütun yoksa bu,
   işaretçiyi en azından "kod başladığı yere" koyar, satırın en başına
   değil). Satır tamamen boşsa 0 döner. */
static int diag__first_nonspace(const char *s) {
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    return s[i] ? i : 0;
}

/* `needle` (ör. hatali tanimlayicinin adi) satirda BUTUN BIR KELIME olarak
   geciyorsa (onceki/sonraki karakter bir tanimlayici karakteri degilse)
   0-indexed konumunu dondurur; yoksa -1. Gercek sutun takibi olmadan
   bile isaretciyi "satirin basi" yerine dogrudan suclu ada koymak icin --
   rapidc'nin lexer'i sutun tutmadigindan bu, elde edilebilecek en iyi
   yaklasimdir (birden fazla gecis varsa ilkini isaretler). */
static int diag__find_word(const char *line, const char *needle) {
    if (!needle || !*needle) return -1;
    size_t nlen = strlen(needle);
    const char *p = line;
    while ((p = strstr(p, needle)) != NULL) {
        int before_ok = (p == line) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        char after = p[nlen];
        int after_ok = !(isalnum((unsigned char)after) || after == '_');
        if (before_ok && after_ok) return (int)(p - line);
        p += 1;
    }
    return -1;
}

/* Ana tani yazdirici. `kind`: "error"/"warning" gibi kisa bir etiket.
   `file`/`line`: kaynak konum (line<=0 ise konum bilinmiyor demektir).
   `col`: 1-indexed sütun, bilinmiyorsa 0 (o zaman `focus_word` satirda
   aranir, o da yoksa ilk-bosluk-olmayan karakter kullanilir). `headline`:
   hatanin kisa özeti (sem_error'daki `msg` + `name` birlestirilmis hali
   gibi). `focus_word`: NULL olabilir -- biliniyorsa, isaretcinin satirda
   tam olarak hangi kelimeyi gostermesi gerektigi (ör. suclu tanimlayici
   adi); col=0 iken kullanilir. `help`: NULL olabilir, tek satırlık bir
   öneri/ipucu (Rust'taki `= help: ...` satırı gibi). */
static void diag_emit(const char *kind, const char *file, int line, int col,
                       const char *headline, const char *focus_word, const char *help) {
    FILE *e = stderr;
    const char *kind_color = (strstr(kind, "error") != NULL) ? DIAG_C_RED : DIAG_C_YELLOW;

    /* Baslik satiri. Eski (M42'den beri) "file:line: semantic error: msg"
       / "file:line: parse error: msg" formatiyla BIREBIR AYNI metni ayni
       satirda tutar -- run_tests.sh ve m42tests/m43tests gibi testler bu
       tam dizgiyi grep'ler, bu yuzden kelimesi kelimesine korunuyor.
       Rust-tarzi `-->` + cerceveli kaynak satiri + help, bundan SONRAKI
       satirlarda EK bilgi olarak gelir; hicbir eski tani tuketen script
       kirilmaz, sadece daha fazla bilgi eklenir. */
    if (line > 0)
        fprintf(e, "%s%s%s%s:%d: %s: %s%s\n",
                DIAG_C_BOLD, kind_color, file ? file : "<input>", DIAG_C_RESET,
                line, kind, DIAG_C_BOLD, headline);
    else
        fprintf(e, "%s%s%s: %s%s\n", DIAG_C_BOLD, kind_color, kind, DIAG_C_BOLD, headline);
    fprintf(e, "%s", DIAG_C_RESET);

    if (line <= 0) {
        /* Konum bilgisi yok (ör. dosya geneli bir hata) -- eski "duz"
           davranışa dön, çerçeve çizmenin anlamı yok. */
        if (help) fprintf(e, "  %s=%s help: %s\n", DIAG_C_BLUE, DIAG_C_RESET, help);
        return;
    }

    char line_text[4096];
    int have_line = diag__read_line(file, line, line_text, sizeof(line_text));

    char loc[600];
    if (col > 0) snprintf(loc, sizeof(loc), "%s:%d:%d", file ? file : "<input>", line, col);
    else         snprintf(loc, sizeof(loc), "%s:%d", file ? file : "<input>", line);
    fprintf(e, " %s-->%s %s\n", DIAG_C_BLUE, DIAG_C_RESET, loc);

    if (!have_line) {
        /* Kaynak dosya artik okunamiyor (silinmis, stdin'den gelmis,
           vs) -- en azindan dosya:satir bilgisini verdik, çerçeve yok. */
        if (help) fprintf(e, "  %s=%s help: %s\n", DIAG_C_BLUE, DIAG_C_RESET, help);
        return;
    }

    int caret_col;
    if (col > 0) {
        caret_col = col - 1;
    } else {
        int word_pos = diag__find_word(line_text, focus_word);
        caret_col = (word_pos >= 0) ? word_pos : diag__first_nonspace(line_text);
    }
    if (caret_col < 0) caret_col = 0;

    char linenum[16];
    snprintf(linenum, sizeof(linenum), "%d", line);
    int gutter = (int)strlen(linenum);
    if (gutter < 3) gutter = 3;

    /* " |" bosluk satiri (Rust'in ust cercevesi) */
    fprintf(e, " %*s %s|%s\n", gutter, "", DIAG_C_BLUE, DIAG_C_RESET);
    /* "N | <kaynak satiri>" */
    fprintf(e, " %s%*d%s %s|%s %s\n", DIAG_C_BOLD, gutter, line, DIAG_C_RESET,
            DIAG_C_BLUE, DIAG_C_RESET, line_text);
    /* " | <bosluklar>^ <headline>" -- isaretci + kisa aciklama ayni satirda,
       Rust'in "^^^ expected X, found Y" tarzi. */
    fprintf(e, " %*s %s|%s ", gutter, "", DIAG_C_BLUE, DIAG_C_RESET);
    for (int i = 0; i < caret_col; i++) fputc(' ', e);
    fprintf(e, "%s^%s\n", kind_color, DIAG_C_RESET);

    if (help) {
        fprintf(e, " %*s %s=%s help: %s\n", gutter, "", DIAG_C_BLUE, DIAG_C_RESET, help);
    }
    fputc('\n', e);
}

/* ---------------------------------------------------------------------
   Help-metni eşleştirme tablosu: sem_error/yyerror'a geçirilen mesaj
   metninde (alt dize olarak) bir anahtar geçiyorsa, kullanıcıya kısa bir
   "bir sonraki adım" önerisi eklenir (Rust'ın `= help: ...` satırı gibi).
   Derleyicinin ~90 farklı mesajının HER biri için ayrı ayrı yer
   değiştirmek yerine (ki bu da "tek dosyaya dokun" hedefini bozardı),
   mesajların kendisi hiç değişmeden burada tek bir yerde yakalanıyor.
   Eşleşme yoksa help NULL kalır, sadece çerçeveli konum gösterilir --
   hiçbir hata "eksik" görünmez, sadece bir kısmı ekstra ipucu alır. */
typedef struct { const char *needle; const char *help; } DiagHelp;

static const DiagHelp DIAG_HELP_TABLE[] = {
    { "undefined identifier",
      "bu isim bu kapsamda bilinmiyor; bir 'var'/'const' ile bildirdiniz mi, yoksa yazim hatasi mi kontrol edin" },
    { "undefined struct",
      "bu struct hic tanimlanmamis; adini kontrol edin veya onu tanimlayan dosyayi 'use' ile dahil edin" },
    { "undefined enum",
      "bu enum hic tanimlanmamis; adini kontrol edin veya onu tanimlayan dosyayi 'use' ile dahil edin" },
    { "call to undefined function",
      "bu fonksiyon bu kapsamda bilinmiyor; adini, parametre sayisini, veya gerekli bir 'use' direktifini kontrol edin" },
    { "redeclaration of variable",
      "ayni isim bu kapsamda zaten bildirilmis; farkli bir isim secin veya mevcut bildirimi kaldirin" },
    { "redeclaration of function",
      "ayni imzayla bir fonksiyon zaten var; overload icin parametre tiplerini degistirin" },
    { "cannot assign to const",
      "bu degisken 'const' ile bildirilmis; degistirmek icin 'var' kullanin" },
    { "cannot modify const",
      "bu degisken 'const' ile bildirilmis; degistirmek icin 'var' kullanin" },
    { "type mismatch in declaration",
      "bildirilen tip ile baslangic degerinin tipi uyusmuyor; aciklamayi degistirin veya degeri donusturun (ornek: toInt(x), toFloat(x))" },
    { "type mismatch in assignment",
      "atanan degerin tipi degiskenin tipiyle uyusmuyor; bir donusum fonksiyonu gerekebilir (toInt/toFloat/toDouble/toString)" },
    { "type mismatch in binary expression",
      "ikili islemin iki tarafi da ayni tipte (veya uyumlu sayisal tipte) olmali" },
    { "return type mismatch",
      "return edilen degerin tipi fonksiyonun bildirilen donus tipiyle uyusmuyor" },
    { "missing return value for non-void function",
      "bu fonksiyon bir deger donduruyor; 'return <deger>;' kullanin, tek basina 'return;' yetmez" },
    { "wrong number of arguments",
      "cagrida verilen argument sayisi fonksiyonun bildirimindeki parametre sayisiyla uyusmuyor" },
    { "argument type mismatch",
      "cagridaki bir argumanin tipi, fonksiyonun o parametresi icin bildirilen tiple uyusmuyor" },
    { "if condition must be bool",
      "'if' kosulu bir 'bool' ifadesi olmali; bir karsilastirma kullanin (ornek: x > 0)" },
    { "while condition must be bool",
      "'while' kosulu bir 'bool' ifadesi olmali; bir karsilastirma kullanin (ornek: x > 0)" },
    { "for condition must be bool",
      "'for' dongusunun orta kosulu bir 'bool' ifadesi olmali" },
    { "do-while condition must be bool",
      "'do-while' kosulu bir 'bool' ifadesi olmali" },
    { "break outside of loop",
      "'break' yalnizca bir 'while'/'for'/'do-while' govdesinin icinde kullanilabilir" },
    { "continue outside of loop",
      "'continue' yalnizca bir 'while'/'for'/'do-while' govdesinin icinde kullanilabilir" },
    { "no such field in struct",
      "bu alan adi struct tanimida yok; struct tanimindaki alan adlarini kontrol edin" },
    { "no such member in enum",
      "bu uye adi enum tanimida yok; enum tanimindaki uye adlarini kontrol edin" },
    { "dereference requires a pointer",
      "'*x' yalnizca x bir pointer (T*) oldugunda kullanilabilir" },
    { "address-of operator requires an addressable lvalue",
      "'&x' yalnizca x bir degiskense (bir gecici/ifade degil) uygulanabilir" },
    { "indexing requires a list, string, or pointer",
      "'xs[i]' yalnizca list<T>, string, veya bir pointer (T*) uzerinde calisir" },
    { "index must be int",
      "indeks ifadesi 'int' tipinde olmali; bir donusum gerekebilir (toInt(x))" },
    { "call to fallible function must be wrapped in try or catch",
      "': !T' donduren bir fonksiyonun sonucu asla dogrudan kullanilamaz; 'try cagri' ya da 'cagri catch varsayilan' kullanin" },
    { "try used in a function that cannot fail",
      "'try' yalnizca kendisi de ': !T' ile bildirilmis bir fonksiyonun icinde kullanilabilir" },
    { "try requires a call to a fallible function",
      "'try' yalnizca ': !T' donduren bir fonksiyon cagrisina uygulanabilir, duz bir ifadeye degil" },
    { "fail used in a function that cannot fail",
      "'fail error::X;' yalnizca kendisi ': !T' ile bildirilmis bir fonksiyonun icinde kullanilabilir" },
    { "catch requires a call to a fallible function",
      "'catch' yalnizca ': !T' donduren bir fonksiyon cagrisinin saginda kullanilabilir" },
    { "too many variables",
      "bu fonksiyondaki yerel degisken sayisi derleyicinin milestone siniri olan 64'u asiyor; fonksiyonu kucuk parcalara bolun" },
    { "too many functions",
      "programdaki toplam fonksiyon sayisi derleyicinin MAX_FUNCS sinirini asiyor; src/semantic.c ve src/codegen.c'deki MAX_FUNCS'u (ikisi de AYNI olmali) yukseltip yeniden derleyin" },
    { "expression nested too deeply",
      "ifade MAX_EXPR_DEPTH (400) seviyeden fazla ic ice gecmis; ifadeyi ara degiskenlere bolerek sadelestirin" },
    { "no main function found",
      "her programda parametresiz, void donuslu tam olarak bir 'fn main()' olmalidir" },
    { "main must take no parameters",
      "'main' parametre almaz ve 'void' doner: 'fn main() { ... }'" },
    { "main cannot be declared fallible",
      "'main' ': !T' ile bildirilemez; olasi hatalari fonksiyon icinde 'catch' ile sonlandirin" },
    /* NOT: "shim \"..." ve "invalid library name" mesajlari BILEREK
       burada DEGIL -- onlar module.c/main.c'nin duz fprintf() cikisi,
       sem_error()/yyerror() uzerinden hic gecmiyor, yani diag_find_help()
       onlari asla goremez; buraya eklenseler olu veri olurlardi (nitekim
       bir zamanlar buradaydilar, diag_selfcheck_help_table() ile bu fark
       edildi). O iki hata zaten kendi fprintf'lerinde yeterince acik;
       Rust-tarzi cerceveye tasimak istenirse module.c/main.c'nin de
       diag_emit() cagirmasi gerekir -- bu, bu tablonun degil, o iki
       dosyanin degisikligidir. */
    { NULL, NULL }
};

/* `msg` (ve varsa `name`) icinde DIAG_HELP_TABLE'daki anahtarlardan birini
   arar, ilk eslesmenin help metnini dondurur; hic eslesme yoksa NULL. */
static const char *diag_find_help(const char *msg) {
    if (!msg) return NULL;
    for (int i = 0; DIAG_HELP_TABLE[i].needle; i++) {
        if (strstr(msg, DIAG_HELP_TABLE[i].needle)) {
            const char *h = DIAG_HELP_TABLE[i].help;
            return (h && *h) ? h : NULL;
        }
    }
    return NULL;
}

/* DIAG_HELP_TABLE eslestirmesi kirilgandir: needle'lar semantic.c'deki
   sem_error() cagrilarinin ILK ARGUMANIYLA (hata mesaji metniyle) birebir
   alt-dize olarak eslesmesi GEREKIR, ama derleyici bunu denetlemez --
   semantic.c'de bir mesaj metni yeniden yazilirsa, bu tablodaki karsilik
   gelen needle sessizce artik hic eslesmez ve o hata bundan boyle help
   satirsiz kalir; ne derleme hatasi ne de calisma-zamani uyarisi olur.

   Bu, gelistirici o deginmeyi fark etsin diye opsiyonel bir runtime
   self-check: RAPIDC_DIAG_DEBUG ortam degiskeni set edildiginde process
   basinda cagrilip, her needle'in semantic.c kaynagi icinde gercekten
   gecip gecmedigini dogrular; kopan varsa stderr'e listeler. Normal bir
   kullanici calistirmasi bunu hic cagirmaz (main() sadece env degiskeni
   setse cagirir), yani normal kullanimda sifir maliyet. semantic.c
   kaynagi bulunamazsa (kurulmus bir binary, kaynaksiz) sessizce atlar --
   bu bir hata degil, sadece kontrolun yapilamadigi anlamina gelir. */
/* diag.h header-only oldugu icin parser.y/parser.tab.c'ye de dahil
   edilir, ama self-check'i yalnizca semantic.c cagirir (bkz.
   semantic_diag_selfcheck()) -- o diger derleme birimlerinde "tanimli
   ama kullanilmiyor" uyarisini bastirmak icin GCC/Clang'in __unused__
   attribute'u kullanilir; bu, fonksiyonu gercekten cagirmayan bir .c
   dosyasinda link hatasina yol acmadan (static oldugu icin zaten
   acmazdi, sadece -Wunused-function'i susturur). */
static void diag_selfcheck_help_table(const char *semantic_c_path) __attribute__((unused));
static void diag_selfcheck_help_table(const char *semantic_c_path) {
    if (!getenv("RAPIDC_DIAG_DEBUG")) return;

    FILE *f = fopen(semantic_c_path ? semantic_c_path : "src/semantic.c", "r");
    if (!f) {
        fprintf(stderr, "[diag_selfcheck] semantic.c kaynagi bulunamadi, atlaniyor\n");
        return;
    }

    /* Tum dosyayi tek bir buffer'a oku (semantic.c birkaç yuz KB, bu tek
       seferlik gelistirici-modu kontrolu icin sorun degil). */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return; }
    char *src = malloc((size_t)sz + 1);
    if (!src) { fclose(f); return; }
    size_t n = fread(src, 1, (size_t)sz, f);
    src[n] = '\0';
    fclose(f);

    int stale = 0;
    for (int i = 0; DIAG_HELP_TABLE[i].needle; i++) {
        if (!strstr(src, DIAG_HELP_TABLE[i].needle)) {
            fprintf(stderr,
                "[diag_selfcheck] STALE needle, semantic.c'de artik bulunamiyor: \"%s\"\n",
                DIAG_HELP_TABLE[i].needle);
            stale++;
        }
    }
    free(src);

    if (stale) {
        fprintf(stderr,
            "[diag_selfcheck] %d adet kopmus eslesme bulundu -- bu mesajlar "
            "icin help satiri artik sessizce gosterilmiyor. diag.h'deki "
            "DIAG_HELP_TABLE'i semantic.c'deki guncel mesaj metinleriyle "
            "senkronlayin.\n", stale);
    } else {
        fprintf(stderr, "[diag_selfcheck] tum needle'lar semantic.c ile uyumlu\n");
    }
}

#endif /* RAPIDC_DIAG_H */
