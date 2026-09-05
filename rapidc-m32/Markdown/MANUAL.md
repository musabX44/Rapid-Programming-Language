# rapidc — Rapid! Compiler (Milestone 1 prototype)

Rapid!, native x86-64 Linux ELF üreten, statik tipli bir sistem programlama dilidir.

## Pipeline
```
.rapid → Flex (lexer) → Bison (parser) → AST → Semantic Check → QBE IL → qbe → as/ld (via cc) → ELF64
```

## Gereksinimler
- flex
- bison
- qbe (https://c9x.me/compile/ — PATH içinde `qbe` olarak bulunmalı)
- cc/gcc (assembler+linker sürücüsü olarak kullanılıyor)
- make

Ubuntu/Debian:
```
apt-get install flex bison gcc make
# qbe için: kaynaktan derleyip PATH'e ekleyin (apt'ta paket yok)
```

## Build
```
make
```

## Kullanım
```
./rapidc test.rapid -o test_out
./test_out
```

## Milestone 1 kapsamı
- `fn main() { ... }` (parametre yok, dönüş tipi yazılmaz → void varsayılır)
- `var x: int = <expr>;` / `const X: int = <expr>;`
- `io::out(expr)` — built-in modül, println tarzı (newline ekler), int → stdout
- `return;` / `return <expr>;`
- Semantic check: undefined identifier, redeclaration tespiti

## Milestone 16/17/18 kapsamı (eklendi) — io:: genişletildi
- `io::in()` — stdin'den bir satır okur (trailing `\n` atılır), `string` döner.
  `in` bir keyword (`for (x in xs)`) olduğundan grammar'da ayrı bir kural
  gerekti (`IO COLONCOLON IN LPAREN ... RPAREN` bakılamıyor çünkü `in` normal
  bir `IDENT` olarak lex edilmiyor — ayrıca bkz. `parser.y`).
- `io::out(fmt, arg1, arg2, ...)` — 2+ argümanla çağrılan `io::out`, `fmt`
  içindeki her `{}` yer tutucusunu sırasıyla argümanlarla değiştirip
  (int/byte/string karışık olabilir) tek satır + `\n` basar. Tek argümanlı
  eski kullanım (`io::out(x)`) aynen çalışmaya devam ediyor — ayrı bir
  `io::format` adı yok, hepsi `io::out` altında.
  ```
  var name: string = io::in();
  io::out("Merhaba, {}! Yasin: {}", name, 25);
  ```
- `io::open(path: string, flags: int, mode: int): int` — `open(2)`'nin ince
  bir wrapper'ı, fd döner (hata durumunda negatif). `flags`/`mode` ham int
  olarak geçilir (dilde henüz `O_CREAT` gibi sabitler yok); Linux'ta
  yaygın değerler: `O_RDONLY=0`, `O_WRONLY=1`, `O_RDWR=2`, `O_CREAT=64`,
  `O_TRUNC=512`, `O_APPEND=1024` (bit-or ile birleştirilip tek int olarak
  geçilir, örn. `O_WRONLY|O_CREAT|O_TRUNC = 577`).
- `io::read(fd: int, buf: byte*|int*|string, count: int): int` — `read(2)`
  wrapper'ı, okunan byte sayısını (veya hata için negatif) döner.
- `io::write(fd: int, buf: byte*|int*|string, count: int): int` — `write(2)`
  wrapper'ı, aynı dönüş kuralı. `string` doğrudan buffer olarak kabul
  edilir çünkü rapidc string'leri zaten heap'te ham `char*` olarak tutuluyor.
- `io::close(fd: int): int` — `close(2)` wrapper'ı.
- `io::errno(): int` — en son `io::open`/`io::read`/`io::write`/`io::close`
  çağrısının bıraktığı `errno` değerini döner (0 = son çağrı başarılıydı).
  POSIX'teki `errno` ile aynı semantik: yalnızca **başarısızlık** durumunda
  anlamlıdır; başarılı bir çağrı `errno`'yu sıfırlamaz, önceki değeri
  olduğu gibi bırakabilir (C'nin kendisinde de böyledir) — yani
  `io::errno()`'yu her zaman ilgili `io::` çağrısının dönüş değeri negatifse
  (veya beklenenden farklıysa) kontrol edin, "her zaman güncel" bir global
  durum değildir. Yaygın değerler: `ENOENT=2` (dosya yok), `EACCES=13`
  (izin yok), `EEXIST=17` (zaten var).
- Bu dördü `sysprog::alloc/free/syscall` ile aynı desende: reserved isimler,
  kendi imza şekilleri fonksiyon-tablosu yerine `semantic.c`'de doğrudan
  kontrol ediliyor, `codegen.c`'de `runtime.c` içindeki ince C wrapper'lara
  (`rapidc_open/read/write/close`) düz `call` olarak indirgeniyor.
- **Milestone sınırı:** `open`'ın 3. argümanı (mode) `O_CREAT` yokken OS
  tarafından zaten yok sayılıyor ama yine de mecburi (dilde default/opsiyonel
  parametre yok); flag sabitleri dile eklenmedi, kullanıcı ham int giriyor.

## Milestone 2 kapsamı (eklendi)
- `bool` tipi
- Karşılaştırma operatörleri: `==`, `!=`, `<`, `<=`, `>`, `>=` (non-associative)
- `if (cond) { ... } else if (cond) { ... } else { ... }`
- Block-scoping (if içindeki var/const bloktan çıkınca görünmez)
- Semantic check: if koşulu bool olmalı

## Milestone 3 kapsamı (eklendi)
- Assignment: `x = <expr>;` (tip çıkarımı yok — hedef değişkenin zaten deklare edilmiş tipiyle eşleşmeli)
- `x++;` / `x--;` (sadece `int` üzerinde, `const` üzerinde yasak)
- `while (cond) { ... }`
- Semantic check: const'a assignment/++/-- yasak, while koşulu bool olmalı

## Milestone 4 kapsamı (eklendi)
- `for (var i: int = 0; i < N; i++) { ... }` — klasik üç parçalı for
- `break;` / `continue;` (en içteki döngüyü hedefler, iç içe döngülerde doğru davranış)
- Semantic check: for koşulu bool olmalı, break/continue döngü dışında yasak, for'un init değişkeni for'a özel scope'ta

## Milestone 5 kapsamı (eklendi)
- `string` tipi: string literal'ları (`"..."`, `\n`/`\t`/`\"`/`\\` escape desteğiyle), `+` ile birleştirme (çalışma zamanında `rapidc_strcat` runtime yardımcısıyla)
- `int[]` (sabit boyutlu dizi): `[1, 2, 3]` literal'ı, `xs[i]` okuma, `xs[i] = v;` yazma — QBE `alloc8` ile stack'te ayrılıyor
- `for (x in xs) { ... }` — dizi üzerinde gezinme (içeride `while` + gizli indeks sayaca desugar ediliyor)
- `switch (expr) { case v: ... break; ... default: ... }` — C tarzı fall-through, `break` switch'ten çıkar
- Aritmetik operatörler: `+ - * /` (int için); `+` ayrıca string birleştirme için overload edilmiş
- Semantic check: for-in hedefi array olmalı, case değer tipi switch subject'iyle eşleşmeli, en fazla bir `default`, dizi indeksleme/atama int tipiyle
- **Hata düzeltmesi (M4'te vardı, hiç fark edilmemişti):** `io::out` codegen'i QBE'nin variadic çağrı sözdizimini ters yazıyordu (`call $printf(l $fmt, ..., l %arg)` yerine doğrusu `call $printf(l $fmt, l %arg, ...)`). Bu yüzden M4 hiçbir zaman gerçek `qbe` ile derlenip çalıştırılamamıştı; M5 ile birlikte düzeltildi ve M4'ün kendi test dosyası da artık uçtan uca çalışıyor.

## Milestone 6 kapsamı (eklendi)
- Fonksiyon parametreleri: `fn add(a: int, b: int): int { ... }`
- Birden fazla fonksiyon: dosya artık tek `main()` değil, üst düzeyde tanımlanmış bir fonksiyon listesi (`main` aralarında herhangi bir sırada olabilir)
- Fonksiyon çağrıları: `f(a, b)`, dönüş değeri bir ifade olarak kullanılabilir
- Recursion serbestçe çalışır (fonksiyonlar isimle çağrılıyor; semantic check iki geçişli — önce tüm imzalar toplanıyor, sonra gövdeler kontrol ediliyor — bu yüzden ileri referanslar ve doğrudan/dolaylı recursion sorun değil)
- `return <expr>;` artık dönüş tipiyle eşleşiyor mu diye kontrol ediliyor; `void` fonksiyonda değerli `return` yasak, `void`-olmayan fonksiyonda değersiz `return` yasak
- Codegen: her kullanıcı fonksiyonu `$f_<isim>` olarak QBE'ye emit ediliyor; gerçek process girişi (`main`) sadece `$f_main`'i çağıran ince bir wrapper (bu sayede kullanıcı kodu "main" adını bir fonksiyon değeri olarak da çekişmeden kullanabilir)
- Semantic check: fonksiyon yeniden tanımlama tespiti, çağrıda argüman sayısı/tipi eşleşmesi, `main`'in parametresiz ve `void` olması zorunluluğu

## Milestone 12 kapsamı (eklendi)
- Ham sistem programlama primitifleri — `push`/`listCopy`/`len` gibi
  derleyici tarafından rezerve edilmiş (reserved) top-level fonksiyonlar:
  ```
  var p: int* = alloc(64);   // malloc benzeri, byte cinsinden boyut
  *p = 42;
  free(p);

  var pid: int = syscall(39, 0, 0, 0, 0, 0, 0); // SYS_getpid
  ```
- `alloc(size: int): int*` — `int*` tipi (M7'den beri var olan generic
  pointer) döndürür; runtime'da `malloc` wrapper'ı (`rapidc_alloc`).
- `free(ptr: int*): void` — `free` wrapper'ı (`rapidc_free`).
- `syscall(nr: int, a1, a2, a3, a4, a5, a6): int` — 1 ile 7 arasında int
  argüman alır (syscall numarası + en fazla 6 argüman, eksik olanlar 0
  varsayılır), Linux'un ham syscall arayüzüne (`syscall(2)`) doğrudan
  ulaşır; runtime'daki `rapidc_syscall6` glibc'nin `syscall()` fonksiyonunu
  sarmalıyor. Register/stack yönetimi elle yazılmıyor — QBE'nin kendi
  calling-convention codegen'i (amd64_sysv) bu çağrı için doğru register'lara
  argüman yerleştirmeyi zaten hallediyor.
- `alloc`/`free`/`syscall` derleyici tarafından ayrılmış (reserved) isimler
  — kullanıcı fonksiyonu olarak yeniden tanımlanamaz (`push`/`listCopy`/`len`
  ile aynı muamele).
- Semantic check: `alloc` tam 1 int argüman, `free` tam 1 `int*` argüman,
  `syscall` 1-7 int argüman alır; tip uyuşmazlıklarında hata verilir.
- **Milestone sınırı:** `int*` düz bir generic pointer tag'i olduğundan
  pointer aritmetiği (`p + 1` gibi) veya tipli/boyutlu pointer'lar
  (`T*` genel formu) desteklenmiyor — bu ve `mmap`/`brk` gibi daha üst
  seviye bellek primitifleri gelecek bir milestone'a bırakıldı.

## Milestone 29 kapsamı (eklendi) — FFI ile kullanıcı tanımlı `keyword`

- `keyword ad(param: tip, ...) : dönüş_tipi => extern "csembol" [from "kütüphane"];`
  — derleyicinin kendi kaynak koduna (lexer.l/parser.y/semantic.c/codegen.c)
  hiç dokunmadan, bir `.rapid` dosyası içinden FFI aracılığıyla yeni bir
  "keyword" (çağrılabilir yerleşik gibi davranan bir isim) tanımlar.
  ```
  keyword sqrt(x: double): double => extern "sqrt" from "m";
  keyword strlen(s: string): int  => extern "strlen";
  keyword printf(fmt: string, ...): int => extern "printf";
  keyword exit(code: int): void => extern "exit";

  fn main() {
      io::out(sqrt(2.0));      // 1.41421
      io::out(strlen("merhaba")); // 7
      printf("sayi: %ld\n", 42);
      exit(0);
  }
  ```
- **Nasıl çalışır (saf metin düzeyinde makro):** `keyword` bildirimleri,
  modül birleştirme adımının (`module.c: resolve_modules`) en sonunda,
  kaynak lexer'a verilmeden hemen önce çalışan yeni bir metin-genişletme
  geçişinde (`expand_keyword_decls`) tanınır ve saf metinsel olarak
  zaten var olan M27/M28 `extern fn` + `link` sözdizimine dönüştürülür:
  ```
  keyword sqrt(x: double): double => extern "sqrt" from "m";
  ```
  yukarıdaki satır, lexer hiç görmeden önce şuna genişler:
  ```
  link "m";
  extern fn sqrt(x: double): double;
  ```
  Bu sayede yeni bir token, yeni bir AST düğümü, yeni bir semantic-check
  dalı ya da yeni bir codegen case'i gerekmez — çağrı sitesindeki tip
  kontrolü, `$<isim>` codegen'i ve `-l<kütüphane>` link bayrağı tamamen
  mevcut `extern fn`/`link` makinesinden miras alınır.
- `keyword`, `use "dosya.rapid";` ile içe aktarılan bir dosyanın içinde de
  tanımlanabilir (modül birleştirmesi, `keyword` genişletmesinden önce
  çalıştığı için) — yani bir "FFI keyword kütüphanesi" ayrı bir `.rapid`
  dosyasında toplanıp projeler arasında `use` ile paylaşılabilir.
- Parametre listesi ve dönüş tipi, sözdizimsel olarak birebir `extern fn`
  ile aynıdır: varargs için sondaki `, ...` desteklenir, `void` dönüş
  `: void` ile yazılabilir, `from "kütüphane"` yan tümcesi opsiyoneldir
  (verilmezse yalnızca libc'de zaten örtük linklenen semboller hedeflenebilir,
  tıpkı düz `extern fn` gibi).
- **Yeniden adlandırma (`ad` ≠ `csembol`) desteklenir:** `keyword ad(...) =>
  extern "csembol" ...;` içinde `ad` ile `csembol` farklı olabilir — örneğin
  bir C shim fonksiyonunu (`rapidc_http_get`) RapidC tarafında daha kısa/
  temiz bir isimle (`http_get`) sunmak istendiğinde. Bu durumda
  `expand_keyword_decls` iki parça üretir: gerçek C sembolü altında
  namespace'lenmiş gizli bir `extern fn __kw_ffi_<csembol>(...)` bildirimi,
  artı çağrı sitelerinin gördüğü genel `ad` ismi altında, tüm parametreleri
  pozisyonel sırayla gizli extern'e ileten küçük bir `efn` forwarding
  wrapper'ı (`efn ad(...) => __kw_ffi_<csembol>(...);`). Örnek:
  ```
  keyword http_get(url: string, out_status: int*): string
      => extern "rapidc_http_get";
  ```
  şuna genişler:
  ```
  extern fn __kw_ffi_rapidc_http_get(url: string, out_status: int*): string;
  efn http_get(url: string, out_status: int*) => __kw_ffi_rapidc_http_get(url, out_status);
  ```
  Bu da hâlâ derleyici koduna dokunmadan yapılır: `extern fn` ve `efn` zaten
  var olan grammar kuralları, ikisinin birleşimi de yalnızca metinsel
  genişletme. Varargs içeren bir imzada yeniden adlandırma desteklenmez
  (`efn` wrapper'ı `...`'yi ileremez) — bu durumda `expand_keyword_decls`
  derleme zamanında net bir hata verir.
- Parametre listesi ve dönüş tipi, sözdizimsel olarak birebir `extern fn`
  ile aynıdır: varargs için sondaki `, ...` desteklenir (yalnızca `ad` ==
  `csembol` olduğunda, yukarıdaki yeniden adlandırma sınırı gereği),
  `void` dönüş `: void` ile yazılabilir, `from "kütüphane"` yan tümcesi
  opsiyoneldir (verilmezse yalnızca libc'de zaten örtük linklenen semboller
  hedeflenebilir, tıpkı düz `extern fn` gibi).
- `extern fn`'in kendi milestone sınırları (harici kütüphane linklemesi
  `link` ile mümkün, struct by-value garantisi yok vb.) `keyword` için de
  aynen geçerlidir, çünkü altta üretilen kod birebir bir `extern fn`
  (+ isim eşleşmiyorsa bir `efn` wrapper'ı) dır.

## Kapsam dışı (sonraki milestone'lar)
- Diğer primitive tipler (char, byte, int8..64, uint*, float32/64, decimal)
- `map`
- First-class fonksiyon değerleri (`fn` tipi), closure yokluğu
- `?` error-propagation operatörü, `defer`
- LAM bellek modeli, `use` modül sistemi, `stdlib/*`, `net::`, `sysprog::`

## Milestone 10 kapsamı (eklendi)
- `efn` (Easy Function): tek expression'dan oluşan fonksiyonlar için kısa syntax
  ```
  efn square(x) => x * x
  efn add(a, b) => a + b
  efn greet(name) => "Hello " + name
  efn calculate(x) => square(double(x))
  ```
  `efn NAME(PARAMS) => EXPRESSION`, parser seviyesinde normal bir `fn`'e
  (gövdesi tek bir `return EXPRESSION;` olan) desugar edilir; `return`, `end`,
  `{}` veya block body kullanılmaz, sonuç otomatik döndürülür.
- Parametreler tipsiz yazılabilir (`efn square(x) => ...`, `x` varsayılan
  olarak `int` kabul edilir) ya da normal `fn` gibi açıkça tiplenebilir
  (`efn greet(name: string) => ...`), ikisi aynı `efn` içinde karışık olarak
  da kullanılabilir.
- Dönüş tipi **çıkarılır**: `efn`'in gövde ifadesinin tipi, semantic check
  sırasında (fonksiyon imzaları kaydedildikten hemen sonra, gövdeler
  kontrol edilmeden önce) hesaplanıp fonksiyonun gerçek `ret_type`'ı olarak
  kaydedilir. Bu sayede `efn`'ler birbirini ve normal fonksiyonları,
  tanım sırasından bağımsız olarak (ileri referans / karşılıklı çağrı dahil)
  çağırabilir — normal `fn` recursion/forward-reference desteğiyle aynı.
- `efn` ile tanımlanan fonksiyonlar normal fonksiyonlarla tamamen aynı
  kurallara tabidir: yeniden tanımlama, reserved isim (`push`/`listCopy`/`len`)
  çakışması ve çağrı-tipi uyuşmazlığı denetimleri aynen uygulanır.
- Yeni syntax eklenmedi (pipeline `|>`, `where`, özel `if/then/else` vb. yok);
  `efn` sadece mevcut expression grammar'ını kullanan sözdizimsel bir kısayoldur.

## Milestone 11 kapsamı (eklendi)
- First-class fonksiyonlar (**closure yok** — C'deki fonksiyon pointer'ı gibi,
  sadece top-level bir `fn`/`efn`'in referansı taşınıyor, ortam yakalama yok):
  ```
  var op: fn(int): int = square;
  io::out(op(5));
  op = double;
  fn apply(f: fn(int): int, x: int): int { return f(x); }
  io::out(apply(square, 6));
  ```
- Yeni tip: `fn(T1, T2, ...): Tret` — değişken/const/parametre/dönüş tipi
  olarak kullanılabilir.
- Bare bir fonksiyon adı, çağrılmadan (parantezsiz) kullanıldığında otomatik
  olarak bir fonksiyon değeri (`fn(...)` tipinde) sayılır; `efn`'ler de dahil
  her top-level fonksiyon bu şekilde değer olarak alınabilir.
- Fonksiyon değeri tutan bir değişken/parametre üzerinden çağrı (`f(x)`)
  otomatik olarak indirect call'a (QBE'de register üzerinden `call`)
  desugar edilir; normal isimle çağrı (`add(1,2)`) hâlâ direkt `call $f_add`
  olarak kalır.
- Reserved builtin isimler (`push`/`listCopy`/`len`) fonksiyon değeri olarak
  alınamaz (zaten kullanıcı fonksiyonu olarak tanımlanamıyorlardı).
- **Milestone sınırı:** `fn(...)` tipi düz bir tag (`TypeKind`) olduğundan,
  bir değişkene atanan gerçek fonksiyonun imzası (parametre/dönüş tipleri)
  değişkenin bildirilen `fn(...)` imzasıyla veya bir indirect call'daki
  argümanlarla satır satır karşılaştırılmıyor — bu tam imza kontrolü
  gelecek bir milestone'a bırakıldı. Anonim/lambda fonksiyonlar ve
  struct/array/list içinde fonksiyon değeri tutmak da kapsam dışı.

## Milestone 9 kapsamı (eklendi)
- `list<int>`: heap'te tutulan, büyüyebilen (dinamik) int dizisi. Runtime temsili: `[len:8][cap:8][data...]` şeklinde bir header + bitişik veri (`src/runtime.c`, `RAPIDC_LIST_HEADER_BYTES`)
- `list<int> xs = [1, 2, 3];` / `list<int> xs = [];` — array literal'den (boş dahil) dinamik liste oluşturma
- `push(xs, v)` — sona eleman ekler, gerekirse kapasiteyi 2 katına çıkararak realloc eder; `xs` değişkeni otomatik olarak (olası) yeni pointer'a güncellenir
- `listCopy(xs)` — bağımsız, derin bir kopya döndürür (elemanları paylaşmaz, ayrı ayrı büyüyebilir)
- `len(xs)` — hem `list<int>` hem sabit `int[]` için eleman sayısı (liste için runtime'da, sabit dizi için compile-time'da hesaplanır)
- `xs[i]` okuma/yazma ve `for (x in xs) { ... }` artık hem `int[]` hem `list<int>` üzerinde çalışıyor
- `push`/`listCopy`/`len` derleyici tarafından ayrılmış (reserved) isimler — kullanıcı fonksiyonu olarak yeniden tanımlanamaz
- Semantic check: `push` tam olarak 2 argüman alır (birincisi bir `list<int>` değişkeni olmalı, ikincisi `int`), `listCopy`/`len` tip uyumsuzluklarını denetler

## Milestone 20 kapsamı (eklendi) — modül sistemi (`use`)
- `use "dosya.rapid";` — belirtilen dosyayı (kullanan dosyanın bulunduğu
  dizine göre relative) derlemeye dahil eder. Tamamen bir **kaynak-seviyesi
  önişlemci** olarak çalışır (`src/module.c`): `main.c`, `yyparse()`'ı
  çağırmadan önce `resolve_modules()` ile tüm `use` zincirini takip edip
  hepsini tek bir bellek arabelleğinde birleştirir; lexer/parser/semantic/
  codegen'in geri kalanı bunun tek bir `.rapid` dosyası olduğunu bile
  bilmez — "basit dosya-bazlı derleme birleştirme (tek IR dosyasına
  linkleme)" milestone hedefi böylece doğal olarak sağlanmış olur (tek
  AST, tek semantic pass, tek `.ssa` çıktısı).
  ```
  // mathlib.rapid
  fn add(a: int, b: int): int { return a + b; }

  // main.rapid
  use "mathlib.rapid";
  fn main() { io::out(add(2, 3)); }
  ```
- **Aynı dosya birden fazla yerden `use` edilirse yalnızca bir kez**
  derlemeye dahil edilir (compile-once / "diamond import" desteklenir);
  döngüsel `use` zincirleri (`a.rapid` → `b.rapid` → `a.rapid`) derleme
  zamanında net bir hata ile reddedilir.
- `use "dosya.rapid" as takma_ad;` — dosyayı bir **alias** altında içe
  aktarır; o dosyanın herkese açık (private olmayan) her üst-seviye
  `fn`/`efn`/`struct`/`enum` ismi yalnızca `takma_ad::isim` şeklinde
  erişilebilir olur (fonksiyon çağrıları ve tip adları için). Bu, iki farklı
  modülün aynı isimde public bir şey tanımladığı durumlarda isim
  çakışmalarını çözmenin standart yoludur:
  ```
  use "vecmath.rapid" as vm;
  use "strmath.rapid" as sm;
  fn main() {
      io::out(vm::add(3, 4)); // vecmath.rapid'in add'i
      io::out(sm::add(3, 4)); // strmath.rapid'in add'i (farklı davranış)
  }
  ```
- `private fn` / `private efn` / `private struct` / `private enum` — bir
  üst-seviye tanımı yalnızca kendi dosyasına kapalı yapar; o dosya içindeki
  diğer fonksiyonlar onu serbestçe kullanabilir, ama onu `use` eden başka
  bir dosya (alias ile bile) o isme hiçbir şekilde erişemez.
  ```
  // helper.rapid
  private fn secret(x: int): int { return x + 100; }
  fn publicFn(x: int): int { return secret(x); } // OK, aynı dosya

  // main.rapid
  use "helper.rapid";
  fn main() {
      io::out(publicFn(5)); // OK
      io::out(secret(5));   // semantic error: call to undefined function
  }
  ```
- Bir modülün kendi `main` fonksiyonu — entry-point dosyası dışında her
  zaman örtük olarak private kabul edilir (bir modül programın giriş
  noktasını sağlayamaz), böylece iki dosyanın da bağımsız birer `main`
  tanımlaması derleme zamanında `main` çakışmasına yol açmaz.
- **Uygulama detayı (mangling):** `private` isimler dosyaya özgü
  `isim__privN` şekline, alias'lı public isimler `takma_ad__isim` şekline
  metin seviyesinde yeniden adlandırılır (string/char literal'ler ve
  yorum satırları bu taramadan muaf tutulur); `takma_ad::isim` çağrı/tip
  siteleri de aynı şemaya göre otomatik olarak `takma_ad__isim`'e
  dönüştürülür, böylece iki taraf her zaman aynı ismi üretir.
- **Milestone sınırı:** `use` taraması basit (tam bir tokenizer değil) bir
  üst-seviye tarayıcıdır — yalnızca satır başı `fn`/`efn`/`struct`/`enum`/
  `use`/`private` anahtar kelimelerini tanır; bu, dilin zaten yalnızca
  üst-seviyede tanım yapılmasına izin veren sabit grammar'ıyla tutarlıdır.
  Döngüsel olmayan ama çok derin `use` zincirleri `MAX_MODULES` (256) ile
  sınırlıdır. `use` yolları her zaman kullanan dosyaya göre relative'dir;
  mutlak yol (`/` ile başlayan) da kabul edilir.

## Milestone 23 kapsamı (eklendi) — sabit genişlikli tamsayı tipleri

- `int8`/`int16`/`int32`/`int64` (işaretli) ve `uint8`/`uint16`/`uint32`/
  `uint64` (işaretsiz) eklendi. `int`/`byte` mevcut anlamlarını (sırasıyla
  64-bit işaretli, 8-bit işaretsiz) korur; yeni tipler eş anlamlı değil,
  ayrı `TypeKind` değerleridir.
- Herhangi iki tamsayı tipi (`int`/`byte`/`int8..64`/`uint8..64`) aritmetik,
  karşılaştırma, atama, deklarasyon ve fonksiyon çağrısı argümanlarında
  birbirleriyle örtük olarak uyumludur (daraltma/genişletme örtük yapılır);
  karışık genişlikli bir ifadede sonuç tipi C'nin "usual arithmetic
  conversions" kuralına yakın şekilde belirlenir (daha geniş olan kazanır,
  eşit genişlikte işaretsiz kazanır).
- **Milestone sınırı:** yeni genişliklere işaretçi (`int8*`, `uint32*`, ...)
  eklenmedi — yalnızca `int*`/`byte*` destekleniyor. Ayrıca codegen tarafında
  bu tipler hâlâ tam 64-bit (`l`) slotlarda tutuluyor; genişliğe özgü
  `load`/`store` genişlik farkı (`loadsb`/`loadsh`/`loadsw` gibi) henüz
  uygulanmadı — bu bir sonraki milestone'a bırakıldı.

## Milestone 25 kapsamı (eklendi) — `float` / `double`

- `float` (32-bit IEEE-754) ve `double` (64-bit IEEE-754) eklendi. Bunlar,
  int ailesinin aksine QBE'de kendi register sınıflarını kullanır (`float`
  için `s`, `double` için `d`) — codegen'de artık her değerin int/pointer
  mi yoksa float-ailesi mi olduğuna göre farklı `load`/`store`/aritmetik/
  karşılaştırma opcode'ları (`loads`/`stores`/`loadd`/`stored`, `adds`/
  `addd`, `ceqs`/`ceqd`, ...) üretiliyor.
- Ondalıklı sayı literal'leri (`3.14`, `2.0`) lexer'da `float`/`double`
  anahtar kelimelerinden ayrı olarak tanınır (`{DIGIT}+"."{DIGIT}+`); tek
  başına yazıldıklarında (tıpkı tipsiz bir int literalinin `int`'e
  varsaymasına benzer şekilde) `double`'a varsayılır, ama bir `float`
  değişkenini başlatmak için de kullanılabilirler (örn.
  `var f: float = 3.14;`).
- `float` ve `double` birbirleriyle aritmetik/karşılaştırma/atama/
  deklarasyon/fonksiyon-argümanı bağlamlarında örtük olarak uyumludur (int
  ailesindeki genişlik uyumluluğuna benzer şekilde); `float op double`
  ifadesi her zaman `double`'a genişletilir (QBE düzeyinde `exts` ile).
  **Tamsayı ve float tipleri ise kasıtlı olarak birbirine örtük dönüştürülmez**
  — `int + float` gibi bir ifade semantic error verir; bu, int<->byte gibi
  "temsili aynı" tipler arasındaki örtük dönüşümden farklı olarak gerçek
  bir temsil (ve QBE register sınıfı) değişikliği gizlememek içindir.
  Açık `toFloat()`/`toDouble()`/`toInt()` gibi dönüştürme builtin'leri
  gelecekteki bir milestone'a bırakıldı.
- Fonksiyon parametreleri, dönüş tipleri ve çağrı argümanları artık
  QBE'de kendi sınıflarıyla (`s`/`d`) geçiriliyor — bu, x86-64 SysV ABI'de
  float/double değerlerin genel amaçlı registerlar yerine XMM registerları
  üzerinden geçirilmesi gerektiği için codegen'de her çağrı sitesinin
  argüman sınıfını statik tipe göre (bir önceki milestonelardaki gibi sabit
  `l` yerine) hesaplaması gerekti.
- `io::out(f)` bir float/double değeri `%g` formatıyla (ondalık basamak
  sayısını sabitlemeden, gereksiz sondaki sıfırları atarak) yazdırır. `printf`
  varargs kuralı gereği bir `float` değeri her zaman `double`'a genişletilip
  öyle geçiriliyor (C'nin kendisinde de varargs'a giren `float`'lar örtük
  olarak `double`'a yükseltilir).
- **Milestone sınırı:** `float`/`double` işaretçisi (`float*`, `double*`)
  yok; `&floatDegiskeni` semantic error verir (yalnızca `int`/`byte`
  lvalue'ların adresi alınabilir, M23 notuyla aynı sınır). `float`/`double`
  üzerinde `++`/`--` desteklenmiyor (yalnızca tamsayı tipleri). Array/list
  eleman tipi olarak `float`/`double` henüz desteklenmiyor (bir sonraki
  milestone'un kapsamında). Çok argümanlı `io::out(fmt, arg1, ...)`
  (`{}` şablonlu biçim) içinde float/double argüman kabul edilmiyor —
  yalnızca int/byte/string.
- **Geriye dönük uyumluluk notu:** `float` ve `double` artık rezerve
  keyword'ler; bu isimleri fonksiyon/değişken adı olarak kullanan önceki
  kod artık derlenmez (örn. `efn double(x) => x * 2` yerine başka bir isim
  seçilmeli — tıpkı `int`/`bool`/`string` gibi diğer tip adlarının da hep
  rezerve olması gibi).

## Milestone 27 kapsamı (eklendi) — minimal FFI (`extern fn`)

- `extern fn ad(param: tip, ...): dönüş_tipi;` — gövdesiz, dış (tipik olarak
  C/libc) bir fonksiyonun bildirimi. `;` ile biter (normal `fn` gibi bir
  `{ ... }` bloğu yok). Aksine `fn`'in "parametre yoksa void varsayılır"
  kısayolunun tersine, `extern fn` her zaman dönüş tipini açıkça ister —
  gövdesi olmadığı için çıkarılacak bir şey yok; `void` dönüş de
  `: void` ile yazılabilir.
  ```
  extern fn strlen(s: string): int;
  extern fn puts(s: string): int;
  extern fn exit(code: int): void;

  fn main() {
      var n: int = strlen("merhaba");
      io::out(n);          // 7
      puts("cikmadan once");
      exit(0);
  }
  ```
- **Varargs:** parametre listesi `, ...` ile bitebilir (yalnızca `extern
  fn`'de; kullanıcı `fn`/`efn`'i variadic olamaz). Bu durumda çağrı
  sitesinde bildirilen sabit parametrelerden fazla argüman kabul edilir;
  fazla argümanlar yine de tip kontrolünden geçer (yalnızca bildirilmiş bir
  parametre tipiyle eşleştirilmezler). QBE tarafında bu, `io::out`'un
  kendi `printf` çağrısında zaten kullandığı `call $fn(..., ...)` varargs
  işaretiyle aynı şekilde emit edilir.
  ```
  extern fn printf(fmt: string, ...): int;
  fn main() {
      printf("sayi: %ld\n", 42);
      printf("iki: %ld ve %ld\n", 7, 9);
  }
  ```
- **Codegen:** normal kullanıcı fonksiyonları her zaman `$f_<isim>` olarak
  mangle edilirken (M6), `extern fn` çağrıları düz `$<isim>` sembolüne
  (`$printf`, `$strlen`, ...) çağrı üretir — bu isimler rapidc'nin ürettiği
  koddan gelmediği için mangling'e tabi tutulmazlar. Linkleme adımı
  değişmedi (`cc -no-pie ... -o output`): `cc` sürücü olarak kullanıldığı
  için glibc zaten örtük olarak linkleniyor; ek bir `-l<kütüphane>` bayrağı
  eklemenin bir yolu henüz yok (bkz. milestone sınırı).
- Semantic olarak `extern fn`, diğer her fonksiyon gibi tek bir global
  imza tablosuna kaydedilir (Pass 1) — bu sayede çağrı sitesindeki
  argüman sayısı/tip kontrolü, redeclaration tespiti ve `alloc`/`free`/
  `syscall` gibi rezerve isimlerle çakışma kontrolü hiçbir yeni kod
  yazılmadan otomatik olarak uygulanır; yalnızca Pass 2'nin gövde
  kontrolü extern fonksiyonlar için atlanır (gövde olmadığı için).
- **Milestone sınırı:** extern fonksiyonlar first-class-function
  registry'sine kaydedilmez — yani bir `extern fn`'in adı `fn(...)::ret`
  tipli bir değişkene atanamaz veya dolaylı çağrılamaz, yalnızca doğrudan
  `ad(...)` şeklinde çağrılabilir (`semantic error: undefined identifier`
  ile net bir hata verir, sessizce yanlış davranmaz). Harici kütüphane
  linklemesi (`-lm`, `-lsqlite3`, ...) desteklenmiyor — yalnızca `cc`'nin
  zaten örtük linklediği libc sembolleri erişilebilir. Struct'ı değer
  olarak (by-value) bir extern fonksiyona geçirmek/döndürmek test
  edilmedi ve C ABI'siyle layout uyumu garanti değildir — yalnızca skaler
  tipler (int aileleri, `bool`, `float`/`double`, `string`, pointer'lar)
  önerilir.
