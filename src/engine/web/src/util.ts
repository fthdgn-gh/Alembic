// SPDX-License-Identifier: GPL-3.0-or-later
import { t, lang } from "./i18n";

export const IS_PS5 = /PlayStation/i.test(navigator.userAgent);
// Started there, a pairing code is shown here: only in a browser on this computer.
export const LOCAL_PAGE = /^(127\.0\.0\.1|localhost)$/.test(location.hostname);

// Per-browser settings; storage can be missing (private windows).
export const store = {
  get<T = unknown>(k: string): T | null {
    try { return JSON.parse(localStorage.getItem("alembic." + k) as string) as T; } catch (e) { return null; }
  },
  set(k: string, v: unknown) {
    try { localStorage.setItem("alembic." + k, JSON.stringify(v)); } catch (e) { /* not kept */ }
  },
};

export class ApiError extends Error {
  data: Record<string, unknown>;
  constructor(message: string, data: Record<string, unknown>) { super(message); this.data = data; }
}

export async function api<T = any>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(path, { method, headers: body ? { "Content-Type": "application/json" } : {}, body: body ? JSON.stringify(body) : undefined });
  const text = await res.text();
  let data: any = {};
  try { data = text ? JSON.parse(text) : {}; } catch (e) { data = { error: text }; }
  if (!res.ok) throw new ApiError(data.error || res.statusText, data);
  return data as T;
}

export function bytes(n: number | null | undefined): string {
  if (n == null) return "";
  const u = ["B", "KB", "MB", "GB", "TB"];
  let i = 0;
  while (n >= 1000 && i < u.length - 1) { n /= 1000; i++; }
  return (i ? n.toFixed(n < 10 ? 2 : n < 100 ? 1 : 0) : n) + " " + u[i];
}

export function duration(s: number): string {
  s = Math.max(0, Math.round(s));
  const hh = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), r = s % 60;
  return hh ? t("{h} h {m} min", { h: hh, m }) : m ? (r ? t("{m} min {s} s", { m, s: r }) : t("{m} min", { m })) : t("{s} s", { s: r });
}

export function date(when: number | undefined, full?: boolean): string {
  if (!when) return "";
  const d = new Date(when * 1000), now = new Date();
  const time = d.toLocaleTimeString(lang.value, { hour: "2-digit", minute: "2-digit" });
  if (d.toDateString() === now.toDateString() && !full) return time;
  const day = d.toLocaleDateString(lang.value, { year: "numeric", month: "short", day: "numeric" });
  return full ? day + " " + time : day;
}

export const base = (p: string) => p.replace(/\/+$/, "").split("/").pop() || "";
export const dir = (p: string) => { const s = p.replace(/\/+$/, ""); const i = s.lastIndexOf("/"); return i <= 0 ? "/" : s.slice(0, i); };

export const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));
