# Alembic

[English](README.md) · [Türkçe](README.tr.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Magyar](README.hu.md) · [Русский](README.ru.md) · [日本語](README.ja.md) · **简体中文**

*本文是[英文 README](README.md) 的译文；两者不一致时，以英文版为准。*

![PS5 浏览器中的 Alembic 页面：多种格式的游戏库，左侧是驱动器、一个 NAS 共享、一个 NFS 导出、一个 FTPS 服务器和一台已配对的 PC](docs/screenshots/files.png)

Alembic 在自制加载器所用的各种格式之间转换 PS5 和 PS4 游戏：
安装包（PS5 debug FPKG、PS4 fake PKG）、转储文件夹、exFAT 镜像、FFPKG
（UFS2）、FFPFS（PFS）和 FFPFSC（压缩的 PFS）。它可以**在主机上**作为主屏幕上的应用运行
（从 0.3.0 起），也可以作为带网页的 payload 运行，两者都用手柄操作；
在 PC（Windows、macOS、Linux）上，它是命令行工具，也可以运行同样的引擎和页面。
网络中的多个引擎可以配对协作：把 PC 上的安装包用 PC 的处理器转换，
直接写入主机的存储。PC 或 NAS 上的游戏也可以通过 SMB、NFS、WebDAV 或 FTP
读取和保存。

> 仍在开发中。在主机上确认结果之前，请备份你转换的所有内容。

**目前已在主机上验证：** 用转储和安装包制作的 FFPFSC 镜像可以通过
ShadowMount+ 运行；需要 AMPR 模拟器的游戏在添加模拟器后能够启动；
主机和 PC 可以配对并为对方转换；**安装**会把 debug 安装包交给主机的安装程序，
包括 Alembic 由转储构建的安装包（stored 或 Kraken，带有生成的 debug 许可证，
因此游戏不会被锁定）；暂停的转换在引擎更新后会继续；引擎可以从发布版自我更新
（通过 Payload Manager 加载）；主机应用可以从 ShadowMount+ 启动，能启动和重启引擎，
并能驱动已配对 PC 上的转换；从 SMB 共享转换安装包；以 zstd 包存放的游戏
（一个 160 GB 的安装包由 PC 引擎以 5 级打包，生成 96 GB 的 exFAT 写入主机）
能用 ampr-emu-ng 启动。由安装包转换而来的 PS4 游戏能通过 ShadowMount+ 1.7beta8
从镜像启动（它会对这些游戏显示“Trophy registration failed”：这是
ShadowMount+ 的问题，[#97](https://github.com/drakmor/ShadowMountPlus/issues/97)）。
**尚未验证：** 在主机上使用 NFS、WebDAV、SMB 加密、FTPS 和 HTTPS，
以及真正的 NAS 设备和 Windows 共享（这些都已在 PC 上对照 Samba、rclone、
WsgiDAV 和 pyftpdlib 检查过）；启动从 Alembic 安装包安装的游戏
（试过安装，没试过启动）；Alembic 构建的 PS4 安装包；PS4 更新（补丁包）和 DLC。
LZ4 资源包对部分游戏有效，对另一些无效。

## 功能

| 从 ↓ / 到 → | 文件夹 | exFAT | FFPKG (UFS2) | FFPFS (PFS) | FFPFSC | 安装包 |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| Dump 文件夹 | – | ✓ | ✓ | ✓ | ✓ | ✓ |
| exFAT | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPKG (UFS2) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFS (PFS) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| FFPFSC | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| PS5 FPKG (debug) | ✓ | ✓ | ✓ | ✓ | ✓ | `--rebuild` |
| PS4 PKG (fake) | ✓ | ✓ | ✓ | ✓ | ✓ | `--rebuild` |

游戏转换成哪种安装包取决于游戏本身：PS5 游戏（`sce_sys/param.json`）
成为 PS5 debug FPKG，PS4 游戏（`sce_sys/param.sfo`）成为 PS4 fake PKG。页面和应用
会给每个游戏标上 **PS5**（白色）或 **PS4**（黑色）标签。

- **PS4 游戏**（从 0.3.2 起）：使用常见全零密码的 fake PKG（无论来自旧版还是新版打包工具）
  可以像 PS5 安装包一样打开，内容是游戏文件加上安装包中的 `sce_sys`
  （`param.sfo`、图标、奖杯、许可证），并可转换为任意镜像。PS4 游戏的镜像和文件夹
  会获得 ShadowMount+ 启动所需的 `sce_sys/param.json`（由 `param.sfo` 生成，做法与
  [PS4pkg_to_ffpfsc](https://github.com/SadykovIV/PS4pkg_to_ffpfsc) 相同）；
  已在主机上用 ShadowMount+ 1.7beta8 验证。任何格式的 PS4 游戏都能转换回
  PS4 fake PKG，构建方式与
  [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) 的 `pkg_build` 相同：
  保留游戏的许可证（只在缺失时生成），PlayGo 文件重新生成。由同一安装包的任意镜像
  重建出的安装包逐字节相同。密钥来自 LibOrbisPkg（“组件”，在 PC 上则用
  `--ps4-keys FILE`）。AMPR 模拟器和 LZ4 包只用于 PS5 游戏。游戏属于哪台主机
  由其 title ID（`CUSA`、`PPSA`）判断，否则由构建其 `eboot.bin` 的 SDK 判断（自制程序）；
  `alembic game-info` 会显示两者。尚不支持：更新和 DLC，以及用非全零密码制作的安装包。
- **流式处理：** 每个输入都作为文件树读取并直接写入输出，不生成临时副本，
  因此 100 GB 的安装包只需要结果所占的空间。
- **与参考工具逐字节相同：** exFAT、FFPKG 和 FFPFSC 与
  [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool) 一致；FFPFS 与
  [MkPFS](https://github.com/PSBrew/MkPFS)（`pack folder --raw
  --no-compress`）一致；FPKG 输出与
  [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool)
  （`img_create`，stored 或 `--compression kraken`，使用相同的固定种子）一致；
  PS4 安装包与 [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) 的 `pkg_build`
  一致，用其 `pkg_makegp4` 提取的项目重建安装包时也一致。
- **LZ4 资源包**，用于 drakmor 的 [ampr_emu](https://github.com/drakmor/ampr_emu)
  PackedStdio 运行时（`--lz4`），与其 `ampr_pack.py` 逐字节相同，游戏配置来自
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression)。
- **zstd 包**，用于 quer3q 的 [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng)
  （`--zstd`，或转换时选择“打包为 zstd 包”），与
  [ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge) 的输出逐字节相同：比
  LZ4 更小，制作也更快（在 Mac 上，一组 26 GiB 的游戏文件用 33 秒变成 11.1 GiB，
  LZ4 则是 77 秒变成 17.5 GiB）。游戏会获得 ampr-emu-ng 的 `libSceAmpr.sprx`
  （来自“组件”）；请关闭 ShadowMount+ 的“update emulators”，以免它被换成读不了这些包的模拟器。
  级别有 1、5（默认）和 19。已在 PS5 上验证。已打包的游戏可以在“转换”中
  在 LZ4 包和 zstd 包之间切换（“切换为 zstd 包”），旧包直接解码为新包。
- **AMPR 模拟器：** 使用主机 APR 文件读取器的游戏，要从镜像或文件夹启动，
  需要某个模拟器的 `libSceAmpr.sprx`：drakmor 的 ampr_emu 或 quer3q 的
  ampr-emu-ng（“组件”会显示哪个正在使用）。Alembic 会在转换时添加它，
  也可以添加到你已有的游戏；对已有模拟器的游戏，“添加 AMPR 模拟器”会提供切换到另一个模拟器的选项。
- **伪签名：** 可执行文件是普通（未签名）ELF 的 PS5 游戏永远无法启动。
  转换会顺便把它们变成伪签名的 SELF，“转换”也会事先说明：镜像或文件夹的结果
  与 ampr-emu-ng 的 `make_fself.py` 逐字节相同，安装包的结果与
  ProsperoPkgTool 的 `img_create` 相同。
- **快速：** 压缩和安装包解码使用所有核心；AES、CRC32C 和 SHA-256 在 CPU
  支持时使用其专用指令；默认 zlib 级别 6（要与 PS5PKGTool 输出完全一致则用级别 9）。

## 在 PS5 上

工作由引擎（`alembic.elf`）完成。它是一个 payload，在你关闭操作它的界面后仍会继续运行：
操作界面可以是主屏幕上的 **Alembic 应用**，也可以是它的网页——在主机浏览器中打开
（通过它添加到“媒体”标签页的快捷方式），或在网络中任意浏览器里打开
`http://<console-ip>:5940`。应用和页面显示的内容相同，用法也相同。

![PS5 上的 Alembic 应用：一个包含多种格式游戏的 homebrew 文件夹，左侧是驱动器、一个 NAS 共享、一个 NFS 导出和一台已配对的 PC](docs/screenshots/app.png)

**应用**（`PPSA99594`，从 0.3.0 起）有两种安装方式：

1. **通过 ProsperoStore**（最简单）：Alembic 已上架
   [ProsperoStore](https://github.com/darkxex/ProsperoStore)，这是面向原生自制程序的主机商店
   （[Alembic 的页面](https://prosperostore.net/app/PPSA99594/)）。
   在那里安装即可，商店也会负责更新。
2. **作为镜像或文件夹**，通过
   [ShadowMount+](https://github.com/drakmor/ShadowMountPlus)：把发布版中的
   `alembic-app-<version>.ffpkg` 复制到 `/data/homebrew/`（或把
   `alembic-app-<version>.zip` 解压到那里：即 `PPSA99594` 文件夹）。ShadowMount+
   下次扫描后，它的图标会出现在主屏幕上。

启动它。没有引擎响应时，它会把自带的引擎发送到端口 9021 上的 ELF 加载器
（不带引擎的应用会先从 GitHub 发布版下载与自身版本相同的引擎，并对照 `SHA256SUMS` 校验）。

**不使用应用：**

1. 把 `alembic-<version>.elf`（来自发布版）发送到主机的 ELF 加载器。
2. 从“媒体”标签页（或上面的地址）打开 Alembic。
3. 在**文件**中选择内部存储、USB、网络服务器（SMB、NFS、WebDAV 或 FTP）
   或已配对引擎的驱动器上的游戏（标签会区分 PS5 和 PS4 游戏），然后选择
   **转换**、**安装**（安装包）、**复制**或**移动**。把 PS5 游戏转换为安装包时，
   可以在完成后交给主机的安装程序（**完成后安装**）；安装程序读取完后，
   安装包会被删除，除非开启了**保留安装包**，因此在那之前需要留出空间。
4. 任务在**队列**中逐个运行：可以暂停、继续（长任务从检查点接着进行）、
   调整顺序、取消。任务完成时你会收到通知。

![“转换”对话框：目标格式、压缩级别、LZ4 和 zstd 包开关、AMPR 模拟器开关，以及位于已配对 PC 上的输出路径](docs/screenshots/convert.png)

![队列：一个显示速度和剩余时间的运行中转换、一个将从检查点继续的已暂停任务、一个复制到 NAS 共享的任务，以及一个在已配对 PC 上运行的任务](docs/screenshots/queue.png)

页面还有：

- **手柄优先：** 方向键移动，✕ 选择，○ 返回，□ 打开文件的选项，L1/R1 切换区域，
  L2/R2 翻页。要离开时，同时按 L2 + R2，再按 ○（页面无法自己关闭主机的浏览器）。
  “设置”中有手柄测试。在 PC 上：方向键、Enter、Esc、S、Q/E 和 Page Up/Down。
- **位置：** 在任何文件夹的“选项”中都可以把它添加到“位置”；它们会显示在侧栏、
  “保存到”和已配对的引擎上。
- **重命名**（从 0.4.1 起）：重命名游戏时，除了当前名称，还会提供由标题、
  title ID、版本和格式组成的名称（`Title [ABCD12345].ffpfsc`，……）；选中的名称仍可编辑。
  这些格式可以在**设置 → 命名格式**中自行修改：字段有 `{title}` `{titleid}`
  `{contentid}` `{version}` `{format}` `{platform}` `{name}`（或
  `[titleid]`），修饰符如 `{title:upper}`、`{title:20}` 或
  `{version:short}`，`<…>` 表示值缺失时省略的部分；每种格式还可设定空格的处理
  （保留、移除、`_`、`.`、`-`）和大小写（原样、大写、小写）。对 ShadowMount+ 来说
  太长而无法挂载的名称（`.ffpfsc` 之前 58 字节，`.exfat`、`.ffpkg` 和 `.ffpfs`
  之前 63 字节）会被标出，并在后面给出缩短的名称；手动输入的名称则会询问。
  隐藏名称（`.name`）保持隐藏。
- **一次处理多个文件**（从 0.4.2 起）：在文件上按 □ → **选择…**，然后 ✕ 勾选行，
  □ 打开所勾选文件的操作，○ 结束选择。可用操作有：转换（每个游戏一个任务，
  已是目标格式的游戏会跳过）、安装、添加 AMPR 模拟器、对 ShadowMount+ 隐藏或显示、
  按格式重命名（先显示 旧 → 新）、发送到另一个引擎、复制、移动和删除；
  每个操作只作用于适用的文件。
- **包和隐藏**（从 0.5.0 起）：使用资源包的游戏会在格式后带有 LZ4 或 ZSTD 标签。
  **隐藏** / **取消隐藏**（重命名为 `.name` 再改回）适用于任何文件或文件夹，
  包括安装包；镜像仍保留“对 ShadowMount+ 隐藏”/“对 ShadowMount+ 显示”，
  这两项还会触发重新扫描。
- **网络服务器**（“设置”，从 0.3.3 起）：PC 或 NAS 上的游戏无需先复制到主机即可转换，支持
  - **SMB**（Windows 共享文件夹、大多数 NAS 设备）：SMB 2 和 3，带签名
    （Windows 11 要求如此），服务器或共享要求时使用 SMB 3 加密（AES-128-GCM 或
    -CCM）；使用用户名和密码，或访客登录。共享留空即可查看服务器的所有共享。
  - **NFS**（NAS 和 Linux 导出）：NFSv3，用 uid:gid 代替密码。导出留空即可查看所有导出。
  - **WebDAV**（NAS、Nextcloud、rclone）：HTTP 或 HTTPS，Basic 或 Digest 登录。
  - **FTP**：FTP 或 FTPS（显式，或端口 990 上的隐式）。

  服务器只负责存储文件：引擎通过网络读取游戏，自己完成转换。exFAT、FFPKG、
  FFPFS 和文件夹输出可以保存回服务器；FFPFSC 和 PKG 输出先写到主机上。
  开启**使用 TLS 加密**后，FTP 和 WebDAV 通过 TLS 1.2 或 1.3（mbedTLS）传输。
  家用服务器大多使用自签名证书，因此 Alembic 像 SSH 那样在首次使用时信任服务器的证书：
  首次连接会显示其 SHA-256 指纹供你与服务器上的指纹核对，并询问你；之后的连接
  必须出示相同的证书，证书变化时会拒绝连接并发出警告。如果想让 PC 来做转换，
  请改为在 PC 上运行 Alembic 的引擎（见[跨引擎协作](#跨引擎协作)）。
- **从这台电脑：** 在 PC 的浏览器中打开页面时，可以把那台 PC 上的文件共享给主机转换。
- **ShadowMount+：** 无需重启即可让它识别新镜像，转换完成后还可以对它隐藏源文件。
- **镜像：** ShadowMount+ 扫描的文件夹中的所有镜像、游戏文件夹和软件包
  （包括已隐藏的），按文件夹分组，提供“文件”中除移动和重命名以外的全部操作：
  转换、安装、AMPR 模拟器、对 ShadowMount+ 隐藏、发送到另一个引擎、删除，
  以及一次选择多个。对于使用 APR 的游戏，会显示各自使用的 AMPR 模拟器
  （在游戏中或其 backport 中）以及是否有新版本；**更新 AMPR 模拟器**
  会换上每个游戏自身模拟器的最新版本，没有模拟器的游戏则得到正在使用的那个。
  两个模拟器的所有官方版本都通过 GitHub 公布的 SHA-256 识别。

  ![镜像：ShadowMount+ 文件夹中的游戏，两个可更新的 AMPR 模拟器，一个没有模拟器的游戏，以及一个使用 Alembic 不认识的版本的游戏](docs/screenshots/images.png)
- **组件：** Alembic 需要但不随附的第三方文件（AMPR 模拟器 ampr_emu 和
  ampr-emu-ng、ProsperoPkgTool 的 FPKG 密钥文件、LibOrbisPkg 的许可证密钥和
  PS4 安装包密钥）会按需从官方来源下载，只有 SHA-256 匹配时才保留。
  两个 AMPR 模拟器跟随各自项目的最新发布版：引擎在启动时（或点击**检查更新**时）
  查询 GitHub，开启**自动更新**（默认开启）时会下载更新的版本，并对照 GitHub
  和发布版的 `SHA256SUMS` 给出的 SHA-256 校验。有一个模拟器处于**使用中**：
  即转换和“添加 AMPR 模拟器”放进游戏的那一个。游戏会保留它得到的版本；
  对其使用“添加 AMPR 模拟器”时，会提供替换为使用中版本的选项。ampr-emu-ng
  也能读取它自己的 zstd 包。
  **离线：** 在 PC 上从下面的链接下载（模拟器的链接是 Alembic 最后测试过的版本；
  同一项目的任何更新版本也可以），然后按这些确切的名称复制到主机上；
  引擎无需重启即可识别：

  | 组件 | 下载 | 放置位置 | SHA-256 |
  |---|---|---|---|
  | AMPR 模拟器（ampr_emu 0.4.2.1） | [libSceAmpr.sprx](https://github.com/drakmor/ampr_emu/releases/download/0.4.2.1/libSceAmpr.sprx) | `/data/alembic/runtime/libSceAmpr.sprx` | `69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a` |
  | AMPR 模拟器（ampr-emu-ng 0.2.0） | [libSceAmpr.sprx](https://github.com/quer3q/ampr-emu-ng/releases/download/v0.2.0/libSceAmpr.sprx) | `/data/alembic/runtime/ampr-emu-ng/libSceAmpr.sprx` | `7acb2173d9691b5dfb94ea4007660920b7250111c5fd7a86c6eb91b763ad3d76` |
  | FPKG 密钥文件 | [passcode.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/passcode.bin) | `/data/alembic/keys/passcode.bin` | `81e8283d582f522d8da34565ecd6ab39a38b3d3c0cacef078df97d2b285ec24e` |
  | | [mount_image.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/mount_image.bin) | `/data/alembic/keys/mount_image.bin` | `4510462812321cea26b4c0155e363955b16e31b89252b92510accad02eca5cd0` |
  | | [metadata_modulus.bin](https://raw.githubusercontent.com/pearlxcore/ProsperoPkgTool/f2c91cd7a5d8440b7b9ae5e0b1154334093bf613/ProsperoPkgTool/Keys/metadata_modulus.bin) | `/data/alembic/keys/metadata_modulus.bin` | `4a585ee47fea88d6b121d5e8afc9829438a27ee325cebec56a2ff537129a37c5` |
  | 许可证密钥和 PS4 安装包密钥 | [Keys.cs](https://raw.githubusercontent.com/maxton/LibOrbisPkg/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/Util/Keys.cs) | `/data/alembic/keys/ps4/Keys.cs` | `48b44dd03e653a5c0991b0a2975477802ea2639c4e6ea3270b2ff20a1856f8a7` |

  密钥文件必须是固定的那些（SHA-256 相同），否则安装包输出会拒绝使用：
  它们是常量，所以不跟随任何更新。手动复制的模拟器版本仍然可用，但在“组件”中
  会显示为“已更改”，直到引擎自己下载一个为止。
- **引擎**（“设置”）：**重启引擎**会通过 ELF 加载器再次启动同一版本；
  引擎停止后，应用还会提供**启动引擎**。
- **更新**（从 0.2.3 起）：引擎启动时会在 GitHub 上查找更新的发布版，有则提示；
  **设置 → 更新**会显示其说明。**更新**会下载 payload，对照发布版的
  `SHA256SUMS` 和其中写入的版本号校验，然后替换加载器启动它所用的副本：
  Payload Manager 中的副本（重命名为新版本，并在该 payload 的 `meta.json` 中修正名称）
  以及 `ps5_autoloader` 文件夹中的副本（名称不变）；**搜索驱动器**可以找到其他位置的副本。
  随后新引擎通过 ELF 加载器（端口 9021）启动并接管；能保留进度的运行中任务会继续。
  手动发送更新的 `alembic.elf` 也会替换正在运行的引擎。从 0.3.0 起，当发布版中的
  应用 `.ffpkg` 与现有的不同时，同一次更新也会替换它；应用打开期间，会等到它关闭后再替换
  （从 ProsperoStore 安装的应用由商店更新）。比引擎旧的应用会提示这一点，并提供**更新应用**。
- **其他引擎：** 已配对但关机的引擎会继续列出并显示为离线，而不会消失。
- **忙碌的驱动器：** 当运行中的任务占用某个驱动器、游戏标题和图标还在从中加载时，
  应用仍会及时更新队列。
- 语言：English、Türkçe、Deutsch、Español、Français、Magyar、Русский、日本語、简体中文
  （Magyar 由 isty1974 翻译）。

状态（队列、设置、已下载的组件）保存在 `/data/alembic` 中。

## 跨引擎协作

在主机上运行 payload 的同时，在 PC 上运行 `alembic-engine`。配对后，
每个引擎都可以读写对方的文件，并把任务交给对方。

1. 在**设置 → 其他引擎**中，在两边都开启**查找此网络上的引擎**（默认关闭），
   或按地址添加引擎。
2. 按另一个引擎旁边的**配对**。它会显示一个六位数代码（在 PS5 上以通知显示，
   在 PC 上显示在其窗口中）；输入这个代码。已配对的引擎会用协商好的密钥为每个请求签名。
3. 另一个引擎的驱动器和“位置”会出现在**文件**的*其他引擎*下，既可作为输入，
   也可作为保存位置。
4. 在**转换**中，**转换引擎**用于选择由哪个引擎（当前在线的引擎之一）完成工作。
   源文件、执行转换的引擎和输出可以各自位于不同的引擎上；数据在它们之间流式传输，
   不会先复制一份。

任务会显示在它用到其文件的每个引擎的队列中（“on Living room PC”），并且在其中任何一个上
都可以暂停、继续、取消或从历史中清除。当任务在检查输出后删除源文件时，
引擎之间比较 SHA-256 摘要，而不是把文件传回来比较。文件也可以不经转换直接在引擎之间发送
（选项 → 发送到另一个引擎）。

![设置：在网络中找到的一台已配对 PC 和一台 Mac，以及两个网络服务器：一个通过 SMB 访问的 NAS 共享和一个 NFS 导出](docs/screenshots/engines.png)

不同版本的 Alembic 引擎只要相互通信的方式相同，就能一起工作；某个发布版改变了通信方式时，
“设置”会显示需要更新哪个引擎，在更新之前它们会互相拒绝。

页面本身没有密码：网络中的任何人都能打开它。用 `--listen 127.0.0.1` 启动 PC 引擎，
可以把它限制在那台电脑上；不过这样其他引擎也就无法连接它了。

## 在 PC 上

每个发布版为每个系统提供一个包：`alembic-<version>-linux-x64.tar.gz`、
`-macos-arm64.tar.gz`（Apple 芯片）和 `-windows-x64.zip`，每个都包含
`alembic` 和 `alembic-engine`。macOS 版本没有签名：从浏览器下载后，在其所在文件夹中运行一次
`xattr -d com.apple.quarantine alembic
alembic-engine`，否则 macOS 会拒绝启动它们。

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

`alembic-engine` 与 payload 是同一个引擎，带有同样的网页（`http://localhost:5940`），
用于在 PC 上转换或与主机配对。`--root DIR`（可重复）选择它共享的文件夹
（默认是你的主文件夹）；`alembic-engine --help` 列出其他选项。
“设置 → 更新”会在有新发布版时提示；在 PC 上，请自行下载并替换文件。

## 构建

要求：CMake 3.20+、C++20 编译器、zlib（PS5 版本已内置），以及用于引擎网页的
Node.js 20+ 和 npm（`src/engine/web`，Preact 和 TypeScript 由 esbuild 打包；
首次构建会在那里运行 `npm ci`）。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/alembic_tests
```

Windows 版本（`alembic.exe`、`alembic-engine.exe`）在 macOS 或 Linux 上用
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 交叉编译；可执行文件是静态链接的，
可直接在 Windows 10 及更高版本上运行。`tools/windows/smoke.sh` 在 Windows（Git Bash）上检查它们。

```sh
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DLLVM_MINGW=/path/to/llvm-mingw
cmake --build build-win -j
```

PS5 payload 需要 LLVM 18 和
[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) v0.44：

```sh
# macOS: brew install llvm@18 · Debian/Ubuntu: apt install clang-18 lld-18 llvm-18
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk   # default: .refs/ps5-payload-sdk
tools/ps5/build.sh                                # → build-ps5/alembic.elf
```

每次推送只要改动的不仅是文档，GitHub Actions 就会构建并测试所有内容
（见 [.github/workflows/build.yml](.github/workflows/build.yml)）。
每次运行都提供单独下载的 payload `alembic-<version>.elf`，以及一个合集
`alembic-<version>.zip`：payload 在 `ps5/` 中，`alembic` 和
`alembic-engine` 在 `linux-x64/`、`macos-arm64/` 和 `windows-x64/`（`.exe`）中，
另附 README 和许可证文件。版本号是标签（`v0.1.0`），否则是分支和提交
（`main-a2fab12`）。`v*` 标签会把 payload、合集、每个 PC 系统的包、
主机应用（`.ffpkg` 和 `.zip`）以及 `SHA256SUMS` 发布为一个发布版。
从 0.3.1 起，每个发布文件都带有构建证明：`gh attestation
verify <file> --repo fthdgn-gh/Alembic` 会显示它由本仓库的工作流构建，以及构建自哪个提交。

### 网页

引擎的页面位于 `src/engine/web`：用 TypeScript 编写的 Preact 组件
（`src/pages/` 中每个区域一个文件夹；`src/main.tsx` 负责其余部分的对应关系），
为 PS5 较旧的浏览器打包成一个 HTML 文件，嵌入引擎中。要修改它，先运行一个引擎，
让页面在每次改动时重新构建：

```sh
build/alembic-engine --listen 127.0.0.1 &   # its API, on port 5940
cd src/engine/web
node build.mjs --serve                      # the page at http://127.0.0.1:5950
npx tsc --noEmit                            # type check (CI runs it too)
python3 ../../../tools/i18n_keys.py --check # every text translated
```

`tools/screenshots/shoot.sh` 会对照一个带有虚构游戏的替身引擎，生成 README 中的截图。

### 与参考工具对照检查

`tools/` 中的脚本会把 Alembic 的输出与它所遵循的工具逐字节比较。它们需要把参考项目
检出到 `.refs/`（已被 git 忽略）；[docs/NOTES.md](docs/NOTES.md) 说明了如何重建它。

```sh
tools/oracle/compare.sh        # exFAT, FFPKG, FFPFS, FFPFSC vs PS5PKGTool and MkPFS
tools/pkg-oracle/compare.sh    # FPKG reading and writing vs ProsperoPkgTool / LibProsperoPKG
tools/pkg-oracle/write_samples.sh  # FPKG writing vs ProsperoPkgTool on your own packages
tools/lz4-oracle/compare.sh    # LZ4 packs vs ampr_emu's ampr_pack.py
tools/zstd-oracle/compare.sh   # zstd packs vs ps5-dump-forge (builds it with cargo)
tools/fself-oracle/compare.sh  # fake-signed SELFs vs ampr-emu-ng's make_fself.py
tools/ps4-oracle/compare.sh    # PS4 packages vs LibOrbisPkg's pkg_build, pkg_extract and pkg_validate
```

## Alembic 不包含的内容

- 本仓库及其二进制文件中没有 Oodle、`libScePubTools`、闭源安装包库或主机密钥。
  FPKG 输出使用 ProsperoPkgTool 公开的密钥文件；PS4 安装包输出使用 LibOrbisPkg 的
  `Keys.cs`（其中也有 debug 许可证密钥，只用于为没有许可证的游戏生成许可证，
  PS5 和 PS4 游戏都一样）。引擎会按需下载这两者。
- 没有第三方二进制文件：AMPR 模拟器的 `libSceAmpr.sprx` 从各项目的官方 GitHub
  发布版下载，并用随之公布的 SHA-256 校验。

## 致谢

Alembic 建立在 PS5 自制程序社区的成果之上。感谢：

- **pearlxcore** 的 [PS5PKGTool](https://github.com/pearlxcore/PS5PKGTool)
  （exFAT、PFSC 和 FFPFSC；Alembic 逐字节对齐的参考）和
  [ProsperoPkgTool](https://github.com/pearlxcore/ProsperoPkgTool)（读写 debug 安装包，
  以及它的密钥文件）。
- **SadykovIV** 的 [PS4pkg_to_ffpfsc](https://github.com/SadykovIV/PS4pkg_to_ffpfsc)
  （PS4 游戏的镜像要怎样才能被 ShadowMount+ 使用，以及它的 `param.json`）。
- **SvenGDK** 的 [UFS2Tool](https://github.com/SvenGDK/UFS2Tool)（FFPKG 读写器）和
  [LibProsperoPKG](https://github.com/SvenGDK/LibProsperoPKG)（测试安装包）。
- **drakmor** 的 [ampr_emu](https://github.com/drakmor/ampr_emu)（AMPR 索引、
  LZ4 资源包及读取它们的运行时）和
  [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)（Alembic 镜像所针对的加载器，
  以及它的 HTTPS 下载代码）。
- **quer3q** 的 [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng)（第二个
  AMPR 模拟器及其 zstd 包）和
  [ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge)（Alembic 遵循并对照检查的
  zstd 包写入器）。
- **PSBrew** 的 [MkPFS](https://github.com/PSBrew/MkPFS)（PFS 写入器）。
- **maxton** 的 [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg)（PS4
  安装包：Alembic 遵循的写入器、它的密钥，以及用于对照检查的工具）。
- **theghostonline** 的
  [PS5-LZ4-Compression](https://github.com/theghostonline/PS5-LZ4-Compression)
  （LZ4 包配置，以及“引擎加页面”的思路）。
- **danthespal** 随 [MkPFS.CSharp](https://github.com/danthespal/MkPFS.CSharp)
  发布的 LZ4 游戏配置。
- **ps5-payload-dev** 的 [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk)、
  [shsrv](https://github.com/ps5-payload-dev/shsrv)（安装包安装）和
  [websrv](https://github.com/ps5-payload-dev/websrv)（“媒体”标签页快捷方式），
  以及 [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager)
  和 [itsPLK/ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager)。
- **blackbearreloaded** 的 [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui)，
  页面遵循它的设计。
- Microsoft 的[开放规范](https://learn.microsoft.com/openspecs/)
  （SMB2/3、NTLM、SPNEGO、DCE/RPC 和共享列表）以及 NFSv3、ONC RPC 和 WebDAV 的
  IETF RFC，网络客户端据此编写。
- 随引擎内置的 [zlib](https://zlib.net)、[LZ4](https://github.com/lz4/lz4) 和
  [Zstandard](https://github.com/facebook/zstd)，以及打包进页面的
  [Preact](https://github.com/preactjs/preact) 及其
  [signals](https://github.com/preactjs/signals)。

截图中的游戏、引擎和地址都是虚构的
（`tools/screenshots/shoot.sh` 对照一个替身引擎渲染它们）。

## 许可证

GPL-3.0-or-later（[LICENSE](LICENSE)）。Alembic 移植了多个 GPL、LGPL、MIT 和 BSD
项目的代码；[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 列出了每个项目、
从中取用了什么，以及它的许可证。
