#ifndef RAPIDC_LIMITS_H
#define RAPIDC_LIMITS_H

/* Sorun-5 fix: derleyicinin sabit boyutlu dizilerle sinirladigi her seyin
   limiti artik TEK bir yerde tanimli. Onceden ayni limit birden fazla
   dosyada elle tekrarlanmis ve senkron tutulmasi gerekiyordu (ozellikle
   MAX_FUNCS: semantic.c'deki FuncSig tablosu ile codegen.c'deki FuncInfo
   tablosu AYNI fonksiyon sayisini tutabilmeli, yoksa biri digerinden once
   dolup farkli bir noktada "too many functions" hatasi verir ya da daha
   kotusu, biri sessizce tasar). Bu dosya, boyle bir kaymayi derleme
   zamaninda imkansiz kilar: tek define, her iki .c dosyasi da ayni degeri
   okur.

   Limit birden fazla yerde "kavramsal olarak ayni sey" ama farkli isimle
   geciyorsa (orn. MAX_VARS / MAX_LOCALS: biri semantic.c'nin sembol
   tablosu, digeri codegen.c'nin yerel degisken tablosu icin, ama ikisi de
   "bir fonksiyondaki en fazla yerel/degisken sayisi" anlamina geliyor),
   o cift de burada tek bir alttaki define'a baglanir; isim farkini
   korumak cagri yerlerini degistirmemek icin (semantic.c MAX_VARS,
   codegen.c MAX_LOCALS kullanmaya devam eder), ama ikisi de ayni sayiya
   cozer. */

/* Bir fonksiyondaki en fazla yerel degisken/parametre sayisi.
   semantic.c: MAX_VARS (sembol tablosu + parametre dizileri)
   codegen.c:  MAX_LOCALS (LocalTable dizileri) */
#define RAPIDC_MAX_LOCALS_PER_FUNC 64

/* Programdaki en fazla fonksiyon (tum overload'lar dahil) sayisi.
   semantic.c: g_funcs[MAX_FUNCS] (FuncSig)
   codegen.c:  g_funcs[MAX_FUNCS] (FuncInfo)
   Bu ikisi FARKLI tablolar ama AYNI fonksiyon kumesini (aynii sirada)
   tutuyor olmali -- biri digerinden kucuk olursa semantic.c basarili
   derler ama codegen.c "too many functions" ile coker (ya da tam tersi),
   kullaniciya anlamsiz/tutarsiz bir hata verir. M36'da 64'ten 512'ye
   yukseltildi (array+map+matrix stdlib'leri birlikte 64'u asiyordu). */
#define RAPIDC_MAX_FUNCS 512

/* Ifade (expression) agacinin en fazla ic ice gecme derinligi.
   Hem semantic.c (tip kontrolu yaparken) hem codegen.c (IL uretirken)
   AYNI AST'yi ayni derinlikte dolasir; limit farkli olursa biri derler
   digeri coker (ya da ikisi de C call stack'ini tasirip SIGSEGV'ye
   gider -- asil amac buydu, bkz. gen_expr'deki SECURITY/DoS notu). */
#define RAPIDC_MAX_EXPR_DEPTH 400

/* Derleme hatasi/tani (diagnostic) sayisi icin sabit tampon -- ast.c */
#define RAPIDC_MAX_ERRORS 1024

/* codegen.c: program genelinde en fazla farkli string literal sayisi */
#define RAPIDC_MAX_STRINGS 8192

/* codegen.c: en fazla ic ice loop/switch derinligi (break/continue etiket
   yiginlari icin) */
#define RAPIDC_MAX_LOOP_DEPTH 32

/* codegen.c: tek bir fonksiyon govdesinde hoist edilen struct-tipli yerel
   degisken bildirimi sayisi (BUG-3 fix'inin alloc8 slotlari) */
#define RAPIDC_MAX_HOISTED_STRUCTS 256

/* codegen.c: tek bir statement icinde izlenen rapidc_strcat ara-sonuc
   temp'i sayisi (string-concat memory-leak fix'i, bkz. gen_stmt) */
#define RAPIDC_MAX_STRCAT_TMPS 256

/* module.c: proje genelinde en fazla modul (dosya) sayisi */
#define RAPIDC_MAX_MODULES 256

/* module.c: bir `use` yonergesindeki en fazla alt-alias (sub-alias) sayisi */
#define RAPIDC_MAX_SUBALIASES 64

/* module.c: en fazla global (modulsuz) alias sayisi */
#define RAPIDC_MAX_GLOBAL_ALIASES 16

#endif
