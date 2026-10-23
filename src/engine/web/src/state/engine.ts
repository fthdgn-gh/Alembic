// SPDX-License-Identifier: GPL-3.0-or-later
// What the engine says about itself: drives, places, FTP servers, paired
// engines. Signals: components that read one redraw when it changes.
import { signal, computed, batch } from "@preact/signals";
import type { Info, Space, Location, FtpServer, PeerDrive, Peers } from "../api/types";
import { api } from "../api/client";
import { lang } from "../lib/i18n";

export const info = signal<Info | null>(null);
export const space = signal<Space[]>([]);
export const locations = signal<Location[]>([]);
export const ftp = signal<FtpServer[]>([]);
export const peerDrives = signal<PeerDrive[]>([]);
export const peers = signal<Peers | null>(null);
export const notConnected = signal<string | null>(null); // why the engine can't be reached
export const answered = signal(false); // the engine has answered once
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
