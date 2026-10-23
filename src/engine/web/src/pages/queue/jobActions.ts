// SPDX-License-Identifier: GPL-3.0-or-later
// A job's menu: pause, resume, reorder, cancel, retry, show, remove.
import type { Job } from "../../api/types";
import { api } from "../../api/client";
import { jobs, isActive, jobKey, jobPath, refreshJobs } from "../../state/jobs";
import { base, dir } from "../../lib/format";
import { t, tMsg } from "../../lib/i18n";
import { choose } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { showTab } from "../../input/history";
import { browse } from "../files/browse";
import { toggleHidden } from "../files/actions";
import { jobText, cpText } from "./jobText";

export async function jobActions(j: Job) {
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
export async function act(method: string, path: string, body?: unknown) {
  try { await api(method, path, body); } catch (e: any) { toast(tMsg(e.message), "bad"); }
  refreshJobs();
}

// Clears the finished jobs listed: this engine's, and those other engines
// show here (jobs that use this engine's files). Those live on the other
// engine, so they leave its history too, as with Remove from history.
export async function clearHistory() {
  const theirs = jobs.value.filter((j) => j.engine && !isActive(j));
  let problem: string | null = null;
  try { await api("POST", "/api/jobs/clear"); } catch (e: any) { problem = e.message; }
  for (const j of theirs) {
    try { await api("DELETE", jobPath(j)); } catch (e: any) { if (!problem) problem = e.message; }
  }
  if (problem) toast(tMsg(problem), "bad");
  refreshJobs(true);
}
