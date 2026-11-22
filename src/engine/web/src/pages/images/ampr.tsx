// SPDX-License-Identifier: GPL-3.0-or-later
// Updating the AMPR emulator of images (the Images tab, its Options and its
// selection): an "ampr_emu" job each, keeping the game's emulator (or adding
// the one in use where there is none) unless another one is chosen for all,
// the downloads it needs asked for once.
import { signal } from "@preact/signals";
import type { Dep, ImageItem, Plan } from "../../api/types";
import { api } from "../../api/client";
import { images } from "../../state/images";
import { refreshJobs } from "../../state/jobs";
import { shownPath } from "../../state/paths";
import { base } from "../../lib/format";
import { t, tMsg, plural } from "../../lib/i18n";
import { toast } from "../../components/Toasts";
import { ask, choose, type ChoiceItem } from "../../components/Dialogs";
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
  // Emulators that aren't downloaded yet: one question for all of them. What
  // a job needs depends only on its emulator, so one plan each (a plan reads
  // the game; one per image kept the engine busy for long).
  const needs: Dep[] = [];
  const asked: string[] = [];
  for (const i of list) {
    const target = i.ampr!.target || "";
    if (asked.indexOf(target) >= 0) continue;
    asked.push(target);
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

const NG = "ampr_runtime_ng";
const emuName = (e: { name: string; version: string; latest: string }) => e.name + (e.latest || e.version ? " " + (e.latest || e.version) : "");

// Which emulator goes in (issue #6): each game's own kind (the default), or
// one chosen for all. zstd-packed games keep ampr-emu-ng: only it reads their
// packs. Resolves with the list, its targets set; null when cancelled.
async function pickEmulator(list: ImageItem[]): Promise<ImageItem[] | null> {
  const emus = images.value ? images.value.emulators : [];
  if (emus.length < 2) return list;
  const one = list.length === 1 ? list[0] : null;
  const own = one && one.ampr!.emulator ? one.ampr!.emulator : null;
  const anyOwn = list.some((i) => !!i.ampr!.emulator);
  const inUse = emus.find((e) => e.in_use) || emus[0];
  const items: ChoiceItem<string>[] = [];
  // Its kind by name (the game's own build is older: its version would mislead).
  const ownKind = own ? (emus.find((e) => e.id === own.id) || own).name : "";
  if (anyOwn) items.push(own
    ? { label: t("Keep {name}", { name: ownKind }), sub: t("Its newest build"), value: "", selected: true }
    : { label: t("Keep each game's emulator"), sub: t("Each gets its own emulator's newest build; a game without one gets {name}", { name: inUse.name }), value: "", selected: true });
  for (const e of emus) {
    if (own && own.id === e.id) continue;
    if (one && one.ampr!.zstd_packs && e.id !== NG) continue; // only ampr-emu-ng reads its packs
    items.push({ label: emuName(e), value: e.id, selected: !anyOwn && (one ? one.ampr!.target === e.id : e.in_use),
      sub: (e.id === NG ? t("Reads LZ4 and zstd packs") : t("Reads LZ4 packs, not zstd ones")) + (e.in_use ? " · " + t("in use") : "") });
  }
  if (items.length < 2) return list; // nothing to choose
  const chosen = await choose(t("Choose the AMPR emulator"), items, one ? shownPath(one.path) : plural(list.length, "{n} game", "{n} games"));
  if (chosen === null) return null;
  if (!chosen) return list;
  return list.map((i) => (i.ampr!.zstd_packs && chosen !== NG ? i : { ...i, ampr: { ...i.ampr!, target: chosen } }));
}

// One image: which emulator, then at once (an unknown build asks first); several: which emulator, then one question.
export async function updateAmpr(list: ImageItem[]) {
  list = list.filter((i) => amprUpdatable(i) || (i.ampr && i.ampr.status === "unknown" && i.allowed));
  if (!list.length) { toast(t("Every image is up to date"), "ok"); return; }
  if (list.length > 1) list = list.filter((i) => i.ampr!.status !== "unknown"); // custom builds only one at a time
  if (!list.length) { toast(t("Every image is up to date"), "ok"); return; }
  const before = list;
  const picked = await pickEmulator(list);
  if (!picked) return;
  list = picked;
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
  const add = list.filter((i) => i.ampr!.status === "missing").length;
  // One emulator for all: the zstd-packed games that keep ampr-emu-ng.
  const forAll = list.some((i, k) => i !== before[k]) ? list.find((i, k) => i !== before[k])! : null;
  const keptNg = forAll ? list.filter((i, k) => i === before[k]).length : 0;
  const shown = list.slice(0, 6).map((i) => base(i.path)).join("\n") + (list.length > 6 ? "\n" + t("and {n} more", { n: list.length - 6 }) : "");
  const body = <div><div class="mono" style="white-space:pre-line">{shown}</div>
    <p class="muted">{forAll ? t("Each gets {name}.", { name: targetName(forAll) })
      : t("Each keeps its emulator (ampr_emu or ampr-emu-ng) and gets that one's newest build; a game without one gets the one in use.")}</p>
    {keptNg ? <p class="muted">{plural(keptNg, "{n} of them has zstd packs and keeps ampr-emu-ng, the only emulator that reads them.", "{n} of them have zstd packs and keep ampr-emu-ng, the only emulator that reads them.")}</p> : null}
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
