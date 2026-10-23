// SPDX-License-Identifier: GPL-3.0-or-later
// What the page knows, as signals: components that read one redraw when it changes.
import { signal, computed, batch } from "@preact/signals";
import type { Info, Space, Location, FtpServer, PeerDrive, Peers, Listing, Entry, Job } from "./types";
import { api, base } from "./util";
import { t, lang, placeLabel, setPathNamer } from "./i18n";

export const TABS = ["files", "queue", "deps", "settings"] as const;
export type Tab = (typeof TABS)[number];

export const info = signal<Info | null>(null);
export const space = signal<Space[]>([]);
export const locations = signal<Location[]>([]);
export const ftp = signal<FtpServer[]>([]);
export const peerDrives = signal<PeerDrive[]>([]);
export const peers = signal<Peers | null>(null);

export const tab = signal<Tab>("files");
export const cwd = signal<string | null>(null);
export const listing = signal<Listing | null>(null);
export const focusEntry = signal<Entry | null>(null); // the focused row in Files
export const jobs = signal<Job[]>([]); // this engine's, then other engines'
export const notConnected = signal<string | null>(null); // why the engine can't be reached
export const answered = signal(false); // the engine has answered once
export const pad = signal(/PlayStation/i.test(navigator.userAgent)); // show controller glyphs

export const CONVERTIBLE = ["fpkg", "exfat", "ffpfsc", "ffpkg", "ffpfs"];
export const KIND_LABEL: Record<string, string> = { fpkg: "PKG", exfat: "EXFAT", ffpfsc: "FFPFSC", ffpkg: "FFPKG", ffpfs: "FFPFS", folder: "FOLDER", file: "FILE" };
export const isConvertible = (e: Entry) => CONVERTIBLE.indexOf(e.kind) >= 0 || !!e.game;
// Running, or still to run (queued or paused), in the order they run.
export const isActive = (j: Job) => j.state === "running" || j.state === "queued" || j.state === "paused";

// Consoles set to confirm with ○ (the engine reads the system setting). The
// browser already sends the keys to match; the page only shows the matching
// glyphs, and reads raw gamepad buttons (physical ✕ and ○) the same way.
export const circleConfirms = () => pad.value && !!info.value && info.value.enter_button === "circle";

// This engine's name where several engines' places are listed.
export const hereName = () => (info.value && info.value.platform === "ps5" ? "PS5" : (info.value && info.value.host_name) || t("This engine"));

export function rootOf(path: string): (Space & { engine?: string }) | (PeerDrive & { free: number }) | null {
  let best: any = null;
  const all: any[] = (space.value as any[]).concat(peerDrives.value);
  for (const s of all) if (path === s.root || path.indexOf(s.root + "/") === 0) if (!best || s.root.length > best.root.length) best = s;
  return best;
}
// The paired engine's drive a path is on (null for this engine's own).
export function peerDriveOf(path: string): PeerDrive | null {
  const r = rootOf(path) as PeerDrive | null;
  return r && r.engine ? r : null;
}
// The FTP server a path is on (null for paths on the console).
export function ftpOf(path: string): FtpServer | null {
  for (const s of ftp.value) if (path === s.path || String(path).indexOf(s.path + "/") === 0) return s;
  return null;
}
export function driveName(root: string): string {
  let m;
  const f = ftpOf(root);
  if (f && f.path === root) return f.name;
  const pd = peerDrives.value.find((d) => d.root === root);
  if (pd) return pd.engine + " · " + placeLabel(pd.label);
  if (root === "/data") return t("Internal");
  if ((m = /^\/mnt\/usb(\d+)$/.exec(root))) return t("USB {n}", { n: m[1] });
  if ((m = /^\/mnt\/ext(\d+)$/.exec(root))) return t("Extended {n}", { n: m[1] });
  return base(root) || root;
}

// A path as people read it: another engine's "/peer/<id>/data/x" is
// "PS5: /data/x" (paths keep their real form everywhere else).
export function shownPath(p: string): string;
export function shownPath(p: string | null | undefined): string | null | undefined;
export function shownPath(p: string | null | undefined) {
  if (!p || String(p).indexOf("/peer/") !== 0) return p;
  const rest = p.slice(6), slash = rest.indexOf("/"), id = slash < 0 ? rest : rest.slice(0, slash);
  const e: any = ((peers.value && peers.value.engines) || []).find((x) => x.id === id) || peerDrives.value.find((x) => x.id === id);
  const name = e ? e.name || e.engine : t("another engine");
  let local = slash < 0 ? "/" : rest.slice(slash);
  if (/^\/[A-Za-z]:\//.test(local)) local = local.slice(1); // a Windows engine's C:/...
  return name + ": " + local;
}
// Another engine's paths inside a message ("... exists: /peer/<id>/data/x").
setPathNamer((text) => String(text).replace(/\/peer\/[0-9a-f]{32}(?:\/[^\s'"),;]*)?/g, (m) => shownPath(m)));

export const anyPaired = computed(() => !!peers.value && peers.value.engines.some((e) => e.paired));

export async function loadInfo() {
  const i = await api<Info>("GET", "/api/info");
  // The engine's console notifications follow the page's language.
  if (i.lang && i.lang !== lang.value) api("POST", "/api/lang", { lang: lang.value }).catch(() => {});
  // Asked one after another, as the engine serves one request at a time on the console.
  const sp = await api<Space[]>("GET", "/api/fs/space").catch(() => []);
  const loc = await api<Location[]>("GET", "/api/locations").catch(() => []);
  const f = await api<FtpServer[]>("GET", "/api/ftp").catch(() => []);
  const pd = await api<PeerDrive[]>("GET", "/api/peer-drives").catch(() => peerDrives.value);
  const p = await api<Peers>("GET", "/api/peers").catch(() => peers.value);
  batch(() => { info.value = i; space.value = sp; locations.value = loc; ftp.value = f; peerDrives.value = pd; peers.value = p; notConnected.value = null; });
  return i;
}

export async function loadPeers() {
  peers.value = await api<Peers>("GET", "/api/peers");
}
