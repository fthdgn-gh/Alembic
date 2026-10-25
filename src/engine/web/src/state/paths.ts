// SPDX-License-Identifier: GPL-3.0-or-later
// Which drive, FTP server or paired engine a path is on, and how to name it.
import type { Space, PeerDrive, FtpServer } from "../api/types";
import { info, space, ftp, peers, peerDrives } from "./engine";
import { t, placeLabel, setPathNamer } from "../lib/i18n";
import { base } from "../lib/format";

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
// A paired engine that is offline, when `path` is on it.
export function offlinePeer(path: string | null | undefined) {
  if (!path || String(path).indexOf("/peer/") !== 0) return null;
  const id = path.slice(6).split("/")[0];
  return ((peers.value && peers.value.engines) || []).find((e) => e.id === id && e.paired && e.offline) || null;
}
// "/peer/<id>" of a path on another engine, else "".
export function peerRoot(path: string): string {
  return String(path).indexOf("/peer/") === 0 ? "/peer/" + path.slice(6).split("/")[0] : "";
}

export function driveName(root: string): string {
  let m;
  const f = ftpOf(root);
  if (f && f.path === root) return f.name;
  const pd = peerDrives.value.find((d) => d.root === root);
  if (pd) return pd.engine + " · " + placeLabel(pd.label);
  // A whole engine (one that is offline: its drives are not known).
  if (peerRoot(root) === root) { const e = ((peers.value && peers.value.engines) || []).find((x) => "/peer/" + x.id === root); return e ? e.name : t("another engine"); }
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

// This engine's name where several engines' places are listed.
export const hereName = () => (info.value && info.value.platform === "ps5" ? "PS5" : (info.value && info.value.host_name) || t("This engine"));
