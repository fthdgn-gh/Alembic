// SPDX-License-Identifier: GPL-3.0-or-later
// Files shared from this computer's browser (not on the console): the
// console reads a picked file through this page. It asks for pieces (want,
// a long poll) and the page posts them, so the page must stay open.
import { signal } from "@preact/signals";
import type { Entry } from "../../api/types";
import { api } from "../../api/client";
import { jobs, isActive } from "../../state/jobs";
import { IS_PS5 } from "../../lib/env";

interface Share { path: string; file: File; sent: number }
export const shares = signal<Share[]>([]);
export const isShared = (path: string) => typeof path === "string" && path.indexOf("/remote/") === 0;
export const isSharedHere = (path: string) => shares.value.some((s) => s.path === path);

export function kindOfName(name: string): Entry["kind"] {
  const n = name.toLowerCase();
  for (const [ext, kind] of [[".pkg", "fpkg"], [".exfat", "exfat"], [".ffpfsc", "ffpfsc"], [".ffpkg", "ffpkg"], [".ffpfs", "ffpfs"]] as [string, Entry["kind"]][])
    if (n.slice(-ext.length) === ext) return kind;
  return "file";
}
async function relay(share: Share, id: string) {
  for (;;) {
    let want: { pieces?: { offset: number; length: number }[] };
    try { want = await api("POST", "/api/remote/" + id + "/want"); }
    catch (e) { await new Promise((r) => setTimeout(r, 2000)); continue; }
    for (const p of want.pieces || []) {
      const blob = share.file.slice(p.offset, p.offset + p.length);
      for (let attempt = 0; attempt < 3; attempt++) {
        try {
          const r = await fetch("/api/remote/" + id + "/data?offset=" + p.offset, { method: "POST", body: blob });
          if (r.ok) { share.sent += p.length; break; }
        } catch (e) { await new Promise((res) => setTimeout(res, 1000)); }
      }
    }
    shares.value = shares.value.slice(); // the bar shows what was sent
  }
}
export async function shareFile(file: File) {
  const r = await api<{ path: string }>("POST", "/api/remote", { name: file.name, size: file.size, last_modified: file.lastModified });
  const known = shares.value.find((s) => s.path === r.path);
  if (!known) {
    const share = { path: r.path, file, sent: 0 };
    shares.value = shares.value.concat(share);
    const id = r.path.split("/")[2];
    for (let i = 0; i < 3; i++) relay(share, id); // three pieces in flight
  } else {
    known.file = file;
    shares.value = shares.value.slice();
  }
  return r.path;
}
if (!IS_PS5) window.addEventListener("beforeunload", (ev) => {
  const busy = jobs.value.some((j) => isActive(j) && isShared(j.spec.input) && isSharedHere(j.spec.input));
  if (busy) { ev.preventDefault(); ev.returnValue = ""; }
});
