// SPDX-License-Identifier: GPL-3.0-or-later
// The job queue: this engine's jobs, then the ones other engines run that
// involve it. Asked for every second (main.tsx).
import { signal } from "@preact/signals";
import type { Job } from "../api/types";
import { api } from "../api/client";
import { info, anyPaired, notConnected, answered, loadInfo } from "./engine";
import { tab, cwd } from "./view";
import { t } from "../lib/i18n";
import { base } from "../lib/format";
import { toast } from "../components/Toasts";
import { overlays } from "../components/Overlay";
import { browse } from "../pages/files/browse";

export const jobs = signal<Job[]>([]);

// Running, or still to run (queued or paused), in the order they run.
export const isActive = (j: Job) => j.state === "running" || j.state === "queued" || j.state === "paused";
// Job numbers repeat across engines.
export const jobKey = (j: Job) => (j.engine ? j.engine.id + ":" + j.id : String(j.id));
export const jobPath = (j: Job) => (j.engine ? "/api/peers/" + encodeURIComponent(j.engine.id) + "/jobs/" + j.id : "/api/jobs/" + j.id);

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

// Speed per running job. A sample counts when the progress moved: another
// engine's jobs come every second or so, so a refresh in between is no
// news, not a stop (after 10 s it is).
const speeds: Record<string, { t: number; done: number; rate: number; stage: string }> = {};
function sample(j: Job) {
  const key = jobKey(j), now = Date.now() / 1000, sp = speeds[key];
  if (sp && sp.stage === j.stage && j.done >= sp.done && now - sp.t > 0.5 && (j.done !== sp.done || now - sp.t > 10)) {
    const inst = (j.done - sp.done) / (now - sp.t);
    speeds[key] = { t: now, done: j.done, rate: sp.rate ? sp.rate * 0.75 + inst * 0.25 : inst, stage: j.stage };
  } else if (!sp || sp.stage !== j.stage) speeds[key] = { t: now, done: j.done, rate: 0, stage: j.stage };
}
// Bytes a second, 0 until known.
export const rateOf = (j: Job) => { const sp = speeds[jobKey(j)]; return sp && sp.stage === j.stage ? sp.rate : 0; };

// Jobs seen running or waiting, every job seen, and the engines whose lists arrived.
let wasActive = new Set<string>(), known = new Set<string>();
const listed = new Set<string>();
let remoteJobs: Job[] = [], remoteAt = 0, remoteBusy = false;
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
  // Toast and refresh the listing when a job finishes: one this page saw
  // running or waiting, or a new one already done (a quick copy between two
  // refreshes); not the finished jobs in an engine's first list, which are
  // history (on reload, or when another engine's jobs first arrive).
  const engineOf = (j: Job) => (j.engine ? j.engine.id : "");
  const fresh = (j: Job) => !known.has(jobKey(j)) && listed.has(engineOf(j));
  const ended = list.filter((j) => !isActive(j) && (wasActive.has(jobKey(j)) || fresh(j))).sort((a, b) => b.finished - a.finished);
  if (ended.length) {
    const newest = ended[0];
    if (newest.state === "done") toast(t("Finished: {name}", { name: base(newest.spec.input) }), "ok");
    if (newest.state === "failed") toast(t("Failed: {name}", { name: base(newest.spec.input) }), "bad");
    if (cwd.value && !overlays.value.length) browse(cwd.value, false);
    loadInfo().catch(() => {});
  }
  wasActive = new Set(list.filter(isActive).map(jobKey));
  known = new Set(list.map(jobKey));
  listed.add("");
  for (const j of list) listed.add(engineOf(j));
  jobs.value = list;
}
