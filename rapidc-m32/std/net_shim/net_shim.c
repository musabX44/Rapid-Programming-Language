/* net_shim.c — RapidC "std::net" için ince libcurl sarmalayıcısı.
 *
 * libcurl'ün "easy" API'si rapidc'nin extern fn'siyle doğrudan
 * bildirilemez, çünkü:
 *   - curl_easy_setopt(3) VARARGS ve union-tipli bir 3. argüman alıyor
 *     (bazen long, bazen char*, bazen function pointer) — rapidc'nin
 *     extern varargs desteği yalnızca "fazladan argümanlar tip
 *     kontrolünden muaf" demek, C'nin kendi varargs ABI'sini taklit
 *     etmiyor.
 *   - Yanıt gövdesini okumak bir WRITEFUNCTION callback'i gerektiriyor
 *     (fonksiyon pointer'ı C tarafına geçilmesi gerekiyor) — rapidc'de
 *     henüz fonksiyon pointer'ını FFI'a value olarak geçirme yok.
 *
 * Bu shim, tüm bu adımları (init → setopt'lar → perform → cleanup →
 * gövdeyi tek bir heap string'e topla) TEK bir düz fonksiyonda gizliyor:
 *
 *     char *rapidc_http_get(const char *url, long *out_status);
 *
 * rapidc tarafı bunu iki skaler argümanla (string url, int* status)
 * çağırıyor ve düz bir `string` (heap'te char*) geri alıyor — tıpkı
 * projenin kendi string'lerinin zaten temsil edildiği gibi, bu yüzden
 * `keyword`/`extern fn` tarafında hiçbir özel işlem gerekmiyor.
 */
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} RespBuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    RespBuf *b = (RespBuf *)userdata;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 4096;
        while (newcap < b->len + n + 1) newcap *= 2;
        b->data = realloc(b->data, newcap);
        b->cap = newcap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* Basit HTTP GET. Dönüş değeri her zaman malloc'lu, NUL-terminated bir
 * gövde (hata durumunda boş string "") — rapidc'nin string'leri zaten
 * ham char* olarak heap'te tutulduğu için ekstra bir kopyalama/dönüşüm
 * gerekmiyor. HTTP durum kodu (200, 404, ...) *out_status'a yazılır;
 * ağ/DNS/TLS seviyesinde bir hata olursa (bağlantı hiç kurulamadıysa)
 * *out_status -1 olur.
 */
char *rapidc_http_get(const char *url, long *out_status) {
    RespBuf buf = {0};
    buf.data = malloc(1);
    buf.data[0] = '\0';
    buf.cap = 1;

    CURL *h = curl_easy_init();
    if (!h) {
        if (out_status) *out_status = -1;
        return buf.data;
    }

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "rapidc-std-net/1.0");
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 15L);

    CURLcode rc = curl_easy_perform(h);

    long status = -1;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    }
    if (out_status) *out_status = status;

    curl_easy_cleanup(h);
    return buf.data; /* rapidc tarafı bunu düz bir `string` olarak alır */
}

/* rapidc'nin global pointer alloc/free deseniyle simetrik: gövdeyi
 * okuduktan sonra çağıranın belleği geri vermesi için ince bir free
 * sarmalayıcısı (doğrudan libc free()'yi extern fn ile bildirmek de
 * çalışırdı, ama isim netliği için ayrı tutuluyor). */
void rapidc_http_free(char *body) {
    free(body);
}
