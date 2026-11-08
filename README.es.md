# Alembic

[English](README.md) · [Türkçe](README.tr.md) · [Deutsch](README.de.md) · **Español** · [Français](README.fr.md) · [Magyar](README.hu.md) · [Русский](README.ru.md) · [日本語](README.ja.md) · [简体中文](README.zh.md)

*Esta es una traducción del [README en inglés](README.md); si no coinciden, el inglés es el que está al día.*

![La página de Alembic en el navegador de la PS5: una biblioteca de juegos en varios formatos, con unidades, un recurso compartido de un NAS, una exportación NFS, un servidor FTPS y un PC emparejado a la izquierda](docs/screenshots/files.png)

Alembic convierte juegos de PS5 y PS4 entre los formatos que usan los
cargadores homebrew: paquetes (FPKG debug de PS5, PKG falso de PS4), carpetas
de volcado, imágenes exFAT, FFPKG (UFS2), FFPFS (PFS) y FFPFSC (PFS
comprimido). Funciona **en la consola** como aplicación en la pantalla de
inicio (desde la 0.3.0) o como payload con una página web, ambas manejables
con el mando, y en un PC (Windows, macOS, Linux) como herramienta de línea de
comandos o con el mismo motor y la misma página. Los motores de tu red pueden
emparejarse y trabajar juntos: convertir un paquete que está en tu PC, con el
procesador del PC, directamente en el almacenamiento de la consola. Los juegos
de un PC o un NAS también se pueden leer y guardar mediante SMB, NFS, WebDAV o
FTP.

> En desarrollo. Guarda una copia de todo lo que conviertas hasta que hayas
> comprobado el resultado en tu consola.

**Probado en una consola hasta ahora:** imágenes FFPFSC creadas a partir de
volcados y de paquetes funcionan con ShadowMount+; los juegos que necesitan el
emulador AMPR arrancan una vez añadido; la consola y un PC se emparejan y
convierten el uno para el otro; **Instalar** pasa un paquete debug al
instalador de la consola, incluidos los paquetes que Alembic crea a partir de
un volcado (sin comprimir o con Kraken, con una licencia debug generada, así
que el título no muestra candado); una conversión en pausa continúa después de
actualizar el motor; el motor se actualiza solo a partir de una versión
publicada (cargado con Payload Manager); la aplicación de la consola arranca
desde ShadowMount+, inicia y reinicia el motor y maneja una conversión en un
PC emparejado; un paquete convertido desde un recurso compartido SMB; un juego
en paquetes zstd (un paquete de 160 GB empaquetado a nivel 5 por un motor de PC
en un exFAT de 96 GB escrito en la consola) arranca con ampr-emu-ng.
Los juegos de PS4 convertidos a partir de sus paquetes arrancan desde imágenes
con ShadowMount+ 1.7beta8 (muestra "Trophy registration failed" con ellos: un
problema de ShadowMount+, [#97](https://github.com/drakmor/ShadowMountPlus/issues/97)).
**Aún no:** NFS, WebDAV, cifrado SMB, FTPS y HTTPS desde una consola, ni NAS
reales ni carpetas compartidas de Windows (todo ello se comprobó en un PC con
Samba, rclone, WsgiDAV y pyftpdlib); arrancar un juego instalado desde un
paquete de Alembic (se probó la instalación, no el arranque); un paquete de PS4
creado por Alembic; actualizaciones de PS4 (paquetes de parche) y DLC. Los
paquetes de recursos LZ4 funcionan con algunos juegos y con otros no.

## Qué hace

| De ↓ / A → | Carpeta | exFAT | FFPKG (UFS2) | FFPFS (PFS) | FFPFSC | Paquete |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| Carpeta de volcado | – | ✓ | ✓ | ✓ | ✓ | ✓ |
| exFAT | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPKG (UFS2) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFS (PFS) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFSC | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FPKG de PS5 (debug) | ✓ | ✓ | ✓ | ✓ | ✓ | `--rebuild` |
| PKG de PS4 (falso) | ✓ | ✓ | ✓ | ✓ | ✓ | `--rebuild` |

El paquete en que se convierte un juego depende del juego: un juego de PS5
(`sce_sys/param.json`) pasa a ser un FPKG debug de PS5, y uno de PS4
(`sce_sys/param.sfo`), un PKG falso de PS4. La página y la aplicación marcan
cada juego con una etiqueta **PS5** (blanca) o **PS4** (negra).

- **Juegos de PS4** (desde la 0.3.2): los PKG falsos con la habitual clave de
  acceso todo ceros (tanto de herramientas de empaquetado antiguas como
  nuevas) se abren como los paquetes de PS5, con los archivos del juego más
  `sce_sys` (`param.sfo`, iconos, trofeos, licencia) del paquete, y se
  convierten a cualquier imagen. Las imágenes y carpetas de un juego de PS4
  reciben el `sce_sys/param.json` que ShadowMount+ necesita para arrancarlo
  (creado a partir de `param.sfo`, como lo hace
  [PS4pkg_to_ffpfsc](https://github.com/SadykovIV/PS4pkg_to_ffpfsc)); probado
  en una consola con ShadowMount+ 1.7beta8. Un juego de PS4 en cualquier
  formato se puede volver a convertir en un PKG falso de PS4, creado como lo
  hace `pkg_build` de [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg):
  se conserva la licencia del juego (solo se genera si falta) y los archivos
  de PlayGo se crean de nuevo. Un paquete recreado desde cualquier imagen suya
  es idéntico, byte a byte. Las claves vienen de LibOrbisPkg (Componentes, o
  `--ps4-keys FILE` en un PC). El emulador AMPR y los paquetes LZ4 son solo
  para juegos de PS5. La consola a la que pertenece un juego se deduce de su
  ID de título (`CUSA`, `PPSA`) o, si no, del SDK con el que se compiló su
  `eboot.bin` (homebrew); `alembic game-info` muestra ambos. Aún no:
  actualizaciones y DLC, ni paquetes creados con una clave de acceso que no
  sea todo ceros.
- **Flujo continuo:** cada entrada se lee como un árbol de archivos y se
  escribe directamente en la salida, sin copias temporales, así que un paquete
  de 100 GB solo necesita el espacio del resultado.
- **Idéntico byte a byte** a las herramientas de referencia: exFAT, FFPKG y
  FFPFSC coinciden con [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool);
  FFPFS coincide con [MkPFS](https://github.com/PSBrew/MkPFS) (`pack folder --raw
  --no-compress`); la salida FPKG coincide con
  [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool)
  (`img_create`, sin comprimir o con `--compression kraken`, con la misma
  semilla fija); los paquetes de PS4 coinciden con `pkg_build` de
  [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg), también al recrear un
  paquete a partir del proyecto que extrae su `pkg_makegp4`.
- **Paquetes de recursos LZ4** para el runtime PackedStdio de
  [ampr_emu](https://github.com/drakmor/ampr_emu), de drakmor (`--lz4`),
  idénticos byte a byte a los de su `ampr_pack.py`, con perfiles de juego de
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression).
- **Paquetes zstd** para [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng),
  de quer3q (`--zstd`, o "Empaquetar en paquetes zstd" al convertir),
  idénticos byte a byte a los de
  [ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge): más pequeños que
  los LZ4 y más rápidos de crear (un conjunto de 26 GiB de archivos de juego
  quedó en 11,1 GiB en 33 s en un Mac, frente a 17,5 GiB en 77 s con LZ4). El
  juego recibe el `libSceAmpr.sprx` de ampr-emu-ng (de Componentes); desactiva
  "update emulators" en ShadowMount+ para que no lo cambie por uno que no
  pueda leerlos. Niveles 1, 5 (predeterminado) y 19. Probado en una PS5. Un
  juego empaquetado puede pasar de paquetes LZ4 a zstd y viceversa en
  Convertir ("Cambiar a paquetes zstd"): los paquetes antiguos se descodifican
  directamente en los nuevos.
- **Emulador AMPR:** los juegos que usan el lector de archivos APR de la
  consola necesitan el `libSceAmpr.sprx` de un emulador para arrancar desde
  una imagen o carpeta: ampr_emu, de drakmor, o ampr-emu-ng, de quer3q
  (Componentes indica cuál está en uso). Alembic lo añade al convertir, o a un
  juego que ya tengas; en un juego que ya tiene uno, "Añadir el emulador AMPR"
  ofrece cambiarlo por el otro.
- **Firma falsa:** un juego de PS5 cuyos ejecutables son ELF simples (sin
  firmar) nunca arranca. Las conversiones los transforman por el camino en
  SELF con firma falsa, y Convertir lo avisa antes: para una imagen o carpeta,
  de forma idéntica byte a byte a `make_fself.py` de ampr-emu-ng; para un
  paquete, a `img_create` de ProsperoPkgTool.
- **Rápido:** la compresión y la descodificación de paquetes usan todos los
  núcleos; AES, CRC32C y SHA-256 usan las instrucciones propias de la CPU
  cuando las tiene; zlib a nivel 6 de forma predeterminada (nivel 9 para una
  salida exacta a la de PS5PKGTool).

## En la PS5

El trabajo lo hace el motor (`alembic.elf`), un payload que sigue en marcha
después de cerrar aquello con lo que lo manejas: la **aplicación Alembic** de
la pantalla de inicio, o su página web en el navegador de la consola (desde un
acceso directo que añade a la pestaña Multimedia) o en cualquier navegador de
tu red en `http://<console-ip>:5940`. La aplicación y la página muestran lo
mismo y funcionan igual.

![La aplicación Alembic en la PS5: una carpeta homebrew con juegos en varios formatos, unidades, un recurso compartido de un NAS, una exportación NFS y un PC emparejado a la izquierda](docs/screenshots/app.png)

**La aplicación** (`PPSA99594`, desde la 0.3.0) se instala de una de estas
formas:

1. **Desde ProsperoStore** (lo más fácil): Alembic aparece en
   [ProsperoStore](https://github.com/darkxex/ProsperoStore), la tienda de
   la consola para homebrew nativo ([página de Alembic](https://prosperostore.net/app/PPSA99594/)).
   Instálala desde allí; la tienda también la actualiza.
2. **Como imagen o carpeta**, mediante
   [ShadowMount+](https://github.com/drakmor/ShadowMountPlus): copia
   `alembic-app-<version>.ffpkg` de las versiones publicadas en
   `/data/homebrew/` (o descomprime allí `alembic-app-<version>.zip`: la
   carpeta `PPSA99594`). Su icono aparece en la pantalla de inicio tras la
   siguiente búsqueda de ShadowMount+.

Ábrela. Si no responde ningún motor, envía el suyo al cargador ELF en el
puerto 9021 (una aplicación que no lo lleva descarga antes el motor de su misma
versión desde la versión publicada en GitHub, comprobado con `SHA256SUMS`).

**Sin la aplicación:**

1. Envía `alembic-<version>.elf` (de las versiones publicadas) al cargador ELF
   de tu consola.
2. Abre Alembic desde la pestaña Multimedia (o en la dirección de arriba).
3. En **Archivos**, elige un juego del almacenamiento interno, de un USB, de un
   servidor de red (SMB, NFS, WebDAV o FTP) o de una unidad de un motor
   emparejado (una etiqueta distingue los juegos de PS5 y de PS4) y luego
   **Convertir**, **Instalar** (paquetes), **Copiar** o **Mover**. Al
   convertir un juego de PS5 en paquete, este se puede pasar al instalador de
   la consola al terminar (**Instalar al terminar**); después el paquete se
   borra en cuanto el instalador lo ha leído, salvo que **Conservar el
   paquete** esté activado, así que hasta entonces necesita espacio.
4. Las tareas se ejecutan de una en una en **Cola**: pausar, reanudar (las
   tareas largas continúan desde un punto de control), reordenar, cancelar.
   Recibes una notificación cuando termina una.

![El diálogo Convertir: formatos de destino, nivel de compresión, los interruptores de paquetes LZ4 y zstd, el del emulador AMPR y la ruta de salida en un PC emparejado](docs/screenshots/convert.png)

![La cola: una conversión en curso con su velocidad y el tiempo restante, otra en pausa que continúa desde su punto de control, una copia a un recurso compartido de un NAS y una tarea que se ejecuta en el PC emparejado](docs/screenshots/queue.png)

También en la página:

- **Primero el mando:** la cruceta mueve, ✕ elige, ○ vuelve atrás, □ abre las
  opciones de un archivo, L1/R1 cambian de sección y L2/R2 pasan páginas en
  las listas. Para salir, pulsa L2 + R2 a la vez y luego ○ (una página no
  puede cerrar por sí misma el navegador de la consola). Ajustes tiene una
  prueba del mando. En un PC: flechas, Intro, Esc, S, Q/E y Re Pág/Av Pág.
- **Lugares:** añade cualquier carpeta a Lugares desde sus Opciones; aparecen
  en la barra lateral, en Guardar en y en los motores emparejados.
- **Renombrar** (desde la 0.4.1): al renombrar un juego se ofrecen, junto al
  nombre actual, nombres formados con su título, ID de título, versión y
  formato (`Title [ABCD12345].ffpfsc`, …); el elegido se puede seguir
  editando. Los formatos los cambias tú en **Ajustes → Formatos de nombre**:
  campos `{title}` `{titleid}` `{contentid}` `{version}` `{format}`
  `{platform}` `{name}` (o `[titleid]`), modificadores como `{title:upper}`,
  `{title:20}` o `{version:short}`, `<…>` para una parte que se omite cuando
  falta un valor y, por formato, qué hacer con los espacios (conservarlos,
  quitarlos, `_`, `.`, `-`) y con las letras (tal cual, MAYÚSCULAS,
  minúsculas). Un nombre demasiado largo para que ShadowMount+ lo monte (58
  bytes antes de `.ffpfsc`, 63 antes de `.exfat`, `.ffpkg` y `.ffpfs`) se
  marca y va seguido de uno acortado, y si lo has escrito tú, se te pregunta.
  Un nombre oculto (`.name`) sigue oculto.
- **Varios archivos a la vez** (desde la 0.4.2): □ en un archivo →
  **Seleccionar…**; luego ✕ marca filas, □ abre las acciones para las marcadas
  y ○ termina la selección. Convertir (una tarea por juego, omitiendo los que
  ya están en el formato de destino), Instalar, Añadir el emulador AMPR,
  Ocultar para / Mostrar a ShadowMount+, Renombrar con un formato (primero se
  muestra antiguo → nuevo), Enviar a otro motor, Copiar, Mover y Borrar; cada
  acción toma los archivos a los que se aplica.
- **Paquetes y ocultar** (desde la 0.5.0): los juegos en paquetes de recursos
  llevan una etiqueta LZ4 o ZSTD tras su formato. **Ocultar** / **Mostrar**
  (renombrar a `.name` y al revés) funcionan con cualquier archivo o carpeta,
  paquetes incluidos; las imágenes conservan Ocultar para / Mostrar a
  ShadowMount+, que además vuelven a buscar.
- **Servidores de red** (Ajustes, desde la 0.3.3): los juegos de un PC o un
  NAS se pueden convertir sin copiarlos antes a la consola, mediante
  - **SMB** (carpetas compartidas de Windows, la mayoría de los NAS): SMB 2 y
    3 con firma, como exige Windows 11, y cifrado SMB 3 (AES-128-GCM o -CCM)
    cuando el servidor o el recurso compartido lo exige; usuario y contraseña,
    o acceso de invitado. Deja vacío el recurso compartido para ver todos los
    del servidor.
  - **NFS** (NAS y exportaciones de Linux): NFSv3, con el uid:gid que se usará
    en lugar de una contraseña. Deja vacía la exportación para verlas todas.
  - **WebDAV** (NAS, Nextcloud, rclone): HTTP o HTTPS, inicio de sesión Basic
    o Digest.
  - **FTP**: FTP o FTPS (explícito, o implícito en el puerto 990).

  El servidor solo guarda los archivos: el motor lee el juego por la red y
  hace él mismo la conversión. Las salidas exFAT, FFPKG, FFPFS y de carpeta se
  pueden guardar de vuelta en el servidor; las salidas FFPFSC y PKG van
  primero a la consola. Con **Cifrar con TLS** activado, FTP y WebDAV van por
  TLS 1.2 o 1.3 (mbedTLS). Los servidores domésticos suelen tener certificados
  autofirmados, así que Alembic confía en el certificado de un servidor la
  primera vez que lo ve, como hace SSH: la primera conexión muestra su huella
  SHA-256 para compararla con la del servidor y pregunta; las conexiones
  siguientes deben mostrar la misma, y un certificado distinto se rechaza con
  un aviso. Para que el trabajo lo haga un PC, ejecuta en él el motor de
  Alembic (consulta
  [Trabajar con varios motores](#trabajar-con-varios-motores)).
- **Desde este ordenador:** abierta en el navegador de un PC, la página puede
  compartir un archivo de ese PC para que la consola lo convierta.
- **ShadowMount+:** puede detectar las imágenes nuevas sin reiniciar, y los
  originales se le pueden ocultar una vez convertidos.
- **Componentes:** los archivos de terceros que Alembic necesita pero no
  incluye (los emuladores AMPR ampr_emu y ampr-emu-ng, los archivos de claves
  FPKG de ProsperoPkgTool, la licencia y las claves de paquetes de PS4 de
  LibOrbisPkg) se descargan cuando se piden desde su fuente oficial y solo se
  conservan si su SHA-256 coincide.
  Los dos emuladores AMPR siguen las últimas versiones de sus proyectos: el
  motor consulta GitHub al iniciarse (o con **Buscar actualizaciones**) y, con
  **Actualizar automáticamente** activado (lo predeterminado), descarga una
  compilación más nueva, comprobada con el SHA-256 que dan para ella GitHub y
  el `SHA256SUMS` de la versión. Un emulador está **en uso**: el que las
  conversiones y "Añadir el emulador AMPR" ponen en un juego. Un juego conserva
  la compilación que recibió; "Añadir el emulador AMPR" en él ofrece
  sustituirla por la que está en uso. ampr-emu-ng también lee sus propios
  paquetes zstd.
  **Sin conexión:** descárgalos en un PC desde los enlaces de abajo (para los
  emuladores, las compilaciones con las que se probó Alembic por última vez;
  cualquier versión posterior del mismo proyecto también sirve) y cópialos a
  la consola con estos nombres exactos; el motor los detecta sin reiniciar:

  | Componente | Descarga | Ubicación | SHA-256 |
  |---|---|---|---|
  | Emulador AMPR (ampr_emu 0.4.2.1) | [libSceAmpr.sprx](https://github.com/drakmor/ampr_emu/releases/download/0.4.2.1/libSceAmpr.sprx) | `/data/alembic/runtime/libSceAmpr.sprx` | `69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a` |
  | Emulador AMPR (ampr-emu-ng 0.2.0) | [libSceAmpr.sprx](https://github.com/quer3q/ampr-emu-ng/releases/download/v0.2.0/libSceAmpr.sprx) | `/data/alembic/runtime/ampr-emu-ng/libSceAmpr.sprx` | `7acb2173d9691b5dfb94ea4007660920b7250111c5fd7a86c6eb91b763ad3d76` |
  | Archivos de claves FPKG | [passcode.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/passcode.bin) | `/data/alembic/keys/passcode.bin` | `81e8283d582f522d8da34565ecd6ab39a38b3d3c0cacef078df97d2b285ec24e` |
  | | [mount_image.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/mount_image.bin) | `/data/alembic/keys/mount_image.bin` | `4510462812321cea26b4c0155e363955b16e31b89252b92510accad02eca5cd0` |
  | | [metadata_modulus.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/metadata_modulus.bin) | `/data/alembic/keys/metadata_modulus.bin` | `4a585ee47fea88d6b121d5e8afc9829438a27ee325cebec56a2ff537129a37c5` |
  | Licencia y claves de paquetes de PS4 | [Keys.cs](https://raw.githubusercontent.com/maxton/LibOrbisPkg/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/Util/Keys.cs) | `/data/alembic/keys/ps4/Keys.cs` | `48b44dd03e653a5c0991b0a2975477802ea2639c4e6ea3270b2ff20a1856f8a7` |

  Los archivos de claves deben ser los fijados (el mismo SHA-256); si no, la
  salida de paquetes los rechaza: son constantes, así que no siguen ninguna
  versión. Una compilación del emulador copiada a mano sigue funcionando, pero
  aparece como "cambiado" en Componentes hasta que el motor descargue una por
  sí mismo.
- **Motor** (Ajustes): **Reiniciar el motor** vuelve a iniciar la misma
  versión mediante el cargador ELF; la aplicación también ofrece **Iniciar el
  motor** cuando se ha detenido.
- **Actualizaciones** (desde la 0.2.3): el motor busca una versión más nueva
  en GitHub al iniciarse y lo avisa; **Ajustes → Actualizaciones** muestra sus
  notas. **Actualizar** descarga el payload, lo comprueba con el `SHA256SUMS`
  de la versión y con la versión escrita en su interior, y sustituye las
  copias desde las que lo inicia tu cargador: la de Payload Manager
  (renombrada a la nueva versión, con el nombre corregido en el `meta.json` de
  ese payload) y las carpetas de `ps5_autoloader` (mismo nombre); **Buscar en
  las unidades** encuentra copias en otros sitios. Después, el nuevo motor
  arranca mediante el cargador ELF (puerto 9021) y toma el relevo; una tarea
  en curso que conserva su progreso continúa. Enviar a mano un `alembic.elf`
  más nuevo también sustituye a un motor en marcha. Desde la 0.3.0, la misma
  actualización sustituye también el `.ffpkg` de la aplicación cuando el de la
  versión es distinto; si la aplicación está abierta, espera a que se cierre
  (una aplicación instalada desde ProsperoStore la actualiza la tienda). Una
  aplicación más antigua que su motor lo indica y ofrece **Actualizar la
  aplicación**.
- **Otros motores:** un motor emparejado que está apagado sigue en la lista
  como desconectado en lugar de desaparecer.
- **Unidades ocupadas:** la aplicación mantiene la cola al día mientras se
  cargan los títulos e iconos de los juegos de una unidad que una tarea en
  curso mantiene ocupada.
- Idiomas: English, Türkçe, Deutsch, Español, Français, Magyar, Русский, 日本語, 简体中文
  (Magyar traducido por isty1974).

El estado (cola, ajustes, componentes descargados) se guarda en
`/data/alembic`.

## Trabajar con varios motores

Ejecuta `alembic-engine` en un PC además del payload en la consola. Una vez
emparejados, cada motor puede leer y escribir los archivos del otro y pasarle
tareas.

1. En **Ajustes → Otros motores**, activa **Buscar motores en esta red** en
   ambos (desactivado de forma predeterminada), o añade uno por su dirección.
2. Pulsa **Emparejar** junto al otro motor. Este muestra un código de seis
   cifras (en la PS5 como notificación, en un PC en su ventana); escríbelo.
   Los motores emparejados firman cada petición con la clave que acordaron.
3. Las unidades y los Lugares del otro motor aparecen en **Archivos** bajo
   *Otros motores*, como entradas y como sitios donde guardar.
4. En **Convertir**, **Convertir en** elige el motor que hace el trabajo
   (entre los que están conectados en ese momento). El origen, el motor que
   convierte y la salida pueden estar cada uno en un motor distinto; los datos
   fluyen entre ellos, sin hacer antes ninguna copia.

Una tarea aparece en la cola de todos los motores cuyos archivos usa ("en PC
del salón") y se puede pausar, reanudar, cancelar o quitar del historial desde
cualquiera de ellos. Cuando una tarea borra su origen tras comprobar la salida,
los motores comparan resúmenes SHA-256 en lugar de devolver los archivos.
También se pueden enviar archivos entre motores sin convertirlos (Opciones →
Enviar a otro motor).

![Ajustes: un PC emparejado y un Mac encontrados en la red, y dos servidores de red: un recurso compartido de un NAS por SMB y una exportación NFS](docs/screenshots/engines.png)

Los motores de distintas versiones de Alembic trabajan juntos mientras se
comuniquen de la misma manera; cuando eso cambia en una versión, Ajustes
indica qué motor hay que actualizar, y se rechazan mutuamente hasta que se
actualiza.

La página en sí no tiene contraseña: cualquiera de tu red puede abrirla.
Inicia un motor de PC con `--listen 127.0.0.1` para limitarlo a ese ordenador;
entonces los demás motores no pueden contactar con él.

## En un PC

Cada versión publicada trae un paquete por sistema:
`alembic-<version>-linux-x64.tar.gz`, `-macos-arm64.tar.gz` (Apple silicon) y
`-windows-x64.zip`, cada uno con `alembic` y `alembic-engine`. Las versiones
de macOS no están firmadas: tras descargarlas desde un navegador, ejecuta una
vez `xattr -d com.apple.quarantine alembic alembic-engine` en su carpeta, o
macOS se negará a abrirlas.

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

`alembic-engine` es el mismo motor que el payload, con la misma página web
(`http://localhost:5940`), para convertir en un PC o emparejarse con la
consola. `--root DIR` (se puede repetir) elige las carpetas que comparte (tu
carpeta personal de forma predeterminada); `alembic-engine --help` muestra las
demás opciones. Ajustes → Actualizaciones avisa cuando sale una versión más
nueva; en un PC, descárgala y sustituye tú los archivos.

## Compilación

Requisitos: CMake 3.20+, un compilador de C++20, zlib (incluido para la PS5) y
Node.js 20+ con npm para la página web del motor (`src/engine/web`, Preact y
TypeScript empaquetados con esbuild; la primera compilación ejecuta allí
`npm ci`).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/alembic_tests
```

La versión de Windows (`alembic.exe`, `alembic-engine.exe`) se compila de forma
cruzada con [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) desde macOS o
Linux; los ejecutables se enlazan estáticamente y funcionan tal cual en
Windows 10 y posteriores. `tools/windows/smoke.sh` los comprueba en Windows
(Git Bash).

```sh
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DLLVM_MINGW=/path/to/llvm-mingw
cmake --build build-win -j
```

El payload de PS5 necesita LLVM 18 y
[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) v0.44:

```sh
# macOS: brew install llvm@18 · Debian/Ubuntu: apt install clang-18 lld-18 llvm-18
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk   # default: .refs/ps5-payload-sdk
tools/ps5/build.sh                                # → build-ps5/alembic.elf
```

GitHub Actions compila y prueba todo en cada push que cambia algo más que la
documentación (consulta [.github/workflows/build.yml](.github/workflows/build.yml)).
Cada ejecución ofrece el payload como descarga propia, `alembic-<version>.elf`,
y un paquete conjunto, `alembic-<version>.zip`: el payload en `ps5/`,
`alembic` y `alembic-engine` en `linux-x64/`, `macos-arm64/` y `windows-x64/`
(`.exe`), con el README y los archivos de licencia. La versión es la etiqueta
(`v0.1.0`) o, si no hay, la rama y el commit (`main-a2fab12`). Una etiqueta
`v*` publica como versión el payload, el paquete conjunto, un paquete por
sistema de PC, la aplicación de la consola (`.ffpkg` y `.zip`) y
`SHA256SUMS`. Desde la 0.3.1, cada archivo publicado lleva una atestación de
compilación: `gh attestation verify <file> --repo fthdgn-gh/Alembic` muestra
que lo compiló el workflow de este repositorio, y a partir de qué commit.

### La página web

La página del motor está en `src/engine/web`: componentes de Preact en
TypeScript (`src/pages/` tiene una carpeta por sección; `src/main.tsx` reúne el
resto), empaquetados para el navegador más antiguo de la PS5 en un solo
archivo HTML que el motor incorpora. Para trabajar en ella, ejecuta un motor y
deja que la página se recompile con cada cambio:

```sh
build/alembic-engine --listen 127.0.0.1 &   # its API, on port 5940
cd src/engine/web
node build.mjs --serve                      # the page at http://127.0.0.1:5950
npx tsc --noEmit                            # type check (CI runs it too)
python3 ../../../tools/i18n_keys.py --check # every text translated
```

`tools/screenshots/shoot.sh` genera las capturas del README con un motor de
prueba y juegos inventados.

### Comprobación con las herramientas de referencia

Los scripts de `tools/` comparan byte a byte la salida de Alembic con la de las
herramientas que sigue. Necesitan los proyectos de referencia descargados en
`.refs/` (ignorado por git); [docs/NOTES.md](docs/NOTES.md) explica cómo
recrearlo.

```sh
tools/oracle/compare.sh        # exFAT, FFPKG, FFPFS, FFPFSC vs PS5PKGTool and MkPFS
tools/pkg-oracle/compare.sh    # FPKG reading and writing vs ProsperoPkgTool / LibProsperoPKG
tools/pkg-oracle/write_samples.sh  # FPKG writing vs ProsperoPkgTool on your own packages
tools/lz4-oracle/compare.sh    # LZ4 packs vs ampr_emu's ampr_pack.py
tools/zstd-oracle/compare.sh   # zstd packs vs ps5-dump-forge (builds it with cargo)
tools/fself-oracle/compare.sh  # fake-signed SELFs vs ampr-emu-ng's make_fself.py
tools/ps4-oracle/compare.sh    # PS4 packages vs LibOrbisPkg's pkg_build, pkg_extract and pkg_validate
```

## Lo que Alembic no incluye

- Ni Oodle, ni `libScePubTools`, ni bibliotecas de paquetes de código cerrado,
  ni claves de la consola, en este repositorio ni en sus binarios. La salida
  FPKG usa los archivos de claves publicados de ProsperoPkgTool; la salida de
  paquetes de PS4 usa el `Keys.cs` de LibOrbisPkg (que también contiene la
  clave de licencia debug, usada solo para generar una licencia para un juego
  que no la tiene, de PS5 o de PS4). El motor descarga ambos cuando se piden.
- Ningún binario de terceros: el `libSceAmpr.sprx` de los emuladores AMPR se
  descarga de la versión oficial de cada proyecto en GitHub y se comprueba con
  el SHA-256 publicado junto a ella.

## Créditos

Alembic se apoya en el trabajo de la escena homebrew de PS5. Gracias a:

- **pearlxcore** por [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool)
  (exFAT, PFSC y FFPFSC; la referencia con la que Alembic coincide byte a
  byte) y [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool)
  (lectura y escritura de paquetes debug, y sus archivos de claves).
- **SadykovIV** por [PS4pkg_to_ffpfsc](https://github.com/SadykovIV/PS4pkg_to_ffpfsc)
  (cómo debe ser la imagen de un juego de PS4 para ShadowMount+, su
  `param.json`).
- **SvenGDK** por [UFS2Tool](https://github.com/SvenGDK/UFS2Tool) (el lector y
  escritor de FFPKG) y [LibProsperoPKG](https://github.com/SvenGDK/LibProsperoPKG)
  (paquetes de prueba).
- **drakmor** por [ampr_emu](https://github.com/drakmor/ampr_emu) (el índice
  AMPR, los paquetes de recursos LZ4 y el runtime que los lee) y
  [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) (el cargador
  para el que se hacen las imágenes de Alembic, y su código de descarga HTTPS).
- **quer3q** por [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng) (el
  segundo emulador AMPR, y sus paquetes zstd) y
  [ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge) (el creador de
  paquetes zstd que Alembic sigue y con el que se compara).
- **PSBrew** por [MkPFS](https://github.com/PSBrew/MkPFS) (el escritor de PFS).
- **maxton** por [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) (paquetes
  de PS4: el escritor que sigue Alembic, sus claves y la herramienta con la que
  se compara).
- **theghostonline** por
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression)
  (los perfiles de paquetes LZ4 y la idea de motor más página).
- **danthespal** por los perfiles de juego LZ4 publicados con
  [MkPFS.CSharp](https://github.com/danthespal/MkPFS.CSharp).
- **ps5-payload-dev** por el [SDK de payloads de PS5](https://github.com/ps5-payload-dev/sdk),
  [shsrv](https://github.com/ps5-payload-dev/shsrv) (instalación de paquetes) y
  [websrv](https://github.com/ps5-payload-dev/websrv) (el acceso directo de la
  pestaña Multimedia), junto con
  [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager)
  e [itsPLK/ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager).
- **blackbearreloaded** por [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui),
  el diseño que sigue la página.
- Las [especificaciones abiertas](https://learn.microsoft.com/openspecs/) de
  Microsoft (SMB2/3, NTLM, SPNEGO, DCE/RPC y la lista de recursos compartidos)
  y los RFC del IETF para NFSv3, ONC RPC y WebDAV, a partir de los cuales se
  escribieron los clientes de red.
- [zlib](https://zlib.net), [LZ4](https://github.com/lz4/lz4) y
  [Zstandard](https://github.com/facebook/zstd), incluidos en el motor, y
  [Preact](https://github.com/preactjs/preact) con sus
  [signals](https://github.com/preactjs/signals), incluidos en su página.

Los juegos, motores y direcciones de las capturas son inventados
(`tools/screenshots/shoot.sh` los genera con un motor de prueba).

## Licencia

GPL-3.0-or-later ([LICENSE](LICENSE)). Alembic adapta código de varios
proyectos GPL, LGPL, MIT y BSD; [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
enumera cada uno, lo que se tomó de él y su licencia.
