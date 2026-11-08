// SPDX-License-Identifier: GPL-3.0-or-later
// Updating the AMPR emulator of images (the Images tab, its Options and its
// selection): an "ampr_emu" job each, keeping the game's emulator (or adding
// the one in use where there is none), the downloads it needs asked for once.
import { signal } from "@preact/signals";
import type { Dep, ImageItem, Plan } from "../../api/types";
import { api } from "../../api/client";
import { images } from "../../state/images";
import { refreshJobs } from "../../state/jobs";
import { shownPath } from "../../state/paths";
import { base } from "../../lib/format";
import { t, tMsg, plural } from "../../lib/i18n";
import { toast } from "../../components/Toasts";
import { ask } from "../../components/Dialogs";
import { getNeeds } from "../../dialogs/getNeeds";

export const amprPending = signal<string[]>([]); // paths with a job queued from here

// An update applies: a newer build of its kind, or none yet; inside the engine's folders.
export const amprUpdatable = (i: ImageItem) => !!i.ampr && i.allowed && amprPending.value.indexOf(i.path) < 0 &&
  (i.ampr.status === "update" || i.ampr.status === "missing" || i.ampr.status === "not_downloaded");

// The emulator an Update or Add puts there, by name.
export function targetName(i: ImageItem) {
  const id = i.ampr && i.ampr.target, e = images.value && images.value.emulators.find((x) => x.id === id);
  return e ? (e.version ? e.name + " " + e.version : e.name) : id || "";
}

const specOf = (i: ImageItem) => ({ kind: "ampr_emu", input: i.path, ampr_emu: i.ampr!.status === "missing" ? "auto" : "replace", emulator: i.ampr!.target });

async function queue(list: ImageItem[]): Promise<number> {
  // Emulators that aren't downloaded yet: one question for all of them.
  const needs: Dep[] = [];
  for (const i of list) {
    try { for (const d of (await api<Plan>("POST", "/api/plan", specOf(i))).needs || []) if (!needs.some((y) => y.id === d.id)) needs.push(d); }
    catch (e) { /* the job says why */ }
  }
  try { if (needs.length && !await getNeeds({ output: "", needs } as Plan, "This needs {name}, which isn't downloaded yet.")) return 0; }
  catch (e: any) { toast(tMsg(e.message), "bad"); return 0; }
  let queued = 0;
  const failed: string[] = [];
  for (const i of list) {
    try { await api("POST", "/api/jobs", specOf(i)); queued++; amprPending.value = amprPending.value.concat([i.path]); }
    catch (e: any) { failed.push(base(i.path) + ": " + tMsg(e.message)); }
  }
  if (queued) refreshJobs();
  if (failed.length) toast(failed.length > 1 ? t("{first} (and {n} more)", { first: failed[0], n: failed.length - 1 }) : failed[0], "bad");
  return queued;
}

// One image: at once (an unknown build asks first); several: one question.
export async function updateAmpr(list: ImageItem[]) {
  list = list.filter((i) => amprUpdatable(i) || (i.ampr && i.ampr.status === "unknown" && i.allowed));
  if (!list.length) { toast(t("Every image is up to date"), "ok"); return; }
  if (list.length === 1) {
    const i = list[0];
    if (i.ampr!.status === "unknown") {
      const body = <div><div class="mono">{shownPath(i.path)}</div>
        <p>{t("This game has a libSceAmpr.sprx that is not an official build of either AMPR emulator. Replace it with {new}?", { new: targetName(i) })}</p></div>;
      if (!await ask({ title: t("Replace the AMPR emulator?"), body, ok: t("Replace") })) return;
    }
    if (await queue(list)) toast(t("Queued: {name}", { name: base(i.path) }), "ok");
    return;
  }
  list = list.filter((i) => i.ampr!.status !== "unknown"); // custom builds only one at a time
  const add = list.filter((i) => i.ampr!.status === "missing").length;
  const shown = list.slice(0, 6).map((i) => base(i.path)).join("\n") + (list.length > 6 ? "\n" + t("and {n} more", { n: list.length - 6 }) : "");
  const body = <div><div class="mono" style="white-space:pre-line">{shown}</div>
    <p class="muted">{t("Each keeps its emulator (ampr_emu or ampr-emu-ng) and gets that one's newest build; a game without one gets the one in use.")}</p>
    {add ? <p class="muted">{plural(add, "{n} of them has no emulator yet.", "{n} of them have no emulator yet.")}</p> : null}
    <p class="muted">{t("Game folders are changed in place; images are not changed: ShadowMount+ gets a backport folder for each.")}</p></div>;
  if (!await ask({ title: plural(list.length, "Update {n} image?", "Update {n} images?"), body, ok: t("Update") })) return;
  const n = await queue(list);
  if (n) toast(plural(n, "{n} job queued", "{n} jobs queued"), "ok");
}

// Every image whose emulator can be updated or added.
export function updateAll() {
  return updateAmpr((images.value ? images.value.images : []).filter(amprUpdatable));
}
