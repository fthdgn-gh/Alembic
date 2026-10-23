// SPDX-License-Identifier: GPL-3.0-or-later
// The queue: jobs running or waiting, then the finished ones. Jobs that
// other engines run for this one are listed too ("on <engine>").
import { useEffect, useState } from "preact/hooks";
import type { Job, Meta } from "./types";
import { jobs, tab, cwd, anyPaired, info, isActive, notConnected, answered, shownPath, CONVERTIBLE, KIND_LABEL, loadInfo } from "./state";
import { api, base, bytes, date, dir, duration } from "./util";
import { t, tMsg, tMethod } from "./i18n";
import { Ico, getMeta, iconUrl } from "./ui";
import { choose, overlays } from "./overlay";
import { toast } from "./toasts";
import { browse, toggleHidden, isShared, isSharedHere } from "./files";
import { showTab } from "./app";

export const jobKey = (j: Job) => (j.engine ? j.engine.id + ":" + j.id : String(j.id));
const jobPath = (j: Job) => (j.engine ? "/api/peers/" + encodeURIComponent(j.engine.id) + "/jobs/" + j.id : "/api/jobs/" + j.id);

// Another engine's job with its paths as this engine names them.
function fromEngine(eid: string, p: string | undefined) {
  if (!p) return p;
  const self = info.value && info.value.id ? "/peer/" + info.value.id : null;
  if (self && (p === self || p.indexOf(self + "/") === 0)) return p.slice(self.length) || "/";
  if (p.indexOf("/peer/") === 0) return p;
  return "/peer/" + eid + (p[0] === "/" ? "" : "/") + p;
}
function localView(j: Job): Job {
  const id = j.engine!.id, sp: any = Object.assign({}, j.spec), r: any = j.result ? Object.assign({}, j.result) : j.result;
  for (const k of ["input", "output", "output_dir"]) sp[k] = fromEngine(id, sp[k]);
  if (r) for (const k of ["output", "source_hidden", "smp_source"]) if (r[k]) r[k] = fromEngine(id, r[k]);
  return Object.assign({}, j, { spec: sp, result: r, raw: j.raw || j });
}

// Speed per running job (job numbers repeat across engines). A sample counts
// when the progress moved: another engine's jobs come every second or so, so
// a refresh in between is no news, not a stop (after 10 s it is).
const speeds: Record<string, { t: number; done: number; rate: number; stage: string }> = {};
function sample(j: Job) {
  const key = jobKey(j), now = Date.now() / 1000, sp = speeds[key];
  if (sp && sp.stage === j.stage && j.done >= sp.done && now - sp.t > 0.5 && (j.done !== sp.done || now - sp.t > 10)) {
    const inst = (j.done - sp.done) / (now - sp.t);
    speeds[key] = { t: now, done: j.done, rate: sp.rate ? sp.rate * 0.75 + inst * 0.25 : inst, stage: j.stage };
  } else if (!sp || sp.stage !== j.stage) speeds[key] = { t: now, done: j.done, rate: 0, stage: j.stage };
}
const rateOf = (j: Job) => { const sp = speeds[jobKey(j)]; return sp && sp.stage === j.stage ? sp.rate : 0; };

let lastFinished: string | null = null, remoteJobs: Job[] = [], remoteAt = 0, remoteBusy = false;
export async function refreshJobs(remoteNow?: boolean) {
  let list: Job[];
  // Only a failed request means the engine is gone.
  try { list = await api<Job[]>("GET", "/api/jobs"); }
  catch (e) { notConnected.value = t("Engine not reachable"); return; }
  if (notConnected.value) notConnected.value = null;
  answered.value = true;
  // Other engines' jobs: every second while one of them works or waits, or
  // the queue is open; else every five seconds. One request at a time.
  const busy = tab.value === "queue" || remoteJobs.some(isActive);
  if (anyPaired.value && !remoteBusy && (remoteNow || Date.now() - remoteAt > (busy ? 900 : 5000))) {
    remoteAt = Date.now();
    remoteBusy = true;
    try { remoteJobs = ((await api<Job[]>("GET", "/api/peers/jobs").catch(() => null)) || remoteJobs.map((j) => j.raw!)).map(localView); }
    finally { remoteBusy = false; }
  } else if (!anyPaired.value) remoteJobs = [];
  list = list.concat(remoteJobs);
  for (const j of list) if (j.state === "running") sample(j);
  // Toast and refresh the listing when a job finishes.
  const finished = list.filter((j) => !isActive(j)).map((j) => jobKey(j) + j.state).join();
  if (lastFinished !== null && finished !== lastFinished) {
    const newest = list.filter((j) => j.finished).sort((a, b) => b.finished - a.finished)[0];
    if (newest && newest.state === "done") toast(t("Finished: {name}", { name: base(newest.spec.input) }), "ok");
    if (newest && newest.state === "failed") toast(t("Failed: {name}", { name: base(newest.spec.input) }), "bad");
    if (cwd.value && !overlays.value.length) browse(cwd.value, false);
    loadInfo().catch(() => {});
  }
  lastFinished = finished;
  jobs.value = list;
}

function jobText(j: Job) {
  const s = j.spec;
  if (s.kind === "copy" || s.kind === "move") return { title: base(s.input), to: t(s.kind === "copy" ? "Copy to {path}" : "Move to {path}", { path: shownPath(s.output) }) };
  if (s.kind === "install") return { title: base(s.input), to: t("Install on this console") };
  if (s.kind === "ampr_emu") return { title: base(s.input), to: j.result && j.result.output ? t("AMPR emulator into {path}", { path: shownPath(j.result.output) }) : t("Add the AMPR emulator") };
  return { title: base(s.input), to: "→ " + shownPath(s.output) };
}
// Where a waiting job continues from, in percent (null without a checkpoint).
const cpPct = (j: Job) => (j.checkpoint && j.checkpoint.total ? Math.floor(100 * j.checkpoint.done / j.checkpoint.total) : null);
const cpText = (j: Job) => (cpPct(j) == null ? "" : t("{n}%", { n: cpPct(j)! }));

// The game's title, title ID, version and icon, as in the file list: read
// from the input, or from the output once the input is gone.
function useJobMeta(j: Job): (Meta & { from: string }) | null {
  const [m, setM] = useState<(Meta & { from: string }) | null>(null);
  const out = j.result && j.result.output ? j.result.output : null;
  const second = j.state === "done" && out && out !== j.spec.input ? out : null;
  useEffect(() => {
    let live = true;
    const tryPath = (path: string, next: string | null) => getMeta({ path, mtime: 0, size: 0, name: "", kind: "file" }).then((x) => {
      if (!live) return;
      if (x) setM(Object.assign({ from: path }, x)); else if (next) tryPath(next, null);
    });
    if (!m) tryPath(j.spec.input, second);
    return () => { live = false; };
  }, [j.spec.input, second]);
  return m;
}

function JobCard(props: { j: Job }) {
  const j = props.j, s = j.spec, r = j.result || {}, jt = jobText(j);
  const m = useJobMeta(j);
  const pct = j.total ? Math.min(100, 100 * j.done / j.total) : 0;
  let detail = "";
  if (j.state === "running") {
    const rate = rateOf(j);
    detail = [t(j.stage), j.total ? t("{n}%", { n: pct.toFixed(1) }) : "", rate > 1 ? bytes(rate) + "/s" : "",
      rate > 1 && j.total ? t("{time} left", { time: duration((j.total - j.done) / rate) }) : "", j.elapsed != null ? t("{time} so far", { time: duration(j.elapsed) }) : ""].filter(Boolean).join("  ·  ");
  } else if (j.state === "done") {
    detail = [tMethod(r.method), r.output_bytes ? bytes(r.output_bytes) : "", j.elapsed != null ? duration(j.elapsed) : "", r.source_deleted ? t("source deleted") : ""].filter(Boolean).join("  ·  ");
  } else if (j.state === "queued") detail = cpText(j) ? t("Waiting · continues from {p}", { p: cpText(j) }) : t("Waiting for the job before it");
  else if (j.state === "paused") detail = cpText(j) ? t("Paused at {p} · resume to continue", { p: cpText(j) }) : t(j.stage || "Paused");
  else detail = j.finished ? date(j.finished) : "";
  const tags: string[] = [];
  if (j.engine) tags.push(t("on {engine}", { engine: j.engine.name }));
  if (s.kind === "convert") {
    tags.push(s.to);
    if (s.to === "ffpfsc") tags.push(t("level {n}", { n: s.level }));
    if (s.lz4) tags.push("LZ4 · " + s.lz4_profile);
    if (s.delete_source) tags.push(s.verify ? t("check, then delete source") : t("delete source"));
  }
  const notes = r.lz4 && r.lz4.profile && r.lz4.profile.notes ? r.lz4.profile.notes : [];
  const kind = s.kind === "convert" ? s.to : s.kind;
  const conv = CONVERTIBLE.indexOf(kind) >= 0;
  const sub = m ? [base(s.input), m.version ? "v" + m.version.replace(/^0+(?=\d)/, "") : "", m.title_id].filter(Boolean).join("  ·  ") : "";
  return (
    <div class="card click" tabIndex={0} data-key={"j" + jobKey(j)} onClick={() => jobActions(j)}>
      <div class="hd">
        <Ico kind={conv ? kind : "folder"} label={KIND_LABEL[kind]} icon={m && m.icon ? iconUrl({ path: m.from, mtime: 0 }) : null} />
        <div style="min-width:0;flex:1">
          <div class="ttl ellip">{m && m.title ? m.title : jt.title}</div>
          {m ? <div class="s ellip">{sub}</div> : null}
          <div class="to mono ellip">{jt.to}</div>
        </div>
        <span class={"state " + j.state}>{t(j.state)}</span>
      </div>
      {j.state === "running" ? <div class={"pbar" + (j.total ? "" : " indet")}><div style={"width:" + pct + "%"} /></div>
        : cpText(j) && isActive(j) ? <div class="pbar" style="opacity:.5"><div style={"width:" + cpPct(j) + "%"} /></div> : null}
      <div class="line"><span>{detail}</span><span>{tags.map((x) => <span class="tag">{x}</span>)}</span></div>
      {r.ampr_emulator ? <div class="line">{t("AMPR emulator added: {path}", { path: shownPath(r.ampr_emulator) })}</div> : null}
      {r.source_hidden ? <div class="line">{t("Source hidden from ShadowMount+ as {name}", { name: base(r.source_hidden) })}</div> : null}
      {isShared(s.input) && isActive(j) && !isSharedHere(s.input)
        ? <div class="note">{t("Reads {name} from a computer: keep Alembic open there, or pick the file again with From this computer…", { name: base(s.input) })}</div> : null}
      {r.smp_source ? <div class="note">{t("ShadowMount+ still uses {path} for this game. Hide it (⋯ on this job) to switch to the new one.", { path: shownPath(r.smp_source) })}</div> : null}
      {r.lz4 ? <div class="line">{t("LZ4: {a} of {b} files packed", { a: r.lz4.files_packed, b: r.lz4.files_total }) + " · " + bytes(r.lz4.packed_bytes) + " → " + bytes(r.lz4.volume_bytes)}</div> : null}
      {r.source_kept ? <div class="note">{t("Source kept: {why}", { why: t(r.source_kept) })}</div> : null}
      {notes.length ? <div class="note">{notes.map(tMsg).join(" ")}</div> : null}
      {r.warnings && r.warnings.length ? <div class="note">{r.warnings.map(tMsg).join(" ")}</div> : null}
      {j.error && j.state !== "cancelled" ? <div class="err">{tMsg(j.error)}</div> : null}
    </div>
  );
}

async function jobActions(j: Job) {
  const r = j.result || {}, items: { label: string; value: string; sub?: string | null; danger?: boolean }[] = [];
  // Its place among the waiting jobs of the same engine's queue.
  const engineOf = (x: Job) => (x.engine ? x.engine.id : "");
  const waiting = jobs.value.filter((x) => (x.state === "queued" || x.state === "paused") && engineOf(x) === engineOf(j));
  const pos = waiting.findIndex((x) => jobKey(x) === jobKey(j));
  if (j.state === "running") items.push({ label: t("Pause"), value: "pause",
    sub: j.keeps_progress ? t("Keeps its progress; other jobs run meanwhile") : t("This kind of job can't keep its progress: it starts over when resumed") });
  if (j.state === "paused") items.push({ label: t("Resume"), value: "resume", sub: cpText(j) ? t("Continues from {p}", { p: cpText(j) }) : t("Starts from the beginning") });
  if (j.state === "queued") items.push({ label: t("Hold"), value: "pause", sub: t("Keep it waiting until you resume it") });
  if (pos > 0) items.push({ label: t("Move up"), value: "up", sub: t("Run it before {name}", { name: jobText(waiting[pos - 1]).title }) });
  if (pos >= 0 && pos < waiting.length - 1) items.push({ label: t("Move down"), value: "down", sub: t("Run it after {name}", { name: jobText(waiting[pos + 1]).title }) });
  if (isActive(j)) items.push({ label: t("Cancel"), danger: true, sub: j.state === "running" || j.checkpoint ? t("Its partial output is deleted") : null, value: "cancel" });
  if (["failed", "cancelled", "interrupted"].indexOf(j.state) >= 0) items.push({ label: t("Retry"), value: "retry", sub: cpText(j) ? t("Continues from {p}", { p: cpText(j) }) : null });
  if (j.state === "done" && r.output) items.push({ label: t("Show in Files"), value: "show" });
  if (r.smp_source) items.push({ label: t("Hide {name} from ShadowMount+", { name: base(r.smp_source) }), value: "smp-hide", sub: t("So ShadowMount+ switches to the new image") });
  if (r.source_hidden) items.push({ label: t("Show the source to ShadowMount+ again"), value: "smp-show", sub: t("Renames it back to {name}", { name: base(r.source_hidden).slice(1) }) });
  if (!isActive(j)) items.push({ label: t("Remove from history"), value: "remove", sub: cpText(j) ? t("Also deletes its partial output") : null });
  if (!items.length) return;
  const v = await choose(jobText(j).title, items, jobText(j).to);
  if (v === "pause" || v === "resume") await act("POST", jobPath(j) + "/" + v);
  if (v === "up" || v === "down") await act("POST", jobPath(j) + "/move", { direction: v === "up" ? -1 : 1 });
  if (v === "cancel") await act("POST", jobPath(j) + "/cancel");
  if (v === "retry") await act("POST", jobPath(j) + "/retry");
  if (v === "remove") await act("DELETE", jobPath(j));
  if (v === "show" && r.output) { showTab("files"); browse(dir(r.output), true, r.output); }
  if (v === "smp-hide" && r.smp_source) await toggleHidden(r.smp_source);
  if (v === "smp-show" && r.source_hidden) await toggleHidden(r.source_hidden);
}
async function act(method: string, path: string, body?: unknown) {
  try { await api(method, path, body); } catch (e: any) { toast(tMsg(e.message), "bad"); }
  refreshJobs();
}

export function QueueView(props: { hidden: boolean }) {
  const list = jobs.value;
  const active = list.filter(isActive);
  const done = list.filter((j) => !isActive(j)).reverse();
  return (
    <section class="view scroll" id="view-queue" hidden={props.hidden}>
      <div class="page" data-zone="queue">
        <div class="head2"><div class="sect">{t("Active")}</div></div>
        <div id="active">
          {active.length ? active.map((j) => <JobCard key={jobKey(j)} j={j} />) : <div class="card empty">{t("Nothing running. Pick a package in Files and choose Convert.")}</div>}
        </div>
        <div class="head2"><div class="sect">{t("History")}</div><button class="btn sm ghost" id="btn-clear" onClick={() => act("POST", "/api/jobs/clear")}>{t("Clear history")}</button></div>
        <div id="history">
          {done.length ? done.map((j) => <JobCard key={jobKey(j)} j={j} />) : <div class="card empty">{t("No finished jobs.")}</div>}
        </div>
      </div>
    </section>
  );
}
