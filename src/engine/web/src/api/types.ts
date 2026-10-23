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
  lz4_runtime: { path: string; present: boolean; sha256: string; pinned: boolean };
  lz4_profiles: { id: string; label: string; tested: string }[];
  busy: boolean;
  launcher: { supported: boolean; installed: boolean; title_id: string; status: string; off: boolean };
  cache: { games: number; bytes: number };
  port: number;
  enter_button: string; // "circle" where ○ confirms
  lang: string;
  protocol?: number; // how it talks to other engines (peers.hpp kPeerProtocol); none before v0.2.1
}

export interface Space { root: string; total: number; free: number }

// A suggested destination: this engine's, an FTP server, or a paired engine's.
export interface Location {
  label: string;
  path: string;
  exists: boolean;
  ftp?: boolean;
  peer?: string;
  engine?: string;
  place?: string;
  custom?: boolean;
}

export interface FtpServer { id: string; name: string; host: string; port: number; user: string; path: string }

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
  ftp?: boolean;
  peer?: string;
  shared?: boolean; // a file shared from this computer's browser
}

export interface Listing { path: string; parent: string | null; entries: Entry[]; free?: number; total?: number }

export interface Meta { title: string; title_id: string; content_id: string; version: string; icon: boolean }

export interface Inspect { path: string; format: string; title_id?: string; files: number; bytes: number; lz4_packed: boolean; lz4_profile: string; engine: string }

export interface Spec {
  kind: string;
  input: string;
  output: string;
  output_dir?: string;
  to: string;
  level: number;
  lz4: boolean;
  lz4_profile: string;
  lz4_level: number;
  lz4_unpack: boolean;
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
}

export interface Plan {
  output: string;
  needs?: Dep[];
  warnings?: string[];
  ampr_needed?: boolean;
  ships_emulator?: boolean;
  imports_ampr?: boolean;
}
