// SPDX-License-Identifier: GPL-3.0-or-later
// Sizes, times and paths as people read them.
import { t, lang } from "./i18n";

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
