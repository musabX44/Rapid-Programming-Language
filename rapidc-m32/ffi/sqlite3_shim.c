/* sqlite3_shim.c — rapidc'nin pointer-to-pointer ifade edememesi (yalnizca
   tek seviye `T*` destekleniyor) yuzunden, sqlite3_open'in gercek imzasi
   olan `int sqlite3_open(const char *filename, sqlite3 **ppDb)`'yi
   rapidc tarafinda ifade edilebilir hale getiren ince bir C sarmalayici.
   ppDb'yi disariya cikti parametresi olarak vermek yerine, acilan db
   handle'ini fonksiyonun DONUS DEGERI olarak (pointer'i long'a gomerek)
   verir; basarisizlikta 0 (NULL) doner, hata kodu ayrica
   rapidc_sqlite3_open_rc ile alinabilir.

   sqlite3.h burada yok (sadece runtime .so kurulu, -dev paketi degil), o
   yuzden ihtiyacimiz olan tek fonksiyonun imzasini elle ileri-bildiriyoruz
   -- sqlite3'un C ABI'si surumler arasi sabit oldugu icin bu guvenli. */
typedef struct sqlite3 sqlite3;
extern int sqlite3_open(const char *filename, sqlite3 **ppDb);

static int g_last_open_rc = 0;

long rapidc_sqlite3_open(const char *filename) {
    sqlite3 *db = 0;
    int rc = sqlite3_open(filename, &db);
    g_last_open_rc = rc;
    return (long)(void *)db;
}

int rapidc_sqlite3_open_rc(void) {
    return g_last_open_rc;
}
