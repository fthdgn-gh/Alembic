// SPDX-License-Identifier: GPL-3.0-or-later
// The console's own games, for dumping (engines from 0.6.6): the ones
// running now, the installed ones, and each dump's game by title ID for the
// Queue. Older engines answer 404: then there is nothing to show.
import { signal } from "@preact/signals";
import type { Job, RunningGame, InstalledGames, GameInfo } from "../api/types";
import { api } from "../api/client";
import { info } from "./engine";
import { t } from "../lib/i18n";

export const runningGames = signal<RunningGame[]>([]);
export const installedGames = signal<InstalledGames | null>(null);

export const gameIconUrl = (id: string) => "/api/games/icon?id=" + encodeURIComponent(id);
// Versions as the console shows them: "01.004.000" -> "1.004.000".
export const versionText = (v: string) => v.replace(/^0+(?=\d)/, "");

// Only a console engine has games to dump.
const onConsole = () => !!info.value && info.value.platform === "ps5";

export async function loadRunning() {
  if (!onConsole()) { if (runningGames.value.length) runningGames.value = []; return; }
  try {
    const r = await api<{ games: RunningGame[] }>("GET", "/api/games/running");
    const list = r.games || [];
    if (JSON.stringify(list) !== JSON.stringify(runningGames.value)) runningGames.value = list;
  } catch (e) { if (runningGames.value.length) runningGames.value = []; }
}

export async function loadInstalled() {
  if (!onConsole()) { installedGames.value = null; return; }
  try { installedGames.value = await api<InstalledGames>("GET", "/api/games/installed"); }
  catch (e) { installedGames.value = null; }
}

// A game's title, version and icon by title ID, asked once and kept.
const infos = new Map<string, GameInfo | null>();
const asking = new Set<string>();
export const gameInfoVersion = signal(0); // bumped when one arrives
export function gameInfo(id: string): GameInfo | null | undefined {
  if (!id || id === "next") return null;
  if (infos.has(id)) return infos.get(id);
  if (!asking.has(id)) {
    asking.add(id);
    api<GameInfo>("GET", "/api/games/info?id=" + encodeURIComponent(id))
      .then((g) => { infos.set(id, g); }, () => { infos.set(id, null); })
      .then(() => { asking.delete(id); gameInfoVersion.value++; });
  }
  return undefined;
}

// A dump's game in words: its title once known, else its title ID; the
// next-game placeholder until one starts. Another engine's dumps are not
// this engine's games: their ID only.
export function dumpTitle(j: Job): string {
  const id = j.spec.game || "";
  if (id === "next") return t("Next game that starts");
  const g = j.engine ? null : gameInfo(id);
  return g && g.title ? g.title : id;
}

// PS4 games run through backward compatibility: not dumped yet (the engine refuses them).
export const isPs4 = (id: string) => id.indexOf("CUSA") === 0;

// The dump of a title (or of "next") queued, running or paused here.
export function dumpJobOf(jobs: Job[], id: string): Job | null {
  for (const j of jobs) if (!j.engine && j.spec.game === id && (j.state === "queued" || j.state === "running" || j.state === "paused")) return j;
  return null;
}
