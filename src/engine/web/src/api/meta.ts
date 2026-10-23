// SPDX-License-Identifier: GPL-3.0-or-later
// Game info (title, title ID, version, whether there's an icon) for the
// entries on screen: cached, and asked for two at a time.
import { signal } from "@preact/signals";
import type { Entry, Meta } from "./types";
import { api } from "./client";

const meta = { cache: new Map<string, Meta | null>(), queue: [] as { e: Entry; res: (m: Meta | null) => void }[], active: 0, waiting: new Map<string, Promise<Meta | null>>() };
export const metaVersion = signal(0); // bumped when the cache is cleared
export const metaKey = (e: { path: string; mtime?: number; size?: number }) => e.path + "|" + (e.mtime || 0) + "|" + (e.size || 0);
export const iconUrl = (e: { path: string; mtime?: number }) => "/api/icon?path=" + encodeURIComponent(e.path) + "&v=" + (e.mtime || 0);
export function clearMeta() { meta.cache.clear(); meta.waiting.clear(); metaVersion.value++; }
export function cachedMeta(e: Entry): Meta | null | undefined { return meta.cache.get(metaKey(e)); }

export function getMeta(e: Entry): Promise<Meta | null> {
  const k = metaKey(e);
  if (meta.cache.has(k)) return Promise.resolve(meta.cache.get(k) as Meta | null);
  const w = meta.waiting.get(k);
  if (w) return w;
  const p = new Promise<Meta | null>((res) => { meta.queue.push({ e, res }); pump(); });
  meta.waiting.set(k, p);
  return p;
}
function pump() {
  while (meta.active < 2 && meta.queue.length) {
    const job = meta.queue.shift()!;
    meta.active++;
    api<Meta>("GET", "/api/meta?path=" + encodeURIComponent(job.e.path))
      .then((m) => { meta.cache.set(metaKey(job.e), m); job.res(m); })
      .catch(() => { meta.cache.set(metaKey(job.e), null); job.res(null); })
      .then(() => { meta.waiting.delete(metaKey(job.e)); meta.active--; pump(); });
  }
}

