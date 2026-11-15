// SPDX-License-Identifier: GPL-3.0-or-later
// Dumping the console's own games, at the top of Images (engines from
// 0.6.6, on a console): the game running now ("Dump…"), the next game that
// starts, and the installed games (a sheet of them: "Dump when launched").
// A game with a dump in the queue leads to that job instead.
import { useEffect } from "preact/hooks";
import type { InstalledGame, Job } from "../../api/types";
import { info } from "../../state/engine";
import { jobs } from "../../state/jobs";
import { tab } from "../../state/view";
import { runningGames, installedGames, loadRunning, loadInstalled, gameIconUrl, versionText, dumpJobOf, isPs4 } from "../../state/games";
import { t, tMsg } from "../../lib/i18n";
import { Ico } from "../../components/Icons";
import { Sheet, openOverlay, closeOverlay } from "../../components/Overlay";
import { goBack } from "../../input/history";
import { openDump } from "../../dialogs/ConvertDialog";
import { showJob } from "../queue/jobActions";
import { focusImage } from "./ImagesPage";

// One game's row: icon, title, what it is; on the right the dump's state, or what ✕ does.
function GameRow(props: { id: string; title: string; version: string; icon: boolean; sub: string; job: Job | null; action: string;
  disabled?: boolean; onChoose: () => void }) {
  const p = props;
  return (
    <div class={"row grow" + (p.disabled ? " off" : "")} tabIndex={p.disabled ? -1 : 0} data-key={"game:" + p.id}
      onClick={() => { if (!p.disabled) p.onChoose(); }} onFocus={() => { focusImage.value = null; }}>
      <Ico kind="game" label={t("GAME")} icon={p.icon ? gameIconUrl(p.id) : null} />
      <div style="min-width:0">
        <div class="t ellip">{p.title || p.id}</div>
        <div class="s ellip">{[p.sub, p.version ? "v" + versionText(p.version) : "", p.id].filter(Boolean).join("  ·  ")}</div>
      </div>
      <div class="sz">{p.job ? <span class={"state " + p.job.state}>{t(p.job.state)}</span> : p.action ? <span class="go">{p.action}</span> : null}</div>
    </div>
  );
}

// "Dump when launched": every installed game; PS4 ones say why not.
export async function openInstalled() {
  await loadInstalled();
  const v = await openOverlay<{ game?: InstalledGame; job?: number }>(() => <Installed />, { hints: [["x", t("Choose")], ["o", t("Back"), goBack]] });
  if (!v) return;
  if (v.job) showJob(v.job);
  else if (v.game) openDump({ id: v.game.title_id, title: v.game.title, version: v.game.version, icon: v.game.icon, running: v.game.running });
}
function Installed() {
  const data = installedGames.value, all = jobs.value;
  // Running ones first, then by title; PS4 ones (not dumpable) last.
  const list = (data ? data.games : []).slice().sort((a, b) => (a.dumpable === b.dumpable ? 0 : a.dumpable ? -1 : 1)
    || (a.running === b.running ? 0 : a.running ? -1 : 1) || (a.title || a.title_id).localeCompare(b.title || b.title_id));
  return (
    <Sheet title={t("Installed games")} cls="games-sheet" foot={<button class="btn" onClick={() => closeOverlay(null)}>{t("Close")}</button>}>
      <p class="muted" style="margin:0 0 .6rem">{t("Choose a game to dump when you start it; one that runs now is dumped once it has run for 30 seconds.")}</p>
      {!data ? <div class="empty">{t("Reading the games…")}</div>
        : !list.length ? <div class="empty">{t("No installed games found.")}</div>
        : list.map((g) => {
          const job = g.job ? all.filter((j) => !j.engine && j.id === g.job)[0] || dumpJobOf(all, g.title_id) : dumpJobOf(all, g.title_id);
          return <GameRow id={g.title_id} title={g.title} version={g.version} icon={g.icon} job={job} disabled={!g.dumpable}
            sub={!g.dumpable ? tMsg(g.why || "") : g.running ? t("Running now") : ""}
            action={!g.dumpable ? "" : g.running ? t("Dump…") : t("Dump when launched…")}
            onChoose={() => closeOverlay(job ? { job: job.id } : { game: g })} />;
        })}
    </Sheet>
  );
}

export function Games() {
  const running = runningGames.value, inst = installedGames.value, all = jobs.value;
  const here = !!info.value && info.value.platform === "ps5";
  // Asked when the tab opens (main.tsx asks for the running ones every five seconds).
  useEffect(() => {
    if (tab.value !== "images" || !here) return;
    loadRunning();
    loadInstalled();
  }, [tab.value, here]);
  const supported = !!inst && inst.supported;
  if (!here || (!running.length && !supported)) return null;
  const nextJob = dumpJobOf(all, "next");
  return (
    <div class="games" data-zone="images-games">
      <div class="sect" style="margin:1rem .3rem .3rem">{t("Dump a game")}</div>
      {running.map((g) => {
        const job = dumpJobOf(all, g.title_id), ps4 = isPs4(g.title_id);
        return <GameRow id={g.title_id} title={g.title} version={g.version} icon={g.icon} job={job} disabled={ps4}
          sub={ps4 ? tMsg("PS4 games cannot be dumped yet") : t("Running now") + (g.update ? " · " + t("with its update") : "")} action={ps4 ? "" : t("Dump…")}
          onChoose={() => (job ? showJob(job.id) : openDump({ id: g.title_id, title: g.title, version: g.version, icon: g.icon, running: true }))} />;
      })}
      <div class="gbtns">
        <button class="btn sm" data-key="games:next" onFocus={() => { focusImage.value = null; }}
          onClick={() => (nextJob ? showJob(nextJob.id) : openDump({ id: "next", title: "", version: "", icon: false, running: false }))}>
          {nextJob ? t("Next-game dump: in the queue") : t("Dump the next game that starts…")}
        </button>
        {supported ? <button class="btn sm" data-key="games:installed" onFocus={() => { focusImage.value = null; }} onClick={openInstalled}>{t("Installed games…")}</button> : null}
      </div>
    </div>
  );
}
