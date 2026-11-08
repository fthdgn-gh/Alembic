// SPDX-License-Identifier: GPL-3.0-or-later
// What the engine's API sends (src/engine/main.cpp).

export interface Info {
  id: string;
  name: string;
  version: string;
  platform: string; // "ps5" or "host"
  os: string;
  host_name: string;
  cpu_instructions: string;
  state_dir: string;
  network_urls: string[];
  local_url: string;
  open_to_network: boolean;
  roots: string[];
  lz4_runtime: { path: string; present: boolean; sha256: string; pinned: boolean; name?: string };
  lz4_profiles: { id: string; label: string; tested: string }[];
  busy: boolean;
  launcher: { supported: boolean; installed: boolean; title_id: string; status: string; off: boolean };
  cache: { games: number; bytes: number };
  port: number;
  enter_button: string; // "circle" where ○ confirms
  lang: string;
  protocol?: number; // how it talks to other engines (peers.hpp kPeerProtocol); none before v0.2.1
  update?: { state: string; latest: string | null; newer: boolean; app_pending?: boolean }; // none before v0.2.3
  can_restart?: boolean; // a PS5 engine can start itself again (none before v0.3.0)
  started?: number; // when this engine started (ms): changes with a restart
}

// GET /api/update (update.hpp): this version, GitHub's latest, payload files found.
export interface UpdatePayload { path: string; version: string; rename_to: string; loader: string }
export interface UpdateStatus {
  current: string;
  state: string; // idle, checking, searching, downloading, replacing, starting
  error: string;
  checked: number; // Unix seconds, 0 = never
  can_install: boolean; // the engine runs on a console with an ELF loader
  latest: { version: string; notes: string; url: string } | null;
  newer: boolean;
  searched: string; // "no", "usual", "everywhere"
  payloads: UpdatePayload[];
  // The console app (0.3.0 and later; null until looked for, or on a PC).
  app?: { path: string; image: boolean; version: string; in_release: boolean; current: boolean } | null;
  app_pending?: boolean; // a new app image waits for the app to close
}

export interface Space { root: string; total: number; free: number }

// A suggested destination: this engine's, a network server, or a paired engine's.
export interface Location {
  label: string;
  path: string;
  exists: boolean;
  ftp?: boolean; // on a network server (FTP, SMB or WebDAV: the name is from when there was only FTP)
  server?: Protocol;
  peer?: string;
  engine?: string;
  place?: string;
  custom?: boolean;
}

// A network server: /api/servers. `root` is the SMB share ("Games/PS5") or
// the WebDAV path ("/dav") or the NFS export; `path` is where its files are
// (/ftp/ID, /smb/ID, /dav/ID, /nfs/ID). For NFS, `user` is "uid:gid".
export type Protocol = "ftp" | "smb" | "webdav" | "nfs";
export interface NetServer {
  id: string; protocol: Protocol; name: string; host: string; port: number; user: string; root: string; path: string;
  tls?: boolean; // FTPS / HTTPS (engines before TLS send none)
  fingerprint?: string; // the trusted certificate's SHA-256
}

// A paired engine's drive; `root` is /peer/<id>/<its path>.
export interface PeerDrive { id: string; engine: string; platform: string; root: string; label: string; free: number; total?: number }

export interface Peer {
  id: string;
  name: string;
  os: string;
  platform: string;
  version: string;
  address: string;
  port: number;
  url: string;
  manual: boolean;
  reachable: boolean;
  seen_ago: number;
  paired: boolean;
  waiting_for_code: boolean;
  offline?: boolean; // paired, but not answering (or no longer announced: then no address)
  problem?: string;
  protocol?: number; // 1 when it does not say (v0.2.0)
}
export interface Peers { enabled: boolean; listening?: boolean; engines: Peer[] }

export type Kind = "folder" | "file" | "fpkg" | "exfat" | "ffpfsc" | "ffpkg" | "ffpfs";

export interface Entry {
  name: string;
  path: string;
  kind: Kind;
  size?: number;
  mtime?: number;
  game?: boolean; // a game folder
  ftp?: boolean; // on a network server
  server?: Protocol;
  peer?: string;
  shared?: boolean; // a file shared from this computer's browser
}

export interface Listing { path: string; parent: string | null; entries: Entry[]; free?: number; total?: number }

export interface Meta { title: string; title_id: string; content_id: string; version: string; icon: boolean; platform?: string; packs?: string /* "lz4", "zstd" or "" (0.4.3) */ }

export interface Inspect { path: string; format: string; title_id?: string; files: number; bytes: number; lz4_packed: boolean; lz4_profile: string; engine: string; zstd_packed?: boolean }

export interface Spec {
  kind: string;
  input: string;
  output: string;
  output_dir?: string;
  to: string;
  level: number;
  pkg_compression?: string; // "stored" or "kraken"
  install_after?: boolean; // FPKG: install it, then delete it
  keep_package?: boolean; // with install_after: do not delete it
  lz4: boolean;
  lz4_profile: string;
  lz4_level: number;
  lz4_unpack: boolean;
  zstd?: boolean;       // zstd packs (ampr-emu-ng), engines from 0.4.3
  zstd_level?: number;
  zstd_unpack?: boolean;
  delete_source: boolean;
  verify: boolean;
  overwrite: boolean;
  ampr_emu: string;
}

export interface JobResult {
  output?: string;
  method?: string;
  output_bytes?: number;
  source_deleted?: boolean;
  source_kept?: string;
  source_hidden?: string;
  smp_source?: string;
  ampr_emulator?: string;
  warnings?: string[];
  lz4?: { files_packed: number; files_total: number; packed_bytes: number; volume_bytes: number; profile?: { notes?: string[] } };
  zstd?: { level: number; files_packed: number; files_total: number; packed_bytes: number; volume_bytes: number; warnings?: string[] };
}

export type JobState = "queued" | "running" | "paused" | "done" | "failed" | "cancelled" | "interrupted";

export interface Job {
  id: number;
  spec: Spec;
  state: JobState;
  stage: string;
  done: number;
  total: number;
  created: number;
  started: number;
  finished: number;
  error: string;
  result: JobResult | null;
  elapsed?: number;
  keeps_progress?: boolean;
  checkpoint?: { done: number; total: number };
  engine?: { id: string; name: string }; // a job on another engine
  raw?: Job; // that job with its own paths, as its engine sent it
}

export interface DepFile { path: string; url: string; sha256: string; size: number }
export interface Dep {
  id: string;
  label: string;
  purpose: string;
  source: string;
  license: string;
  state: string;
  files: DepFile[];
  bytes: number;
  group?: string; // "ampr_runtime": one of the group is in use
  in_use?: boolean;
  // A followed release (the AMPR emulators; engines from 0.4.3)
  follows?: string;     // "owner/name" on GitHub
  version?: string;     // downloaded ("" when not)
  latest?: string;      // from the last check ("" before one)
  update?: boolean;     // a newer build than the one downloaded
  check_error?: string;
}

export interface PlanEmulator {
  id: string;
  label: string;
  name: string;         // "ampr-emu-ng 0.2.0"
  downloaded: boolean;
  in_use: boolean;      // in Components
  in_game: boolean;     // the game already runs with this build
  reads_packs: boolean; // false for ampr_emu on a zstd-packed game
}

export interface DepsStatus { auto_update: boolean; checking: boolean }

export interface Plan {
  output: string;
  needs?: Dep[];
  warnings?: string[];
  ampr_needed?: boolean;
  ships_emulator?: boolean;
  imports_ampr?: boolean;
  unsigned_executables?: number; // plain PS5 ELFs: fake-signed on the way
  // "ampr_emu" plans: the libSceAmpr.sprx the game runs with (its backport's, for an image)
  emulator_present?: boolean;
  emulator_name?: string;    // "ampr_emu 0.4.2.1" when it is a known build, else ""
  emulator_current?: boolean; // it is the one in use in Components
  runtime_name?: string;     // the one in use in Components
  zstd_packs?: boolean;
  lz4_packs?: boolean;
  emulators?: PlanEmulator[]; // every AMPR emulator, to switch the game to another (0.4.3)
}

// GET /api/names (name_formats.hpp): the formats Rename suggests names with.
export type Spaces = "keep" | "none" | "_" | "." | "-";
export type Letters = "keep" | "upper" | "lower";
export interface NameFormat { pattern: string; spaces: Spaces; letters: Letters; sample?: string; warning?: string }
export interface NameSuggestion { name: string; pattern: string; warning: string; shortened?: boolean }

// GET /api/images (0.5.5): every image, game folder and package in
// ShadowMount+'s folders, as Files lists them, with each game's AMPR emulator.
export type AmprStatus = "current" | "update" | "missing" | "unknown" | "not_downloaded" | "none" | "unreadable";
export interface ImageAmpr {
  status: AmprStatus;
  emulator?: { id: string; name: string; version: string } | null;
  where?: "game" | "backport" | "";
  target?: string; // the emulator (component id) an Update or Add uses
  zstd_packs?: boolean;
}
export interface ImageItem extends Entry {
  folder: string;  // the folder it is in (the list groups by it)
  hidden: boolean; // ".name": ShadowMount+ skips it
  allowed: boolean; // inside the engine's folders (jobs can reach it)
  ampr: ImageAmpr | null; // null for packages
}
export interface ImagesEmulator { id: string; label: string; name: string; version: string; latest: string; downloaded: boolean; in_use: boolean }
export interface ImagesList {
  images: ImageItem[];
  dirs: string[]; // the folders searched (those that exist)
  smp: boolean;   // ShadowMount+'s API answered
  refreshing: boolean;
  updated: number;
  smp_update_emulators: boolean | null;
  ampr?: boolean; // this engine reads games for their AMPR emulator (a PS5 one)
  emulators: ImagesEmulator[];
}
// GET /api/peers/images: each paired engine's list, its paths as this
// engine reaches them (/peer/<id>/...), without AMPR state.
export interface PeerImages {
  engine: { id: string; name: string; platform: string };
  state: "ok" | "offline" | "old" | "error";
  error?: string;
  images?: ImageItem[];
  dirs?: string[];
  refreshing?: boolean;
}
