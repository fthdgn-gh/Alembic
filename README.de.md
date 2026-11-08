# Alembic

[English](README.md) · [Türkçe](README.tr.md) · **Deutsch** · [Español](README.es.md) · [Français](README.fr.md) · [Magyar](README.hu.md) · [Русский](README.ru.md) · [日本語](README.ja.md) · [简体中文](README.zh.md)

*Dies ist eine Übersetzung der [englischen README](README.md); wenn beide voneinander abweichen, gilt die englische.*

![Die Seite von Alembic im Browser der PS5: eine Bibliothek mit Spielen in mehreren Formaten, links Laufwerke, eine NAS-Freigabe, ein NFS-Export, ein FTPS-Server und ein gekoppelter PC](docs/screenshots/files.png)

Alembic wandelt PS5- und PS4-Spiele zwischen den Formaten um, die
Homebrew-Loader verwenden: Pakete (PS5-Debug-FPKG, PS4-Fake-PKG),
Dump-Ordner, exFAT-Images, FFPKG (UFS2), FFPFS (PFS) und FFPFSC
(komprimiertes PFS). Es läuft **auf der Konsole** als App auf dem
Startbildschirm (ab 0.3.0) oder als Payload mit einer Webseite, beides mit dem
Controller bedienbar, und auf einem PC (Windows, macOS, Linux) als
Kommandozeilenwerkzeug oder mit derselben Engine und Seite. Engines in deinem
Netzwerk lassen sich koppeln und arbeiten zusammen: Ein Paket, das auf deinem
PC liegt, wird vom Prozessor des PCs umgewandelt und landet direkt im Speicher
der Konsole. Spiele auf einem PC oder NAS lassen sich außerdem über SMB, NFS,
WebDAV oder FTP lesen und dort speichern.

> In Arbeit. Sichere alles, was du umwandelst, bis du das Ergebnis auf deiner
> Konsole geprüft hast.

**Bisher auf einer Konsole ausprobiert:** FFPFSC-Images aus Dumps und aus
Paketen laufen über ShadowMount+; Spiele, die den AMPR-Emulator brauchen,
starten, sobald er hinzugefügt ist; Konsole und PC koppeln sich und wandeln
füreinander um; **Installieren** übergibt ein Debug-Paket an den Installer der
Konsole, auch Pakete, die Alembic aus einem Dump erstellt (unkomprimiert oder
Kraken, mit einer erzeugten Debug-Lizenz, also ohne Schloss am Titel); eine
pausierte Umwandlung geht nach einem Update der Engine weiter; die Engine
aktualisiert sich selbst aus einem Release (geladen über Payload Manager); die
Konsolen-App startet aus ShadowMount+, startet die Engine und startet sie neu
und steuert eine Umwandlung auf einem gekoppelten PC; ein Paket wurde von einer
SMB-Freigabe umgewandelt; ein Spiel in zstd-Paketen (ein 160-GB-Paket, von
einer PC-Engine mit Stufe 5 in ein 96-GB-exFAT gepackt, das auf die Konsole
geschrieben wurde) startet mit ampr-emu-ng.
PS4-Spiele, die aus ihren Paketen umgewandelt wurden, starten aus Images über
ShadowMount+ 1.7beta8 (es zeigt bei ihnen „Trophy registration failed“: ein
Problem von ShadowMount+,
[#97](https://github.com/drakmor/ShadowMountPlus/issues/97)).
**Noch nicht:** NFS, WebDAV, SMB-Verschlüsselung, FTPS und HTTPS von einer
Konsole aus sowie echte NAS-Geräte und Windows-Freigaben (alle wurden auf einem
PC gegen Samba, rclone, WsgiDAV und pyftpdlib geprüft); das Starten eines aus
einem Alembic-Paket installierten Spiels (installiert wurde, gestartet noch
nicht); ein von Alembic erstelltes PS4-Paket; PS4-Updates (Patch-Pakete) und
DLC. LZ4-Asset-Pakete funktionieren bei manchen Spielen und bei anderen nicht.

## Was es kann

| Von ↓ / Nach → | Ordner | exFAT | FFPKG (UFS2) | FFPFS (PFS) | FFPFSC | Paket |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| Dump-Ordner | – | ✓ | ✓ | ✓ | ✓ | ✓ |
| exFAT | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPKG (UFS2) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFS (PFS) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFSC | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| PS5-FPKG (Debug) | ✓ | ✓ | ✓ | ✓ | ✓ | `--rebuild` |
| PS4-PKG (Fake) | ✓ | ✓ | ✓ | ✓ | ✓ | `--rebuild` |

Welches Paket aus einem Spiel wird, hängt vom Spiel ab: Ein PS5-Spiel
(`sce_sys/param.json`) wird ein PS5-Debug-FPKG, ein PS4-Spiel
(`sce_sys/param.sfo`) ein PS4-Fake-PKG. Seite und App kennzeichnen jedes Spiel
mit einer Plakette **PS5** (weiß) oder **PS4** (schwarz).

- **PS4-Spiele** (ab 0.3.2): Fake-PKGs mit dem üblichen Passcode aus lauter
  Nullen (aus älteren wie neueren Paketierwerkzeugen) öffnen sich wie
  PS5-Pakete, als die Dateien des Spiels plus `sce_sys` (`param.sfo`, Icons,
  Trophäen, Lizenz) aus dem Paket, und lassen sich in jedes Image umwandeln.
  Images und Ordner eines PS4-Spiels bekommen die `sce_sys/param.json`, die
  ShadowMount+ zum Starten braucht (aus `param.sfo` erzeugt, so wie
  [PS4pkg_to_ffpfsc](https://github.com/SadykovIV/PS4pkg_to_ffpfsc) sie
  erzeugt); auf einer Konsole mit ShadowMount+ 1.7beta8 ausprobiert. Ein
  PS4-Spiel in beliebigem Format lässt sich zurück in ein PS4-Fake-PKG
  umwandeln, gebaut wie mit `pkg_build` von
  [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg): Die Lizenz des Spiels
  bleibt erhalten (nur erzeugt, wenn sie fehlt), und die PlayGo-Dateien werden
  neu erstellt. Ein Paket, das aus einem beliebigen Image davon neu gebaut
  wird, ist Byte für Byte dasselbe. Die Schlüssel stammen aus LibOrbisPkg
  (Komponenten, oder `--ps4-keys FILE` auf einem PC). AMPR-Emulator und
  LZ4-Pakete gibt es nur für PS5-Spiele. Für welche Konsole ein Spiel ist,
  ergibt sich aus seiner Titel-ID (`CUSA`, `PPSA`), sonst aus dem SDK, mit dem
  seine `eboot.bin` gebaut wurde (Homebrew); `alembic game-info` zeigt beides.
  Noch nicht: Updates und DLC sowie Pakete mit einem anderen Passcode als
  lauter Nullen.
- **Streaming:** Jede Eingabe wird als Dateibaum gelesen und direkt in die
  Ausgabe geschrieben, ohne temporäre Kopien; ein 100-GB-Paket braucht also
  nur den Platz für das Ergebnis.
- **Byte-identisch** zu den Referenzwerkzeugen: exFAT, FFPKG und FFPFSC
  stimmen mit [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool) überein;
  FFPFS mit [MkPFS](https://github.com/PSBrew/MkPFS) (`pack folder --raw
  --no-compress`); die FPKG-Ausgabe mit
  [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool)
  (`img_create`, unkomprimiert oder `--compression kraken`, mit demselben
  festen Seed); PS4-Pakete mit `pkg_build` von
  [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg), auch beim Neubau eines
  Pakets aus dem Projekt, das dessen `pkg_makegp4` extrahiert.
- **LZ4-Asset-Pakete** für die PackedStdio-Laufzeit von drakmors
  [ampr_emu](https://github.com/drakmor/ampr_emu) (`--lz4`), byte-identisch zu
  dessen `ampr_pack.py`, mit Spielprofilen aus
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression).
- **zstd-Pakete** für quer3qs [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng)
  (`--zstd`, oder „In zstd-Pakete packen“ beim Umwandeln), byte-identisch zu
  denen von [ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge):
  kleiner als LZ4 und schneller erstellt (26 GiB Spieldateien wurden auf einem
  Mac in 33 s zu 11,1 GiB, gegenüber 17,5 GiB in 77 s mit LZ4). Das Spiel
  bekommt die `libSceAmpr.sprx` von ampr-emu-ng (aus Komponenten); schalte in
  ShadowMount+ „update emulators“ aus, damit sie nicht gegen eine getauscht
  wird, die diese Pakete nicht lesen kann. Stufen 1, 5 (Standard) und 19. Auf
  einer PS5 ausprobiert. Ein gepacktes Spiel kann in Umwandeln zwischen LZ4-
  und zstd-Paketen wechseln („Zu zstd-Paketen wechseln“); die alten Pakete
  werden dabei direkt in die neuen dekodiert.
- **AMPR-Emulator:** Spiele, die den APR-Dateileser der Konsole nutzen,
  brauchen die `libSceAmpr.sprx` eines Emulators, um aus einem Image oder
  Ordner zu starten: drakmors ampr_emu oder quer3qs ampr-emu-ng (Komponenten
  zeigt, welcher in Verwendung ist). Alembic fügt ihn beim Umwandeln hinzu oder
  zu einem Spiel, das du schon hast; bei einem Spiel, das bereits einen hat,
  bietet „AMPR-Emulator hinzufügen“ den Wechsel zum anderen an.
- **Fake-Signierung:** Ein PS5-Spiel, dessen ausführbare Dateien einfache
  (unsignierte) ELF-Dateien sind, startet nie. Umwandlungen machen daraus
  unterwegs fake-signierte SELFs, und Umwandeln sagt das vorher: bei einem
  Image oder Ordner byte-identisch zu `make_fself.py` von ampr-emu-ng, bei
  einem Paket zu `img_create` von ProsperoPkgTool.
- **Schnell:** Komprimierung und Dekodierung von Paketen nutzen alle Kerne;
  AES, CRC32C und SHA-256 nutzen die eigenen Befehle der CPU, wo sie welche
  hat; standardmäßig zlib-Stufe 6 (Stufe 9 für exakt die Ausgabe von
  PS5PKGTool).

## Auf der PS5

Die Arbeit macht die Engine (`alembic.elf`), eine Payload, die weiterläuft,
nachdem du geschlossen hast, womit du sie bedienst: die **Alembic-App** auf dem
Startbildschirm oder ihre Webseite im Browser der Konsole (über eine
Verknüpfung, die sie im Tab Medien anlegt) oder in einem beliebigen Browser in
deinem Netzwerk unter `http://<console-ip>:5940`. App und Seite zeigen
dasselbe und funktionieren gleich.

![Die Alembic-App auf der PS5: ein Homebrew-Ordner mit Spielen in mehreren Formaten, links Laufwerke, eine NAS-Freigabe, ein NFS-Export und ein gekoppelter PC](docs/screenshots/app.png)

**Die App** (`PPSA99594`, ab 0.3.0), auf eine von zwei Arten installiert:

1. **Aus ProsperoStore** (am einfachsten): Alembic ist in
   [ProsperoStore](https://github.com/darkxex/ProsperoStore) gelistet, dem
   Konsolen-Store für native Homebrew
   ([Seite von Alembic](https://prosperostore.net/app/PPSA99594/)).
   Installiere es dort; der Store aktualisiert es auch.
2. **Als Image oder Ordner**, über
   [ShadowMount+](https://github.com/drakmor/ShadowMountPlus): Kopiere
   `alembic-app-<version>.ffpkg` aus den Releases nach `/data/homebrew/` (oder
   entpacke `alembic-app-<version>.zip` dorthin: den Ordner `PPSA99594`). Ihre
   Kachel erscheint nach dem nächsten Scan von ShadowMount+ auf dem
   Startbildschirm.

Starte sie. Wenn keine Engine antwortet, sendet sie ihre eigene an den
ELF-Loader auf Port 9021 (eine App ohne eigene Engine lädt zuerst die Engine
ihrer eigenen Version aus dem GitHub-Release herunter, geprüft gegen
`SHA256SUMS`).

**Ohne die App:**

1. Sende `alembic-<version>.elf` (aus den Releases) an den ELF-Loader deiner
   Konsole.
2. Öffne Alembic über den Tab Medien (oder die Adresse oben).
3. Wähle in **Dateien** ein Spiel auf dem internen Speicher, auf USB, auf einem
   Netzwerkserver (SMB, NFS, WebDAV oder FTP) oder auf einem Laufwerk einer
   gekoppelten Engine (eine Plakette unterscheidet PS5- und PS4-Spiele), dann
   **Umwandeln**, **Installieren** (Pakete), **Kopieren** oder
   **Verschieben**. Wandelst du ein PS5-Spiel in ein Paket um, kann es danach
   an den Installer der Konsole übergeben werden (**Nach Abschluss
   installieren**); das Paket wird dann gelöscht, sobald der Installer es
   gelesen hat, außer **Paket behalten** ist an. Bis dahin braucht es also
   Platz.
4. Aufträge laufen nacheinander in der **Warteschlange**: pausieren,
   fortsetzen (lange Aufträge machen an einem Sicherungspunkt weiter), neu
   ordnen, abbrechen. Ist einer fertig, bekommst du eine Benachrichtigung.

![Der Dialog Umwandeln: Zielformate, Kompressionsstufe, die Schalter für LZ4- und zstd-Pakete, der Schalter für den AMPR-Emulator und der Ausgabepfad auf einem gekoppelten PC](docs/screenshots/convert.png)

![Die Warteschlange: eine laufende Umwandlung mit Geschwindigkeit und Restzeit, eine pausierte, die am Sicherungspunkt weitermacht, eine Kopie auf eine NAS-Freigabe und ein Auftrag auf dem gekoppelten PC](docs/screenshots/queue.png)

Außerdem auf der Seite:

- **Controller zuerst:** Das Steuerkreuz bewegt, ✕ wählt, ○ geht zurück, □
  öffnet die Optionen einer Datei, L1/R1 wechseln den Bereich, und L2/R2
  blättern durch Listen. Zum Verlassen drückst du L2 + R2 zusammen, dann ○
  (eine Seite kann den Browser der Konsole nicht selbst schließen). In den
  Einstellungen gibt es einen Controller-Test. Auf einem PC: Pfeiltasten,
  Enter, Esc, S, Q/E und Bild auf/ab.
- **Orte:** Füge einen beliebigen Ordner über seine Optionen zu den Orten
  hinzu; sie erscheinen in der Seitenleiste, unter Speichern unter und auf
  gekoppelten Engines.
- **Umbenennen** (ab 0.4.1): Beim Umbenennen eines Spiels werden neben dem
  aktuellen Namen Namen aus Titel, Titel-ID, Version und Format angeboten
  (`Title [ABCD12345].ffpfsc`, …); der gewählte lässt sich noch bearbeiten.
  Die Formate legst du selbst in **Einstellungen → Namensformate** fest:
  Felder `{title}` `{titleid}` `{contentid}` `{version}` `{format}`
  `{platform}` `{name}` (oder `[titleid]`), Modifikatoren wie
  `{title:upper}`, `{title:20}` oder `{version:short}`, `<…>` für einen Teil,
  der wegfällt, wenn ein Wert fehlt, und je Format, was mit Leerzeichen
  passiert (behalten, entfernen, `_`, `.`, `-`) und mit Buchstaben (wie sie
  sind, GROSS, klein). Ein Name, der für ShadowMount+ zum Einhängen zu lang
  ist (58 Byte vor `.ffpfsc`, 63 vor `.exfat`, `.ffpkg` und `.ffpfs`), wird
  markiert und von einem gekürzten gefolgt; bei einem eingetippten wird
  nachgefragt. Ein versteckter Name (`.name`) bleibt versteckt.
- **Mehrere Dateien auf einmal** (ab 0.4.2): □ auf einer Datei →
  **Auswählen…**, dann hakt ✕ Zeilen an, □ öffnet die Aktionen für die
  angehakten, und ○ beendet die Auswahl. Umwandeln (ein Auftrag pro Spiel,
  ein Spiel, das schon im Zielformat ist, wird ausgelassen), Installieren,
  AMPR-Emulator hinzufügen, Vor ShadowMount+ verbergen oder Für ShadowMount+
  wieder sichtbar machen, Umbenennen mit einem Format (alt → neu wird zuerst
  gezeigt), An eine andere Engine senden, Kopieren, Verschieben und Löschen;
  jede Aktion nimmt die Dateien, für die sie gilt.
- **Pakete und Ausblenden** (ab 0.5.0): Spiele in Asset-Paketen tragen nach
  ihrem Format eine Markierung LZ4 oder ZSTD. **Ausblenden** / **Einblenden**
  (Umbenennen zu `.name` und zurück) funktionieren bei jeder Datei und jedem
  Ordner, Pakete eingeschlossen; Images behalten Vor ShadowMount+ verbergen /
  Für ShadowMount+ wieder sichtbar machen, die außerdem einen neuen Scan
  auslösen.
- **Netzwerkserver** (Einstellungen, ab 0.3.3): Spiele auf einem PC oder NAS
  lassen sich umwandeln, ohne sie vorher auf die Konsole zu kopieren, über
  - **SMB** (freigegebene Windows-Ordner, die meisten NAS-Geräte): SMB 2 und 3
    mit Signierung, wie Windows 11 sie verlangt, und SMB-3-Verschlüsselung
    (AES-128-GCM oder -CCM), wenn Server oder Freigabe sie verlangen;
    Benutzer und Passwort oder Gast-Anmeldung. Lass die Freigabe leer, um
    alle Freigaben des Servers zu sehen.
  - **NFS** (NAS- und Linux-Exporte): NFSv3, mit der uid:gid, die statt eines
    Passworts verwendet wird. Lass den Export leer, um alle zu sehen.
  - **WebDAV** (NAS, Nextcloud, rclone): HTTP oder HTTPS, Anmeldung per Basic
    oder Digest.
  - **FTP**: FTP oder FTPS (explizit, oder implizit auf Port 990).

  Der Server speichert nur die Dateien: Die Engine liest das Spiel über das
  Netzwerk und wandelt selbst um. Ausgaben als exFAT, FFPKG, FFPFS und Ordner
  lassen sich zurück auf den Server speichern; FFPFSC- und PKG-Ausgaben landen
  zuerst auf der Konsole. Mit **Mit TLS verschlüsseln** laufen FTP und WebDAV
  über TLS 1.2 oder 1.3 (mbedTLS). Server zu Hause haben meist selbstsignierte
  Zertifikate, deshalb vertraut Alembic dem Zertifikat eines Servers bei der
  ersten Verwendung, wie SSH: Die erste Verbindung zeigt seinen
  SHA-256-Fingerabdruck zum Vergleich mit dem des Servers und fragt nach;
  spätere Verbindungen müssen denselben zeigen, und ein geändertes Zertifikat
  wird mit einer Warnung abgelehnt. Soll ein PC die Arbeit machen, lass
  stattdessen die Engine von Alembic darauf laufen (siehe
  [Mit mehreren Engines arbeiten](#mit-mehreren-engines-arbeiten)).
- **Von diesem Computer:** Im Browser eines PCs geöffnet, kann die Seite eine
  Datei von diesem PC freigeben, damit die Konsole sie umwandelt.
- **ShadowMount+:** Neue Images können ohne Neustart erkannt werden, und
  Quellen lassen sich nach dem Umwandeln vor ihm verbergen.
- **Images:** alle Images, Spielordner und Pakete in den Ordnern, die
  ShadowMount+ durchsucht (auch versteckte), nach Ordner gruppiert, als Raster
  aus Spielsymbolen oder als Liste (der Schalter darüber; Dateien hat denselben
  und startet als Liste). Es bietet alles, was Dateien bietet, außer dem
  Verschieben und Kopieren in Ordner: Konvertieren, Installieren, Umbenennen,
  den AMPR-Emulator, Vor ShadowMount+ verstecken, An eine andere Engine
  senden, Löschen, Auswählen für mehrere auf einmal und **In Dateien zeigen**,
  das den Ordner öffnet, in dem es liegt. Bei Spielen, die das APR nutzen,
  zeigt es den AMPR-Emulator, mit dem jedes läuft (im Spiel oder in seinem
  Backport), und ob ein neuerer Build erschienen ist; **AMPR-Emulatoren
  aktualisieren** setzt den neuesten Build des jeweils eigenen Emulators ein,
  bei Spielen ohne Emulator den verwendeten. Jeder offizielle Build beider
  Emulatoren wird an dem SHA-256 erkannt, den GitHub für ihn veröffentlicht.
  Pakete, die Apps nach `/data/downloads` laden (in beliebige Ordner
  darunter), werden ebenfalls aufgeführt.

  ![Images: die Spiele in den Ordnern von ShadowMount+ als Raster, zwei AMPR-Emulatoren zu aktualisieren, ein Spiel ohne Emulator und eines mit einem Build, den Alembic nicht kennt](docs/screenshots/images.png)
- **Komponenten:** Dateien von Dritten, die Alembic braucht, aber nicht
  mitliefert (die AMPR-Emulatoren ampr_emu und ampr-emu-ng, die FPKG-Schlüsseldateien
  von ProsperoPkgTool, die Lizenz- und PS4-Paketschlüssel von LibOrbisPkg),
  werden auf Wunsch von ihrer offiziellen Quelle heruntergeladen und nur
  behalten, wenn ihr SHA-256 stimmt.
  Die beiden AMPR-Emulatoren folgen den neuesten Releases ihrer Projekte: Die
  Engine fragt beim Start (oder bei **Nach Updates suchen**) bei GitHub nach
  und lädt, wenn **Automatisch aktualisieren** an ist (Standard), einen
  neueren Build herunter, geprüft gegen den SHA-256, den GitHub und die
  `SHA256SUMS` des Releases dafür angeben. Ein Emulator ist **in Verwendung**:
  der, den Umwandlungen und „AMPR-Emulator hinzufügen“ in ein Spiel legen. Ein
  Spiel behält den Build, den es bekommen hat; „AMPR-Emulator hinzufügen“
  bietet bei ihm an, ihn durch den in Verwendung zu ersetzen. ampr-emu-ng
  liest außerdem seine eigenen zstd-Pakete.
  **Offline:** Lade sie auf einem PC über die Links unten herunter (bei den
  Emulatoren die Builds, mit denen Alembic zuletzt getestet wurde; jedes
  spätere Release desselben Projekts geht auch) und kopiere sie unter genau
  diesen Namen auf die Konsole; die Engine erkennt sie ohne Neustart:

  | Komponente | Download | Ablegen unter | SHA-256 |
  |---|---|---|---|
  | AMPR-Emulator (ampr_emu 0.4.2.1) | [libSceAmpr.sprx](https://github.com/drakmor/ampr_emu/releases/download/0.4.2.1/libSceAmpr.sprx) | `/data/alembic/runtime/libSceAmpr.sprx` | `69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a` |
  | AMPR-Emulator (ampr-emu-ng 0.2.0) | [libSceAmpr.sprx](https://github.com/quer3q/ampr-emu-ng/releases/download/v0.2.0/libSceAmpr.sprx) | `/data/alembic/runtime/ampr-emu-ng/libSceAmpr.sprx` | `7acb2173d9691b5dfb94ea4007660920b7250111c5fd7a86c6eb91b763ad3d76` |
  | FPKG-Schlüsseldateien | [passcode.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/passcode.bin) | `/data/alembic/keys/passcode.bin` | `81e8283d582f522d8da34565ecd6ab39a38b3d3c0cacef078df97d2b285ec24e` |
  | | [mount_image.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/mount_image.bin) | `/data/alembic/keys/mount_image.bin` | `4510462812321cea26b4c0155e363955b16e31b89252b92510accad02eca5cd0` |
  | | [metadata_modulus.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/metadata_modulus.bin) | `/data/alembic/keys/metadata_modulus.bin` | `4a585ee47fea88d6b121d5e8afc9829438a27ee325cebec56a2ff537129a37c5` |
  | Lizenz- und PS4-Paketschlüssel | [Keys.cs](https://raw.githubusercontent.com/maxton/LibOrbisPkg/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/Util/Keys.cs) | `/data/alembic/keys/ps4/Keys.cs` | `48b44dd03e653a5c0991b0a2975477802ea2639c4e6ea3270b2ff20a1856f8a7` |

  Schlüsseldateien müssen die festgelegten sein (gleicher SHA-256), sonst
  lehnt die Paketausgabe sie ab: Sie sind Konstanten und folgen daher nichts.
  Ein von Hand kopierter Emulator-Build funktioniert trotzdem, wird in
  Komponenten aber als „geändert“ angezeigt, bis die Engine selbst einen
  herunterlädt.
- **Engine** (Einstellungen): **Engine neu starten** startet dieselbe Version
  über den ELF-Loader erneut; die App bietet außerdem **Engine starten** an,
  wenn sie gestoppt ist.
- **Updates** (ab 0.2.3): Die Engine sucht beim Start auf GitHub nach einem
  neueren Release und meldet es; **Einstellungen → Updates** zeigt die
  Hinweise dazu. **Aktualisieren** lädt die Payload herunter, prüft sie gegen
  die `SHA256SUMS` des Releases und die in ihr eingetragene Version und
  ersetzt die Kopien, aus denen dein Loader sie startet: die von Payload
  Manager (auf die neue Version umbenannt, mit dem in der `meta.json` dieser
  Payload korrigierten Namen) und die in `ps5_autoloader`-Ordnern (gleicher
  Name); **Laufwerke durchsuchen** findet Kopien an anderen Orten. Dann
  startet die neue Engine über den ELF-Loader (Port 9021) und übernimmt; ein
  laufender Auftrag, der seinen Fortschritt behält, geht weiter. Auch eine von
  Hand gesendete neuere `alembic.elf` ersetzt eine laufende Engine. Ab 0.3.0
  ersetzt dasselbe Update auch das `.ffpkg` der App, wenn das des Releases
  anders ist; ist die App offen, wartet das, bis sie geschlossen wird (eine
  aus ProsperoStore installierte App aktualisiert der Store). Eine App, die
  älter als ihre Engine ist, meldet das und bietet **App aktualisieren** an.
- **Andere Engines:** Eine gekoppelte Engine, die ausgeschaltet ist, bleibt als
  offline gelistet, statt zu verschwinden.
- **Ausgelastete Laufwerke:** Die App hält die Warteschlange aktuell, während
  Spieltitel und Icons von einem Laufwerk laden, das ein laufender Auftrag
  beschäftigt.
- Sprachen: English, Türkçe, Deutsch, Español, Français, Magyar, Русский, 日本語, 简体中文
  (Magyar übersetzt von isty1974).

Der Zustand (Warteschlange, Einstellungen, heruntergeladene Komponenten) liegt
in `/data/alembic`.

## Mit mehreren Engines arbeiten

Lass `alembic-engine` auf einem PC laufen und dazu die Payload auf der
Konsole. Sind sie gekoppelt, kann jede Engine die Dateien der anderen lesen und
schreiben und ihr Aufträge übergeben.

1. Schalte in **Einstellungen → Andere Engines** auf beiden **Engines in
   diesem Netzwerk finden** ein (standardmäßig aus), oder füge eine über ihre
   Adresse hinzu.
2. Drücke **Koppeln** neben der anderen Engine. Sie zeigt einen sechsstelligen
   Code (auf der PS5 als Benachrichtigung, auf einem PC in ihrem Fenster); gib
   ihn ein. Gekoppelte Engines signieren jede Anfrage mit dem Schlüssel, auf
   den sie sich geeinigt haben.
3. Die Laufwerke und Orte der anderen Engine erscheinen in **Dateien** unter
   *Andere Engines*, als Eingaben und als Speicherorte.
4. In **Umwandeln** wählt **Umwandeln auf** die Engine, die die Arbeit macht
   (unter denen, die gerade online sind). Quelle, umwandelnde Engine und
   Ausgabe können jeweils auf einer anderen Engine liegen; die Daten fließen
   direkt zwischen ihnen, ohne vorher eine Kopie anzulegen.

Ein Auftrag erscheint in der Warteschlange jeder Engine, deren Dateien er nutzt
(„auf PC im Wohnzimmer“), und kann auf jeder davon pausiert, fortgesetzt,
abgebrochen oder aus dem Verlauf entfernt werden. Löscht ein Auftrag nach der
Prüfung der Ausgabe seine Quelle, vergleichen die Engines SHA-256-Prüfsummen,
statt die Dateien zurückzuschicken. Dateien lassen sich auch ohne Umwandeln
zwischen Engines senden (Optionen → An eine andere Engine senden).

![Einstellungen: ein gekoppelter PC und ein im Netzwerk gefundener Mac sowie zwei Netzwerkserver: eine NAS-Freigabe über SMB und ein NFS-Export](docs/screenshots/engines.png)

Engines verschiedener Alembic-Versionen arbeiten zusammen, solange sie auf
dieselbe Weise miteinander sprechen; ändert sich das in einem Release, zeigen
die Einstellungen, welche Engine aktualisiert werden muss, und bis dahin
lehnen sie einander ab.

Die Seite selbst hat kein Passwort: Jeder in deinem Netzwerk kann sie öffnen.
Starte eine PC-Engine mit `--listen 127.0.0.1`, um sie auf diesen Computer zu
beschränken; andere Engines erreichen sie dann nicht.

## Auf einem PC

Jedes Release enthält ein Paket pro System: `alembic-<version>-linux-x64.tar.gz`,
`-macos-arm64.tar.gz` (Apple Silicon) und `-windows-x64.zip`, jeweils mit
`alembic` und `alembic-engine`. Die macOS-Builds sind nicht signiert: Führe
nach einem Download über einen Browser einmal `xattr -d com.apple.quarantine alembic
alembic-engine` in ihrem Ordner aus, sonst verweigert macOS den Start.

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

`alembic-engine` ist dieselbe Engine wie die Payload, mit derselben Webseite
(`http://localhost:5940`), zum Umwandeln auf einem PC oder zum Koppeln mit der
Konsole. `--root DIR` (mehrfach angebbar) wählt die Ordner, die sie freigibt
(standardmäßig dein Benutzerordner); `alembic-engine --help` listet die übrigen
Optionen. Einstellungen → Updates meldet, wenn ein neueres Release erschienen
ist; auf einem PC lädst du es herunter und ersetzt die Dateien selbst.

## Bauen

Voraussetzungen: CMake 3.20+, ein C++20-Compiler, zlib (für die PS5
mitgeliefert) und Node.js 20+ mit npm für die Webseite der Engine
(`src/engine/web`, Preact und TypeScript, gebündelt mit esbuild; der erste
Build führt dort `npm ci` aus).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/alembic_tests
```

Windows (`alembic.exe`, `alembic-engine.exe`) wird mit
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) unter macOS oder Linux
cross-kompiliert; die ausführbaren Dateien sind statisch gelinkt und laufen so,
wie sie sind, unter Windows 10 und neuer. `tools/windows/smoke.sh` prüft sie
unter Windows (Git Bash).

```sh
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DLLVM_MINGW=/path/to/llvm-mingw
cmake --build build-win -j
```

Die PS5-Payload braucht LLVM 18 und
[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) v0.44:

```sh
# macOS: brew install llvm@18 · Debian/Ubuntu: apt install clang-18 lld-18 llvm-18
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk   # default: .refs/ps5-payload-sdk
tools/ps5/build.sh                                # → build-ps5/alembic.elf
```

GitHub Actions baut und testet alles bei jedem Push, der mehr als die Doku
ändert (siehe [.github/workflows/build.yml](.github/workflows/build.yml)).
Jeder Lauf bietet die Payload als eigenen Download an, `alembic-<version>.elf`,
und ein Bündel, `alembic-<version>.zip`: die Payload in `ps5/`, `alembic` und
`alembic-engine` in `linux-x64/`, `macos-arm64/` und `windows-x64/` (`.exe`),
mit der README und den Lizenzdateien. Die Version ist der Tag (`v0.1.0`), sonst
Branch und Commit (`main-a2fab12`). Ein `v*`-Tag veröffentlicht die Payload,
das Bündel, ein Paket pro PC-System, die Konsolen-App (`.ffpkg` und `.zip`) und
`SHA256SUMS` als Release. Ab 0.3.1 trägt jede Release-Datei eine
Build-Attestierung: `gh attestation
verify <file> --repo fthdgn-gh/Alembic` zeigt, dass der Workflow dieses
Repositorys sie gebaut hat, und aus welchem Commit.

### Die Webseite

Die Seite der Engine ist `src/engine/web`: Preact-Komponenten in TypeScript
(`src/pages/` hat einen Ordner pro Bereich; `src/main.tsx` verbindet den Rest),
für den älteren Browser der PS5 zu einer HTML-Datei gebündelt, die die Engine
einbettet. Um daran zu arbeiten, lass eine Engine laufen und die Seite bei
jeder Änderung neu bauen:

```sh
build/alembic-engine --listen 127.0.0.1 &   # its API, on port 5940
cd src/engine/web
node build.mjs --serve                      # the page at http://127.0.0.1:5950
npx tsc --noEmit                            # type check (CI runs it too)
python3 ../../../tools/i18n_keys.py --check # every text translated
```

`tools/screenshots/shoot.sh` erstellt die Screenshots der README gegen eine
Ersatz-Engine mit erfundenen Spielen.

### Abgleich mit den Referenzwerkzeugen

Die Skripte in `tools/` vergleichen die Ausgabe von Alembic Byte für Byte mit
den Werkzeugen, denen es folgt. Sie brauchen die Referenzprojekte, ausgecheckt
in `.refs/` (von git ignoriert); [docs/NOTES.md](docs/NOTES.md) beschreibt, wie
man es neu anlegt.

```sh
tools/oracle/compare.sh        # exFAT, FFPKG, FFPFS, FFPFSC vs PS5PKGTool and MkPFS
tools/pkg-oracle/compare.sh    # FPKG reading and writing vs ProsperoPkgTool / LibProsperoPKG
tools/pkg-oracle/write_samples.sh  # FPKG writing vs ProsperoPkgTool on your own packages
tools/lz4-oracle/compare.sh    # LZ4 packs vs ampr_emu's ampr_pack.py
tools/zstd-oracle/compare.sh   # zstd packs vs ps5-dump-forge (builds it with cargo)
tools/fself-oracle/compare.sh  # fake-signed SELFs vs ampr-emu-ng's make_fself.py
tools/ps4-oracle/compare.sh    # PS4 packages vs LibOrbisPkg's pkg_build, pkg_extract and pkg_validate
```

## Was Alembic nicht enthält

- Kein Oodle, kein `libScePubTools`, keine Closed-Source-Paketbibliotheken
  und keine Konsolenschlüssel in diesem Repository oder seinen Binärdateien.
  Die FPKG-Ausgabe nutzt die veröffentlichten Schlüsseldateien von
  ProsperoPkgTool; die PS4-Paketausgabe nutzt `Keys.cs` von LibOrbisPkg (darin
  steht auch der Debug-Lizenzschlüssel, der nur dazu dient, eine Lizenz für ein
  Spiel ohne eine zu erzeugen, PS5 oder PS4). Die Engine lädt beides auf
  Wunsch herunter.
- Keine Binärdateien von Dritten: Die `libSceAmpr.sprx` der AMPR-Emulatoren
  wird aus dem offiziellen GitHub-Release des jeweiligen Projekts
  heruntergeladen und anhand des dort veröffentlichten SHA-256 geprüft.

## Danksagung

Alembic baut auf der Arbeit der PS5-Homebrew-Szene auf. Danke an:

- **pearlxcore** für [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool)
  (exFAT, PFSC und FFPFSC; die Referenz, mit der Alembic Byte für Byte
  übereinstimmt) und
  [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool) (Lesen und
  Schreiben von Debug-Paketen, und seine Schlüsseldateien).
- **SadykovIV** für [PS4pkg_to_ffpfsc](https://github.com/SadykovIV/PS4pkg_to_ffpfsc)
  (wie das Image eines PS4-Spiels für ShadowMount+ aussehen muss, seine
  `param.json`).
- **SvenGDK** für [UFS2Tool](https://github.com/SvenGDK/UFS2Tool) (Lesen und
  Schreiben von FFPKG) und [LibProsperoPKG](https://github.com/SvenGDK/LibProsperoPKG)
  (Testpakete).
- **drakmor** für [ampr_emu](https://github.com/drakmor/ampr_emu) (der
  AMPR-Index, LZ4-Asset-Pakete und die Laufzeit, die sie liest) und
  [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) (der Loader,
  für den die Images von Alembic gemacht sind, und sein HTTPS-Download-Code).
- **quer3q** für [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng) (der
  zweite AMPR-Emulator und seine zstd-Pakete) und
  [ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge) (der
  zstd-Paket-Schreiber, dem Alembic folgt und mit dem es abgeglichen wird).
- **PSBrew** für [MkPFS](https://github.com/PSBrew/MkPFS) (der PFS-Schreiber).
- **maxton** für [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg)
  (PS4-Pakete: der Schreiber, dem Alembic folgt, seine Schlüssel und das
  Werkzeug, mit dem es abgeglichen wird).
- **theghostonline** für
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression)
  (LZ4-Paketprofile und die Idee von Engine und Seite).
- **danthespal** für die LZ4-Spielprofile, veröffentlicht mit
  [MkPFS.CSharp](https://github.com/danthespal/MkPFS.CSharp).
- **ps5-payload-dev** für das [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk),
  [shsrv](https://github.com/ps5-payload-dev/shsrv) (Paketinstallationen) und
  [websrv](https://github.com/ps5-payload-dev/websrv) (die Verknüpfung im Tab
  Medien), zusammen mit
  [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager)
  und [itsPLK/ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager).
- **blackbearreloaded** für [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui),
  das Design, dem die Seite folgt.
- Die [offenen Spezifikationen](https://learn.microsoft.com/openspecs/) von
  Microsoft (SMB2/3, NTLM, SPNEGO, DCE/RPC und die Freigabeliste) und die
  IETF-RFCs zu NFSv3, ONC RPC und WebDAV, nach denen die Netzwerk-Clients
  geschrieben sind.
- [zlib](https://zlib.net), [LZ4](https://github.com/lz4/lz4) und
  [Zstandard](https://github.com/facebook/zstd), mit der Engine
  mitgeliefert, und [Preact](https://github.com/preactjs/preact) mit seinen
  [signals](https://github.com/preactjs/signals), in ihre Seite gebündelt.

Die Spiele, Engines und Adressen in den Screenshots sind erfunden
(`tools/screenshots/shoot.sh` rendert sie gegen eine Ersatz-Engine).

## Lizenz

GPL-3.0-or-later ([LICENSE](LICENSE)). Alembic portiert Code aus mehreren GPL-,
LGPL-, MIT- und BSD-Projekten; [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
listet jedes davon auf, was daraus übernommen wurde und seine Lizenz.
