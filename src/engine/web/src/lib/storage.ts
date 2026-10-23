// SPDX-License-Identifier: GPL-3.0-or-later
// Per-browser settings; storage can be missing (private windows).
export const store = {
  get<T = unknown>(k: string): T | null {
    try { return JSON.parse(localStorage.getItem("alembic." + k) as string) as T; } catch (e) { return null; }
  },
  set(k: string, v: unknown) {
    try { localStorage.setItem("alembic." + k, JSON.stringify(v)); } catch (e) { /* not kept */ }
  },
};
