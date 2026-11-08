# Alembic

[English](README.md) · [Türkçe](README.tr.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · **Magyar** · [Русский](README.ru.md) · [日本語](README.ja.md) · [简体中文](README.zh.md)

*Ez az [angol nyelvű README](README.md) fordítása; ha eltérnek, az angol a mérvadó.*

![Az Alembic oldala a PS5 böngészőjében: többféle formátumú játékok könyvtára, bal oldalon meghajtókkal, egy NAS-megosztással, egy NFS-exporttal, egy FTPS-szerverrel és egy párosított PC-vel](docs/screenshots/files.png)

Az Alembic a PS5- és PS4-játékokat alakítja át a homebrew betöltők által
használt formátumok között: csomagok (PS5 debug FPKG, PS4 fake PKG), dump
mappák, exFAT image-ek, FFPKG (UFS2), FFPFS (PFS) és FFPFSC (tömörített PFS).
**A konzolon** fut, a kezdőképernyőn alkalmazásként (0.3.0-tól) vagy
payloadként egy weboldallal, mindkettő kontrollerrel kezelhető; PC-n
(Windows, macOS, Linux) pedig parancssori eszközként, illetve ugyanazzal a
motorral és oldallal. A hálózaton lévő motorok párosíthatók és együtt
dolgozhatnak: a PC-n lévő csomag a PC processzorán alakítható át, egyenesen
a konzol tárhelyére. A PC-n vagy NAS-on lévő játékok SMB-n, NFS-en,
WebDAV-on vagy FTP-n keresztül is olvashatók és oda menthetők.

> Fejlesztés alatt. Készíts biztonsági mentést mindenről, amit átalakítasz,
> amíg nem ellenőrizted az eredményt a konzolodon.

**Eddig konzolon kipróbálva:** a dumpokból és csomagokból készült FFPFSC
image-ek futnak a ShadowMount+-szal; az AMPR-emulátort igénylő játékok
elindulnak, miután hozzáadtuk; a konzol és egy PC párosodnak, és egymásnak
alakítanak át; a **Telepítés** átadja a debug csomagot a konzol
telepítőjének, beleértve az Alembic által dumpból készített csomagokat is
(tárolt vagy Kraken, generált debug licenccel, így a címre nem kerül
zárolás); a szüneteltetett átalakítás a motor frissítése után folytatódik;
a motor egy kiadásból frissíti önmagát (Payload Manageren keresztül
betöltve); a konzolos alkalmazás elindul a ShadowMount+-ból, elindítja és
újraindítja a motort, és átalakítást vezérel egy párosított PC-n; egy
SMB-megosztásról átalakított csomag; egy zstd-csomagokba tömörített játék
(egy 160 GB-os csomag, amelyet egy PC-s motor 5-ös szinten csomagolt egy
96 GB-os, a konzolra írt exFAT-ba) elindul az ampr-emu-ng-vel.
A csomagjukból átalakított PS4-játékok image-ből elindulnak a
ShadowMount+ 1.7beta8-cal (ezeknél „Trophy registration failed” üzenetet
mutat: ez a ShadowMount+ hibája,
[#97](https://github.com/drakmor/ShadowMountPlus/issues/97)).
**Még nem:** NFS, WebDAV, SMB-titkosítás, FTPS és HTTPS konzolról, valamint
valódi NAS-eszközök és Windows-megosztások (mindegyiket PC-n ellenőriztük
Samba, rclone, WsgiDAV és pyftpdlib ellen); Alembic-csomagból telepített
játék elindítása (a telepítést kipróbáltuk, az indítást nem); Alembic által
készített PS4-csomag; PS4-frissítések (patch csomagok) és DLC-k. Az LZ4
asset csomagok egyes játékoknál működnek, másoknál nem.

## Mit csinál

| Miből ↓ / Mibe → | Mappa | exFAT | FFPKG (UFS2) | FFPFS (PFS) | FFPFSC | Csomag |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| Dump mappa | – | ✓ | ✓ | ✓ | ✓ | ✓ |
| exFAT | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPKG (UFS2) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFS (PFS) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFSC | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| PS5 FPKG (debug) | ✓ | ✓ | ✓ | ✓ | ✓ | `--rebuild` |
| PS4 PKG (fake) | ✓ | ✓ | ✓ | ✓ | ✓ | `--rebuild` |

Hogy egy játékból milyen csomag lesz, az a játéktól függ: egy PS5-játékból
(`sce_sys/param.json`) PS5 debug FPKG, egy PS4-játékból (`sce_sys/param.sfo`)
PS4 fake PKG. Az oldal és az alkalmazás minden játékot **PS5** (fehér) vagy
**PS4** (fekete) címkével jelöl.

- **PS4-játékok** (0.3.2-től): a szokásos csupa nulla jelszóval készült fake
  PKG-k (régebbi és újabb csomagolóeszközökből egyaránt) úgy nyílnak meg,
  mint a PS5-csomagok: a játék fájljai, valamint a csomagból származó
  `sce_sys` (`param.sfo`, ikonok, trófeák, licenc), és bármilyen image-dzsé
  átalakíthatók. A PS4-játékok image-ei és mappái megkapják azt a
  `sce_sys/param.json` fájlt, amely a ShadowMount+-nak az indításhoz kell
  (a `param.sfo`-ból készül, ahogy a
  [PS4pkg_to_ffpfsc](https://github.com/SadykovIV/PS4pkg_to_ffpfsc) is
  készíti); konzolon kipróbálva a ShadowMount+ 1.7beta8-cal. Bármilyen
  formátumú PS4-játék visszaalakítható PS4 fake PKG-vé, úgy, ahogy a
  [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) `pkg_build` eszköze
  készíti: a játék licence megmarad (csak hiányzás esetén generálódik), a
  PlayGo-fájlok pedig újra elkészülnek. Az ugyanannak a játéknak bármelyik
  image-éből újraépített csomag bájtra ugyanaz. A kulcsok a LibOrbisPkg-ből
  származnak (Komponensek, PC-n `--ps4-keys FILE`). Az AMPR-emulátor és az
  LZ4-csomagok csak PS5-játékokhoz valók. Hogy egy játék melyik konzolra
  készült, az a címazonosítójából (`CUSA`, `PPSA`) derül ki, ennek híján
  abból az SDK-ból, amellyel az `eboot.bin` készült (homebrew); az
  `alembic game-info` mindkettőt megmutatja. Még nem: frissítések és DLC-k,
  valamint nem csupa nulla jelszóval készült csomagok.
- **Folyamatos átvitel:** minden bemenet fájlfaként olvasódik be, és
  egyenesen a kimenetbe íródik, ideiglenes másolatok nélkül, így egy 100
  GB-os csomaghoz csak az eredménynek kell hely.
- **Bájtra azonos** a referenciaeszközökkel: az exFAT, az FFPKG és az FFPFSC
  a [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool) kimenetével; az
  FFPFS a [MkPFS](https://github.com/PSBrew/MkPFS) kimenetével (`pack folder --raw
  --no-compress`); az FPKG-kimenet a
  [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool) kimenetével
  (`img_create`, tárolt vagy `--compression kraken`, ugyanazzal a rögzített
  seeddel); a PS4-csomagok a
  [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) `pkg_build` kimenetével,
  akkor is, ha a csomag abból a projektből épül újra, amelyet a `pkg_makegp4`
  kibont.
- **LZ4 asset csomagok** drakmor [ampr_emu](https://github.com/drakmor/ampr_emu)
  PackedStdio futtatókörnyezetéhez (`--lz4`), bájtra azonosak az
  `ampr_pack.py` kimenetével, a
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression)
  játékprofiljaival.
- **zstd-csomagok** quer3q [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng)
  emulátorához (`--zstd`, vagy átalakításkor „Csomagolás zstd-formátumba”),
  bájtra azonosak a [ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge)
  kimenetével: kisebbek az LZ4-nél és gyorsabban elkészülnek (egy 26 GiB-os
  játékfájlkészletből Macen 33 s alatt 11,1 GiB lett, szemben az LZ4 77 s
  alatti 17,5 GiB-jával). A játék megkapja az ampr-emu-ng `libSceAmpr.sprx`
  fájlját (a Komponensek közül); kapcsold ki a ShadowMount+ „update
  emulators” beállítását, hogy ne cserélje le egy olyanra, amely nem tudja
  olvasni őket. Szintek: 1, 5 (alapértelmezett) és 19. PS5-ön kipróbálva. A
  csomagolt játék az Átalakítás menüben váltható LZ4- és zstd-csomagok között
  („Váltás zstd-csomagokra”): a régi csomagok egyenesen az újakba
  dekódolódnak.
- **AMPR-emulátor:** a konzol APR fájlolvasóját használó játékoknak egy
  emulátor `libSceAmpr.sprx` fájljára van szükségük ahhoz, hogy image-ből
  vagy mappából elinduljanak: drakmor ampr_emu-jára vagy quer3q
  ampr-emu-ng-jére (a Komponensek megmutatja, melyik van használatban). Az
  Alembic átalakításkor hozzáadja, vagy egy már meglévő játékhoz is; ha a
  játékban már van egy, az „AMPR-emulátor hozzáadása” felajánlja a váltást a
  másikra.
- **Fake aláírás:** az a PS5-játék, amelynek a futtatható fájljai sima
  (aláíratlan) ELF-ek, sosem indul el. Az átalakítás közben ezek fake
  aláírású SELF-ekké válnak, és az Átalakítás ezt előre jelzi: image vagy
  mappa esetén bájtra azonosan az ampr-emu-ng `make_fself.py` kimenetével,
  csomag esetén a ProsperoPkgTool `img_create` kimenetével.
- **Gyors:** a tömörítés és a csomagok dekódolása minden magot használ; az
  AES, a CRC32C és a SHA-256 a processzor saját utasításait használja, ahol
  vannak; alapértelmezés szerint zlib 6-os szint (a PS5PKGTool-lal pontosan
  egyező kimenethez 9-es).

## A PS5-ön

A munkát a motor (`alembic.elf`) végzi, egy payload, amely azután is fut,
hogy bezártad, amivel vezérelted: a kezdőképernyőn lévő **Alembic
alkalmazást**, vagy a weboldalát a konzol böngészőjében (egy parancsikonról,
amelyet a Média lapra tesz) vagy bármely böngészőben a hálózatodon a
`http://<console-ip>:5940` címen. Az alkalmazás és az oldal ugyanazt mutatja,
és ugyanúgy működik.

![Az Alembic alkalmazás a PS5-ön: egy homebrew mappa többféle formátumú játékokkal, bal oldalon meghajtókkal, egy NAS-megosztással, egy NFS-exporttal és egy párosított PC-vel](docs/screenshots/app.png)

**Az alkalmazás** (`PPSA99594`, 0.3.0-tól) kétféleképpen telepíthető:

1. **A ProsperoStore-ból** (a legegyszerűbb): az Alembic szerepel a
   [ProsperoStore](https://github.com/darkxex/ProsperoStore) kínálatában, ami
   a natív homebrew-k konzolos áruháza ([az Alembic oldala](https://prosperostore.net/app/PPSA99594/)).
   Telepítsd onnan; az áruház frissíti is.
2. **Image-ként vagy mappaként**, a
   [ShadowMount+](https://github.com/drakmor/ShadowMountPlus) segítségével:
   másold az `alembic-app-<version>.ffpkg` fájlt a kiadások közül a
   `/data/homebrew/` mappába (vagy csomagold ki oda az
   `alembic-app-<version>.zip` fájlt: a `PPSA99594` mappát). A csempéje a
   ShadowMount+ következő keresése után megjelenik a kezdőképernyőn.

Indítsd el. Ha egyik motor sem válaszol, elküldi a sajátját az ELF-betöltőnek
a 9021-es porton (a motor nélküli alkalmazás előbb letölti a saját
verziójához tartozó motort a GitHub-kiadásból, és a `SHA256SUMS` alapján
ellenőrzi).

**Alkalmazás nélkül:**

1. Küldd el az `alembic-<version>.elf` fájlt (a kiadások közül) a konzolod
   ELF-betöltőjének.
2. Nyisd meg az Alembicet a Média lapról (vagy a fenti címen).
3. A **Fájlok** részen válassz egy játékot a belső tárhelyről, USB-ről, egy
   hálózati szerverről (SMB, NFS, WebDAV vagy FTP) vagy egy párosított motor
   meghajtójáról (egy címke különbözteti meg a PS5- és PS4-játékokat), majd
   **Átalakítás**, **Telepítés** (csomagok), **Másolás ide…** vagy
   **Áthelyezés ide…**. Egy PS5-játék csomaggá alakítása után a csomag a
   végén átadható a konzol telepítőjének (**Telepítés a befejezés után**);
   a csomag ezután törlődik, amint a telepítő beolvasta, hacsak nincs
   bekapcsolva a **Csomag megtartása**, így addig helyre van szüksége.
4. A feladatok egyenként futnak a **Várólista** részen: szüneteltetés,
   folytatás (a hosszú feladatok egy mentési ponttól folytatódnak),
   átrendezés, megszakítás. Értesítést kapsz, amikor egy befejeződik.

![Az Átalakítás ablak: célformátumok, tömörítési szint, az LZ4- és zstd-csomagok kapcsolói, az AMPR-emulátor kapcsolója, és a kimeneti útvonal egy párosított PC-n](docs/screenshots/convert.png)

![A várólista: egy futó átalakítás sebességgel és hátralévő idővel, egy szüneteltetett, amely a mentési pontjától folytatódik, egy másolás egy NAS-megosztásra, és egy feladat, amely a párosított PC-n fut](docs/screenshots/queue.png)

Az oldalon még:

- **Elsősorban kontrollerre:** a D-pad mozgat, a ✕ kiválaszt, a ○ visszalép,
  a □ megnyitja egy fájl beállításait, az L1/R1 a részek között vált, az
  L2/R2 a listákban lapoz. Kilépéshez nyomd meg egyszerre az L2 + R2 gombot,
  majd a ○-t (egy oldal nem tudja maga bezárni a konzol böngészőjét). A
  Beállításokban van kontrollerteszt. PC-n: nyílbillentyűk, Enter, Esc, S,
  Q/E és Page Up/Down.
- **Helyek:** bármely mappa hozzáadható a Helyekhez a Beállításaiból;
  megjelennek az oldalsávban, a Mentés ide listában és a párosított
  motorokon.
- **Átnevezés** (0.4.1-től): egy játék átnevezésekor a jelenlegi név mellett
  a címéből, címazonosítójából, verziójából és formátumából összeállított
  neveket is felajánl (`Title [ABCD12345].ffpfsc`, …); a kiválasztott név még
  szerkeszthető. A formátumokat te alakíthatod a **Beállítások → Átnevezési
  formátumok** menüpontban: mezők: `{title}` `{titleid}` `{contentid}`
  `{version}` `{format}` `{platform}` `{name}` (vagy `[titleid]`), módosítók,
  például `{title:upper}`, `{title:20}` vagy `{version:short}`, `<…>` egy
  olyan részhez, amely kimarad, ha egy érték hiányzik, és formátumonként,
  hogy mi legyen a szóközökkel (megtartás, eltávolítás, `_`, `.`, `-`) és a
  betűkkel (változatlanul, NAGYBETŰS, kisbetűs). A ShadowMount+ számára túl
  hosszú nevet (58 bájt a `.ffpfsc`, 63 az `.exfat`, a `.ffpkg` és az
  `.ffpfs` előtt) megjelöli, és egy rövidítettet tesz utána; a begépelt
  névnél rákérdez. A rejtett név (`.name`) rejtett marad.
- **Több fájl egyszerre** (0.4.2-től): □ egy fájlon → **Kijelölés…**, utána a
  ✕ bejelöli a sorokat, a □ megnyitja a bejelöltekre vonatkozó műveleteket,
  a ○ befejezi a kijelölést. Átalakítás (játékonként egy feladat; a már
  célformátumban lévő játék kimarad), Telepítés, AMPR-emulátor hozzáadása,
  Elrejtés a ShadowMount+ elől vagy Ismételt láthatóvá tétel a ShadowMount+
  számára, Átnevezés formátum alapján (előbb a régi → új név látszik), Küldés
  egy másik motornak, Másolás, Áthelyezés és Törlés; minden művelet azokat a
  fájlokat veszi, amelyekre vonatkozik.
- **Csomagok és elrejtés** (0.5.0-tól): az asset csomagokban lévő játékok a
  formátumuk után LZ4 vagy ZSTD jelölést kapnak. Az **Elrejtés** /
  **Megjelenítés** (átnevezés `.name`-re és vissza) bármely fájlon vagy
  mappán működik, a csomagokat is beleértve; az image-eknél megmarad az
  Elrejtés a ShadowMount+ elől / Ismételt láthatóvá tétel a ShadowMount+
  számára, amelyek újrakeresést is indítanak.
- **Hálózati szerverek** (Beállítások, 0.3.3-tól): a PC-n vagy NAS-on lévő
  játékok anélkül alakíthatók át, hogy előbb a konzolra másolnád őket, a
  következőkön keresztül:
  - **SMB** (Windows-megosztások, a legtöbb NAS): SMB 2 és 3 aláírással,
    ahogy a Windows 11 megköveteli, és SMB 3 titkosítással (AES-128-GCM vagy
    -CCM), ha a szerver vagy a megosztás megköveteli; felhasználónévvel és
    jelszóval, vagy vendégként. Ha a megosztást üresen hagyod, a szerver
    összes megosztása látszik.
  - **NFS** (NAS- és Linux-exportok): NFSv3, jelszó helyett a használandó
    uid:gid megadásával. Ha az exportot üresen hagyod, mindegyik látszik.
  - **WebDAV** (NAS, Nextcloud, rclone): HTTP vagy HTTPS, Basic vagy Digest
    bejelentkezés.
  - **FTP**: FTP vagy FTPS (explicit, vagy implicit a 990-es porton).

  A szerver csak tárolja a fájlokat: a motor a hálózaton keresztül olvassa
  a játékot, és maga végzi az átalakítást. Az exFAT-, FFPKG-, FFPFS- és
  mappakimenetek visszamenthetők a szerverre; az FFPFSC- és PKG-kimenetek
  előbb a konzolra kerülnek. Ha a **Titkosítás TLS-sel** be van kapcsolva,
  az FTP és a WebDAV TLS 1.2 vagy 1.3 felett megy (mbedTLS). Az otthoni
  szervereknek többnyire önaláírt tanúsítványuk van, ezért az Alembic első
  használatkor megbízik a szerver tanúsítványában, ahogy az SSH is: az első
  kapcsolódáskor megmutatja a SHA-256 ujjlenyomatát, hogy összevethesd a
  szerverével, és rákérdez; a későbbi kapcsolódásoknak ugyanazt kell
  mutatniuk, a megváltozott tanúsítványt pedig figyelmeztetéssel
  elutasítja. Ha azt szeretnéd, hogy egy PC végezze a munkát, inkább azon
  futtasd az Alembic motorját (lásd:
  [Munka több motorral](#munka-több-motorral)).
- **Erről a számítógépről:** egy PC böngészőjében megnyitva az oldal
  megoszthat egy fájlt arról a PC-ről, hogy a konzol átalakítsa.
- **ShadowMount+:** az új image-eket újraindítás nélkül is felismerheti, a
  források pedig átalakítás után elrejthetők előle.
- **Image-ek:** a ShadowMount+ által átvizsgált mappák összes image-e,
  játékmappája és csomagja (a rejtettek is), mappák szerint csoportosítva,
  játékikonok rácsaként vagy listaként (a fölötte lévő kapcsolóval; a
  Fájlokban is ugyanez van, és listaként indul). Mindent kínál, amit a
  Fájlok, kivéve a mappákba áthelyezést és másolást: Konvertálás, Telepítés,
  Átnevezés, az AMPR-emulátor, Elrejtés a ShadowMount+ elől, Küldés másik
  motorra, Törlés, Kijelölés többhöz egyszerre, és **Megjelenítés a
  Fájlokban**, amely megnyitja a mappát, amelyben van. Az APR-t használó
  játékoknál megmutatja, melyik AMPR-emulátorral fut mindegyik (a játékban
  vagy a backportjában), és hogy megjelent-e újabb build; az
  **AMPR-emulátorok frissítése** minden játék saját emulátorának legújabb
  buildjét teszi a helyére, az emulátor nélküli játékokba pedig a
  használatban lévőt. Mindkét emulátor minden hivatalos buildjét a GitHub
  által közzétett SHA-256 alapján ismeri fel.
  Az alkalmazások által a `/data/downloads` mappába (bármelyik
  almappájába) letöltött csomagok is megjelennek; számítógépen a rendszer
  Letöltések mappájában lévők.
  Utánuk a párosított motorok image-ei következnek, motoronként külön
  szakaszban (a Küldés, a Konvertálás és a többi úgy működik, mint a
  Fájlokban); AMPR-emulátorokat csak PS5-ön mutat és frissít, a saját
  játékaihoz.

  ![Image-ek: a ShadowMount+ mappáiban lévő játékok rácsban, két frissítendő AMPR-emulátor, egy emulátor nélküli játék és egy olyan, amelynek a buildjét az Alembic nem ismeri](docs/screenshots/images.png)
- **Komponensek:** azokat a külső fájlokat, amelyekre az Alembicnek szüksége
  van, de nem tartalmazza (az ampr_emu és ampr-emu-ng AMPR-emulátorok, a
  ProsperoPkgTool FPKG-kulcsfájljai, a LibOrbisPkg licenc- és
  PS4-csomagkulcsai), kérésre letölti a hivatalos forrásukból, és csak akkor
  tartja meg, ha a SHA-256 egyezik.
  A két AMPR-emulátor a projektjeik legújabb kiadását követi: a motor
  induláskor (vagy a **Frissítések keresése** gombra) megnézi a GitHubot,
  és ha az **Automatikus frissítés** be van kapcsolva (alapértelmezés),
  letölti az újabb buildet, amelyet a GitHub által megadott SHA-256 és a
  kiadás `SHA256SUMS` fájlja alapján ellenőriz. Egy emulátor van
  **használatban**: az, amelyet az átalakítások és az „AMPR-emulátor
  hozzáadása” a játékba tesz. A játék megtartja a kapott buildet; az
  „AMPR-emulátor hozzáadása” rajta felajánlja, hogy lecseréli a
  használatban lévőre. Az ampr-emu-ng a saját zstd-csomagjait is olvassa.
  **Offline:** töltsd le őket egy PC-n az alábbi linkekről (az
  emulátoroknál azokat a buildeket, amelyekkel az Alembicet legutóbb
  teszteltük; ugyanannak a projektnek bármelyik későbbi kiadása is
  működik), és másold őket a konzolra pontosan ezeken a neveken; a motor
  újraindítás nélkül felismeri őket:

  | Komponens | Letöltés | Hová kerüljön | SHA-256 |
  |---|---|---|---|
  | AMPR-emulátor (ampr_emu 0.4.2.1) | [libSceAmpr.sprx](https://github.com/drakmor/ampr_emu/releases/download/0.4.2.1/libSceAmpr.sprx) | `/data/alembic/runtime/libSceAmpr.sprx` | `69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a` |
  | AMPR-emulátor (ampr-emu-ng 0.2.0) | [libSceAmpr.sprx](https://github.com/quer3q/ampr-emu-ng/releases/download/v0.2.0/libSceAmpr.sprx) | `/data/alembic/runtime/ampr-emu-ng/libSceAmpr.sprx` | `7acb2173d9691b5dfb94ea4007660920b7250111c5fd7a86c6eb91b763ad3d76` |
  | FPKG-kulcsfájlok | [passcode.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/passcode.bin) | `/data/alembic/keys/passcode.bin` | `81e8283d582f522d8da34565ecd6ab39a38b3d3c0cacef078df97d2b285ec24e` |
  | | [mount_image.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/mount_image.bin) | `/data/alembic/keys/mount_image.bin` | `4510462812321cea26b4c0155e363955b16e31b89252b92510accad02eca5cd0` |
  | | [metadata_modulus.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/metadata_modulus.bin) | `/data/alembic/keys/metadata_modulus.bin` | `4a585ee47fea88d6b121d5e8afc9829438a27ee325cebec56a2ff537129a37c5` |
  | Licenc- és PS4-csomagkulcsok | [Keys.cs](https://raw.githubusercontent.com/maxton/LibOrbisPkg/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/Util/Keys.cs) | `/data/alembic/keys/ps4/Keys.cs` | `48b44dd03e653a5c0991b0a2975477802ea2639c4e6ea3270b2ff20a1856f8a7` |

  A kulcsfájloknak a rögzített változatoknak kell lenniük (ugyanazzal a
  SHA-256-tal), különben a csomagkimenet elutasítja őket: ezek állandók,
  ezért nem követnek semmit. A kézzel bemásolt emulátorbuild is működik, de
  a Komponensek között „módosítva” állapotúként látszik, amíg a motor maga
  le nem tölt egyet.
- **Motor** (Beállítások): a **Motor újraindítása** ugyanazt a verziót
  indítja el újra az ELF-betöltőn keresztül; az alkalmazás a **Motor
  indítása** lehetőséget is felajánlja, ha a motor leállt.
- **Frissítések** (0.2.3-tól): a motor induláskor újabb kiadást keres a
  GitHubon, és jelzi, ha talál; a **Beállítások → Frissítések** megmutatja a
  kiadási megjegyzéseket. A **Frissítés** letölti a payloadot, ellenőrzi a
  kiadás `SHA256SUMS` fájlja és a benne lévő verzió alapján, majd lecseréli
  azokat a példányokat, amelyekből a betöltőd indítja: a Payload Managerét
  (az új verzióra átnevezve, a nevet a payload `meta.json` fájljában is
  javítva) és a `ps5_autoloader` mappákét (ugyanazzal a névvel); a
  **Meghajtók keresése** máshol lévő példányokat is megtalál. Ezután az új
  motor elindul az ELF-betöltőn keresztül (9021-es port) és átveszi a
  munkát; a futó feladat, amely megőrzi az állapotát, folytatódik. Egy
  újabb `alembic.elf` kézi elküldése is lecseréli a futó motort. 0.3.0-tól
  ugyanez a frissítés az alkalmazás `.ffpkg` fájlját is lecseréli, ha a
  kiadásé eltér; ha az alkalmazás nyitva van, ez megvárja, amíg bezárod (a
  ProsperoStore-ból telepített alkalmazást az áruház frissíti). Ha az
  alkalmazás régebbi a motoránál, ezt jelzi, és felajánlja az **Alkalmazás
  frissítése** lehetőséget.
- **Egyéb motorok:** a kikapcsolt párosított motor offline állapotúként a
  listában marad, ahelyett hogy eltűnne.
- **Foglalt meghajtók:** az alkalmazás akkor is naprakészen tartja a
  várólistát, amikor a játékok címei és ikonjai egy olyan meghajtóról
  töltődnek be, amelyet egy futó feladat lefoglal.
- Nyelvek: English, Türkçe, Deutsch, Español, Français, Magyar, Русский, 日本語, 简体中文
  (a magyar fordítást isty1974 készítette).

Az állapot (várólista, beállítások, letöltött komponensek) a `/data/alembic`
mappában tárolódik.

## Munka több motorral

Futtasd az `alembic-engine` programot egy PC-n is, a konzolon futó payload
mellett. A párosítás után mindkét motor olvashatja és írhatja a másik
fájljait, és feladatokat adhat át neki.

1. A **Beállítások → Egyéb motorok** menüpontban kapcsold be mindkettőn a
   **Motorok keresése ezen a hálózaton** beállítást (alapértelmezés szerint
   ki van kapcsolva), vagy adj hozzá egyet a címe alapján.
2. Nyomd meg a **Párosítás** gombot a másik motor mellett. Az egy hatjegyű
   kódot mutat (PS5-ön értesítésként, PC-n az ablakában); írd be.
   A párosított motorok minden kérést az egyeztetett kulccsal írnak alá.
3. A másik motor meghajtói és Helyei megjelennek a **Fájlok** részen, az
   *Egyéb motorok* alatt, bemenetként és mentési helyként is.
4. Az **Átalakítás** ablakban az **Átalakítás ezen** választja ki, melyik
   motor végezze a munkát (az éppen online motorok közül). A forrás, az
   átalakító motor és a kimenet mind más-más motoron lehet; az adatok
   folyamatosan áramlanak köztük, előzetes másolat nélkül.

Egy feladat minden olyan motor várólistáján látszik, amelynek a fájljait
használja („on Living room PC”), és bármelyiken szüneteltethető,
folytatható, megszakítható vagy törölhető az előzményekből. Ha egy feladat
a kimenet ellenőrzése után törli a forrását, a motorok SHA-256
kivonatokat hasonlítanak össze, ahelyett hogy visszaküldenék a fájlokat. A
fájlok átalakítás nélkül is küldhetők a motorok között (Beállítások →
Küldés egy másik motornak…).

![Beállítások: egy párosított PC és egy, a hálózaton talált Mac, valamint két hálózati szerver: egy SMB-n elérhető NAS-megosztás és egy NFS-export](docs/screenshots/engines.png)

A különböző Alembic-verziójú motorok mindaddig együtt tudnak működni, amíg
ugyanúgy kommunikálnak egymással; ha ez egy kiadásban megváltozik, a
Beállítások megmutatja, melyik motort kell frissíteni, és addig
elutasítják egymást.

Magának az oldalnak nincs jelszava: a hálózatodon bárki megnyithatja. Egy
PC-s motort a `--listen 127.0.0.1` kapcsolóval indíts, ha csak arra a
számítógépre akarod korlátozni; ekkor más motorok sem érik el.

## PC-n

Minden kiadásban rendszerenként van egy csomag:
`alembic-<version>-linux-x64.tar.gz`, `-macos-arm64.tar.gz` (Apple
silicon) és `-windows-x64.zip`, mindegyikben az `alembic` és az
`alembic-engine`. A macOS-buildek nincsenek aláírva: böngészőből való
letöltés után futtasd egyszer a mappájukban az `xattr -d com.apple.quarantine alembic
alembic-engine` parancsot, különben a macOS nem engedi elindítani őket.

```sh
alembic convert game.pkg game.ffpfsc              # format from the extension (PS5 or PS4 package)
alembic convert ps4game.exfat game.pkg --ps4-keys Keys.cs   # a PS4 game back to a PS4 fake PKG
alembic convert game.exfat game/ --to folder      # unpack to a folder
alembic convert game/ game.exfat --lz4 --lz4-runtime libSceAmpr.sprx
alembic convert game/ game.exfat --zstd --zstd-runtime libSceAmpr.sprx   # ampr-emu-ng's
alembic ls game.ffpkg                             # list the files of any input
alembic same game.pkg game.ffpfsc                 # compare two inputs file by file
alembic game-info game.exfat                      # title ID, SDK and PS4 or PS5
alembic --help                                    # every command and option
```

Az `alembic-engine` ugyanaz a motor, mint a payload, ugyanazzal a
weboldallal (`http://localhost:5940`), PC-n való átalakításhoz vagy a
konzollal való párosításhoz. A `--root DIR` (többször is megadható)
választja ki, mely mappákat osztja meg (alapértelmezés szerint a saját
mappádat); az `alembic-engine --help` sorolja fel a többi kapcsolót.
A Beállítások → Frissítések jelzi, ha újabb kiadás jelent meg; PC-n töltsd
le, és cseréld le a fájlokat magad.

## Fordítás forráskódból

Követelmények: CMake 3.20+, C++20-fordító, zlib (a PS5-höz mellékelve),
valamint Node.js 20+ npm-mel a motor weboldalához (`src/engine/web`, Preact
és TypeScript, esbuilddel csomagolva; az első build ott lefuttatja az
`npm ci` parancsot).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/alembic_tests
```

A Windows-változat (`alembic.exe`, `alembic-engine.exe`) keresztfordítással
készül az [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) segítségével
macOS-en vagy Linuxon; a futtatható fájlok statikusan linkeltek, és
Windows 10-en vagy újabbon változtatás nélkül futnak. A
`tools/windows/smoke.sh` Windowson (Git Bash) ellenőrzi őket.

```sh
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DLLVM_MINGW=/path/to/llvm-mingw
cmake --build build-win -j
```

A PS5 payloadhoz LLVM 18 és a
[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) v0.44 kell:

```sh
# macOS: brew install llvm@18 · Debian/Ubuntu: apt install clang-18 lld-18 llvm-18
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk   # default: .refs/ps5-payload-sdk
tools/ps5/build.sh                                # → build-ps5/alembic.elf
```

A GitHub Actions minden olyan pushnál, amely nem csak a dokumentációt
módosítja, mindent lefordít és tesztel (lásd
[.github/workflows/build.yml](.github/workflows/build.yml)).
Minden futásnál külön letölthető a payload (`alembic-<version>.elf`) és egy
csomag (`alembic-<version>.zip`): a payload a `ps5/` mappában, az `alembic`
és az `alembic-engine` a `linux-x64/`, `macos-arm64/` és `windows-x64/`
(`.exe`) mappákban, a README-vel és a licencfájlokkal. A verzió a tag
(`v0.1.0`), ennek híján az ág és a commit (`main-a2fab12`). Egy `v*` tag
kiadásként teszi közzé a payloadot, a csomagot, egy-egy csomagot PC-s
rendszerenként, a konzolos alkalmazást (`.ffpkg` és `.zip`) és a
`SHA256SUMS` fájlt. 0.3.1-től minden kiadási fájlhoz build-tanúsítvány
tartozik: a `gh attestation verify <file> --repo fthdgn-gh/Alembic`
megmutatja, hogy ennek a tárolónak a workflow-ja készítette, és melyik
commitból.

### A weboldal

A motor oldala az `src/engine/web`: TypeScriptben írt Preact-komponensek
(az `src/pages/` mappában részenként egy mappa van; a többit az
`src/main.tsx` köti össze), a PS5 régebbi böngészőjéhez egyetlen
HTML-fájlba csomagolva, amelyet a motor beágyaz. Ha dolgozni akarsz rajta,
futtass egy motort, és hagyd, hogy az oldal minden változásnál újraépüljön:

```sh
build/alembic-engine --listen 127.0.0.1 &   # its API, on port 5940
cd src/engine/web
node build.mjs --serve                      # the page at http://127.0.0.1:5950
npx tsc --noEmit                            # type check (CI runs it too)
python3 ../../../tools/i18n_keys.py --check # every text translated
```

A `tools/screenshots/shoot.sh` készíti a README képernyőképeit egy kitalált
játékokat mutató helyettesítő motorral.

### Ellenőrzés a referenciaeszközökkel

A `tools/` mappában lévő szkriptek bájtra összevetik az Alembic kimenetét
azokkal az eszközökkel, amelyeket követ. Ehhez a referenciaprojekteknek ki
kell lenniük töltve a `.refs/` mappába (a git figyelmen kívül hagyja); a
[docs/NOTES.md](docs/NOTES.md) leírja, hogyan hozható létre újra.

```sh
tools/oracle/compare.sh        # exFAT, FFPKG, FFPFS, FFPFSC vs PS5PKGTool and MkPFS
tools/pkg-oracle/compare.sh    # FPKG reading and writing vs ProsperoPkgTool / LibProsperoPKG
tools/pkg-oracle/write_samples.sh  # FPKG writing vs ProsperoPkgTool on your own packages
tools/lz4-oracle/compare.sh    # LZ4 packs vs ampr_emu's ampr_pack.py
tools/zstd-oracle/compare.sh   # zstd packs vs ps5-dump-forge (builds it with cargo)
tools/fself-oracle/compare.sh  # fake-signed SELFs vs ampr-emu-ng's make_fself.py
tools/ps4-oracle/compare.sh    # PS4 packages vs LibOrbisPkg's pkg_build, pkg_extract and pkg_validate
```

## Ami nincs az Alembicben

- Sem ebben a tárolóban, sem a binárisaiban nincs Oodle, `libScePubTools`,
  zárt forrású csomagkönyvtár vagy konzolkulcs. Az FPKG-kimenet a
  ProsperoPkgTool nyilvánosan közzétett kulcsfájljait használja; a
  PS4-csomagkimenet a LibOrbisPkg `Keys.cs` fájlját (ebben van a debug
  licenckulcs is, amelyet csak arra használ, hogy licencet generáljon egy
  olyan játékhoz, amelynek nincs, legyen az PS5 vagy PS4). A motor
  mindkettőt kérésre tölti le.
- Nincsenek külső binárisok: az AMPR-emulátorok `libSceAmpr.sprx` fájlja az
  egyes projektek hivatalos GitHub-kiadásából töltődik le, és a vele együtt
  közzétett SHA-256 alapján ellenőrződik.

## Köszönet

Az Alembic a PS5 homebrew közösség munkájára épül. Köszönet:

- **pearlxcore**-nak a [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool)
  (exFAT, PFSC és FFPFSC; a referencia, amellyel az Alembic bájtra egyezik)
  és a [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool)
  (debug csomagok olvasása és írása, valamint a kulcsfájljai) miatt.
- **SadykovIV**-nek a [PS4pkg_to_ffpfsc](https://github.com/SadykovIV/PS4pkg_to_ffpfsc)
  miatt (hogyan kell kinéznie egy PS4-játék image-ének a ShadowMount+
  számára, a `param.json` fájlja).
- **SvenGDK**-nak az [UFS2Tool](https://github.com/SvenGDK/UFS2Tool) (az
  FFPKG olvasó és író) és a [LibProsperoPKG](https://github.com/SvenGDK/LibProsperoPKG)
  (tesztcsomagok) miatt.
- **drakmor**-nak az [ampr_emu](https://github.com/drakmor/ampr_emu) (az AMPR
  index, az LZ4 asset csomagok és az őket olvasó futtatókörnyezet) és a
  [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) (a betöltő,
  amelyhez az Alembic image-ei készülnek, valamint a HTTPS-letöltő kódja)
  miatt.
- **quer3q**-nak az [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng) (a
  második AMPR-emulátor és a zstd-csomagjai) és a
  [ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge) (a zstd-csomagíró,
  amelyet az Alembic követ, és amellyel ellenőrzi magát) miatt.
- **PSBrew**-nak a [MkPFS](https://github.com/PSBrew/MkPFS) (a PFS-író)
  miatt.
- **maxton**-nak a [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) (PS4
  csomagok: az író, amelyet az Alembic követ, a kulcsai, és az eszköz,
  amellyel ellenőrizzük) miatt.
- **theghostonline**-nak a
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression)
  (LZ4-csomagprofilok és a motor-plusz-oldal ötlet) miatt.
- **danthespal**-nak a [MkPFS.CSharp](https://github.com/danthespal/MkPFS.CSharp)
  mellett közzétett LZ4-játékprofilokért.
- **ps5-payload-dev**-nek a [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk),
  a [shsrv](https://github.com/ps5-payload-dev/shsrv) (csomagtelepítés) és a
  [websrv](https://github.com/ps5-payload-dev/websrv) (a Média lapon lévő
  parancsikon) miatt, valamint az
  [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager)
  és az [itsPLK/ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager)
  projekteknek.
- **blackbearreloaded**-nek a [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui)
  miatt, amelynek a dizájnját az oldal követi.
- A Microsoft [nyílt specifikációinak](https://learn.microsoft.com/openspecs/)
  (SMB2/3, NTLM, SPNEGO, DCE/RPC és a megosztáslista), valamint az NFSv3-ra,
  az ONC RPC-re és a WebDAV-ra vonatkozó IETF RFC-knek, amelyek alapján a
  hálózati kliensek készültek.
- A [zlib](https://zlib.net), az [LZ4](https://github.com/lz4/lz4) és a
  [Zstandard](https://github.com/facebook/zstd) könyvtárnak, amelyek a
  motorral együtt érkeznek, valamint a [Preact](https://github.com/preactjs/preact)
  keretrendszernek és a [signals](https://github.com/preactjs/signals)
  csomagjának, amelyek az oldalába vannak csomagolva.

A képernyőképeken látható játékok, motorok és címek kitaláltak (a
`tools/screenshots/shoot.sh` egy helyettesítő motorral rajzolja meg őket).

## Licenc

GPL-3.0-or-later ([LICENSE](LICENSE)). Az Alembic több GPL, LGPL, MIT és BSD
projektből vesz át kódot; a [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
mindegyiket felsorolja, azzal együtt, hogy mi származik belőle, és mi a
licence.
