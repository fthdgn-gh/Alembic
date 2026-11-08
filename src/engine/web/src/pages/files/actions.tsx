// SPDX-License-Identifier: GPL-3.0-or-later
// What can be done to a file or folder: its Options menu and each action.
import type { Entry, NameSuggestion, Plan, PlanEmulator } from "../../api/types";
import { api } from "../../api/client";
import { cwd, tab } from "../../state/view";
import { info, locations, peerDrives, loadInfo } from "../../state/engine";
import { shownPath } from "../../state/paths";
import { refreshJobs } from "../../state/jobs";
import { isConvertible, isHiddenName } from "../../lib/kinds";
import { base, bytes, dir } from "../../lib/format";
import { t, tMsg } from "../../lib/i18n";
import { ask, choose } from "../../components/Dialogs";
import type { ChoiceItem } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { openConvert } from "../../dialogs/ConvertDialog";
import { openDest } from "../../dialogs/FolderPicker";
import { getNeeds } from "../../dialogs/getNeeds";
import { browse } from "./browse";
import { startSelecting } from "../../state/selection";
import { loadImages } from "../../state/images";
import { updateAmpr } from "../images/ampr";
import type { ImageItem } from "../../api/types";

// In Images (`lib`, the tab's item): the same, less what moves or renames
// files (Open, Places, Copy, Move, Rename, plain Hide), plus the update of
// the game's AMPR emulator when a newer build is out.
export async function openActions(e: Entry, lib?: ImageItem) {
  if (!lib && cwd.value === "/") return;
  const conv = isConvertible(e);
  const items: { label: string; sub?: string; danger?: boolean; value: () => void }[] = [];
  if (!lib && e.kind === "folder" && e.path !== cwd.value) items.push({ label: t("Open"), value: () => browse(e.path) });
  if (conv) items.push({ label: t("Convert…"), sub: t("To FFPFSC, FFPKG, exFAT or a folder"), value: () => openConvert(e) });
  // An image or folder installed like a package: converted, installed, the package deleted.
  if (conv && e.kind !== "fpkg" && info.value && info.value.platform === "ps5")
    items.push({ label: t("Install…"), sub: t("Converts it to a package, installs it, then deletes the package"), value: () => openConvert(e, true) });
  if (e.kind === "fpkg") items.push({ label: t("Install"), sub: t("Hand it to the console's installer"), value: () => installPkg(e) });
  const amprUpdate = lib && lib.ampr && (lib.ampr.status === "update" || lib.ampr.status === "not_downloaded") ? lib : null;
  if (amprUpdate) items.push({ label: t("Update the AMPR emulator"), sub: t("{old} → the newest build of its kind", { old: amprUpdate.ampr!.emulator ? amprUpdate.ampr!.emulator.name : "" }), value: () => updateAmpr([amprUpdate]) });
  if (conv && e.kind !== "fpkg") items.push({ label: t("Add the AMPR emulator…"), sub: t("For games that stop at start from an image or folder"), value: () => addAmpr(e) });
  if (conv && e.kind !== "fpkg") items.push(isHiddenName(e.name)
    ? { label: t("Show to ShadowMount+ again"), sub: t("Renames it back to {name}", { name: e.name.slice(1) }), value: () => toggleHidden(e.path) }
    : { label: t("Hide from ShadowMount+"), sub: t("Renames it to .{name}, so ShadowMount+ uses another copy of this game", { name: e.name }), value: () => toggleHidden(e.path) });
  // Any other file or folder (a package, say): hidden by its leading dot alone.
  else if (!lib) items.push(isHiddenName(e.name)
    ? { label: t("Unhide"), sub: t("Renames it back to {name}", { name: e.name.slice(1) }), value: () => toggleHidden(e.path, false, false) }
    : { label: t("Hide"), sub: t("Renames it to .{name}", { name: e.name }), value: () => toggleHidden(e.path, false, false) });
  // Places (this engine's or a paired one's): shared with every paired engine.
  if (!lib && e.kind === "folder" && cwd.value !== "/") {
    const isPlace = locations.value.some((l) => l.custom && l.path === e.path);
    items.push(isPlace ? { label: t("Remove from Places"), value: () => setPlace(e.path, false) }
      : { label: t("Add to Places"), sub: t("Shows it in Places and Save to, here and on paired engines"), value: () => setPlace(e.path, true) });
  }
  // A paired engine's usual destination (Settings > Other engines), for a copy without converting.
  const peerPlace = locations.value.find((l) => !!l.peer) || peerDrives.value.map((d) => ({ path: d.root, label: d.engine }))[0];
  if (peerPlace && !e.shared) items.push({ label: t("Send to another engine…"), sub: t("A copy as it is, without converting"), value: () => openDest(e, "copy", peerPlace.path) });
  if (!lib) items.push({ label: t("Copy to…"), value: () => openDest(e, "copy") }, { label: t("Move to…"), value: () => openDest(e, "move") },
    { label: t("Rename…"), value: () => renameEntry(e) });
  // Not the folder you are in (its ⋯ button): a row of the list.
  if (lib || e.path !== cwd.value) items.push({ label: t("Select…"), sub: t("Tick several files to act on them together"), value: () => startSelecting(e) });
  items.push({ label: t("Delete…"), danger: true, value: () => deleteEntry(e) });
  const fn = await choose(e.name, items);
  if (fn) fn();
}


export async function setPlace(path: string, add: boolean) {
  try {
    locations.value = await api("POST", "/api/places", { path, remove: !add });
    toast(add ? t("Added to Places") : t("Removed from Places"), "ok");
  } catch (e: any) { toast(tMsg(e.message), "bad"); }
}

// The installer cannot see these itself: the engine serves them over HTTP.
function elsewhere(e: Entry) { return !!(e.ftp || e.peer || e.shared) || e.path.indexOf("/peer/") === 0; }
async function installPkg(e: Entry) {
  if (info.value && info.value.platform !== "ps5") { toast(t("Installing needs the engine running on a PS5"), "bad"); return; }
  const streamed = elsewhere(e);
  const body = <div><div class="mono">{shownPath(e.path)}</div>
    <p class="muted">{streamed
      ? t("The console installs it in the background; follow it in Downloads. The engine serves it to the installer: keep the engine running and the file in place until the install has finished.")
      : t("The console installs it in the background; follow it in Downloads. Keep the file until the install has finished.")}</p></div>;
  if (!await ask({ title: t("Install this package?"), body, ok: t("Install") })) return;
  try { await api("POST", "/api/jobs", { kind: "install", input: e.path }); toast(t("Install queued"), "ok"); refreshJobs(); }
  catch (err: any) { toast(tMsg(err.message), "bad"); }
}

// ShadowMount+ skips names that start with "." and keeps one source per
// game while that source exists: hiding the old one makes it switch.

// `smp`: an image ShadowMount+ uses (it rescans and the toast says so).
export async function toggleHidden(path: string, quiet?: boolean, smp = true) {
  const n = base(path), name = isHiddenName(n) ? n.slice(1) : "." + n;
  try {
    await api("POST", "/api/fs/rename", { path, name });
    if (!smp) {
      if (!quiet) toast(t("Renamed to {name}", { name }), "ok");
      if (tab.value === "files" && cwd.value === dir(path)) browse(cwd.value, false, dir(path) + "/" + name);
      if (tab.value === "images") loadImages(true);
      return true;
    }
    let scanned = true;
    try { await api("POST", "/api/smp/scan"); } catch (e) { scanned = false; }
    if (!quiet) toast(isHiddenName(name) ? (scanned ? t("Hidden from ShadowMount+; it is rescanning") : t("Hidden from ShadowMount+"))
      : (scanned ? t("Visible to ShadowMount+ again; it is rescanning") : t("Visible to ShadowMount+ again")), "ok");
    if (tab.value === "files" && cwd.value === dir(path)) browse(cwd.value, false, dir(path) + "/" + name);
    if (tab.value === "images") loadImages(true);
    return true;
  } catch (err: any) { toast(tMsg(err.message), "bad"); return false; }
}

// ShadowMount+ backport folder (the image is not changed).
async function addAmpr(e: Entry) {
  const isFolder = e.kind === "folder";
  let plan: Plan;
  try { plan = await api<Plan>("POST", "/api/plan", { kind: "ampr_emu", input: e.path }); }
  catch (err: any) { toast(tMsg(err.message), "bad"); return; }
  const present = plan.emulator_present === undefined ? !!plan.ships_emulator : plan.emulator_present;
  if (present) {
    if (plan.emulators) return switchAmpr(e, plan);
    if (plan.emulator_current || !plan.runtime_name) { toast(t("This game already has the AMPR emulator"), "ok"); return; }
    return replaceAmpr(e, plan);
  }
  const body = <div>
    <div class="mono">{shownPath(e.path)}</div>
    <p>{plan.imports_ampr === false ? t("This game does not seem to use the console's APR, so it may not need the emulator. It does no harm to add it.")
      : t("This game reads its files through the console's APR, which doesn't work when it runs from an image or folder. ampr_emu's libSceAmpr.sprx stands in for it.")}</p>
    <p class="muted">{isFolder ? t("It goes into the game folder's fakelib (and ampr_emu.index, if the game has none).")
      : t("The image is not changed: ShadowMount+ gets a backport folder for this title (the game's fakelib files plus the emulator), which it lays over the game when it starts.")}</p>
  </div>;
  if (!await ask({ title: t("Add the AMPR emulator?"), body, ok: t("Add") })) return;
  try {
    if (!await getNeeds(plan, "This needs {name}, which isn't downloaded yet.")) return;
    await api("POST", "/api/jobs", { kind: "ampr_emu", input: e.path });
    toast(t("Queued: {name}", { name: e.name }), "ok"); refreshJobs();
  } catch (err: any) { toast(tMsg(err.message), "bad"); }
}

// The game has an emulator: switch it to another (LZ4 packs work with
// either; zstd packs only with ampr-emu-ng).
async function switchAmpr(e: Entry, plan: Plan) {
  const isFolder = e.kind === "folder";
  const others = plan.emulators!.filter((x) => !x.in_game && x.reads_packs);
  if (!others.length) {
    toast(plan.zstd_packs ? t("This game has zstd packs and already has ampr-emu-ng, the only emulator that reads them") : t("This game already has the AMPR emulator"), "ok");
    return;
  }
  let pick: PlanEmulator = others[0];
  if (others.length > 1) {
    const picked = await choose(t("Switch to which AMPR emulator?"), others.map((x) => ({ label: x.name, value: x,
      sub: x.in_use ? t("in use") : x.downloaded ? "" : t("not downloaded yet") })));
    if (picked == null) return;
    pick = picked;
  }
  const body = <div>
    <div class="mono">{shownPath(e.path)}</div>
    <p>{t("This game has {old}. Replace it with {new}?", { old: plan.emulator_name || t("another libSceAmpr.sprx"), new: pick.name })}</p>
    {plan.lz4_packs ? <p class="muted">{t("Both emulators read this game's LZ4 packs.")}</p> : null}
    <p class="muted">{isFolder ? t("It is overwritten in the game folder's fakelib.")
      : t("The image is not changed: ShadowMount+ gets a backport folder for this title with the new emulator, which it lays over the game when it starts.")}</p>
  </div>;
  if (!await ask({ title: t("Switch the AMPR emulator?"), body, ok: t("Replace") })) return;
  const spec = { kind: "ampr_emu", input: e.path, ampr_emu: "replace", emulator: pick.id };
  try {
    if (!pick.downloaded && !await getNeeds(await api<Plan>("POST", "/api/plan", spec), "This needs {name}, which isn't downloaded yet.")) return;
    await api("POST", "/api/jobs", spec);
    toast(t("Queued: {name}", { name: e.name }), "ok"); refreshJobs();
  } catch (err: any) { toast(tMsg(err.message), "bad"); }
}

// The game has another libSceAmpr.sprx than the one in use in Components
// (engines before 0.4.3, which list no emulators).
async function replaceAmpr(e: Entry, plan: Plan) {
  const isFolder = e.kind === "folder";
  if (plan.zstd_packs && plan.runtime_name!.indexOf("ampr-emu-ng") !== 0) {
    toast(t("This game has zstd packs, which only ampr-emu-ng reads; choose it in Components first"), "bad"); return;
  }
  const body = <div>
    <div class="mono">{shownPath(e.path)}</div>
    <p>{t("This game has {old}. Replace it with {new}, the one in use in Components?", { old: plan.emulator_name || t("another libSceAmpr.sprx"), new: plan.runtime_name! })}</p>
    <p class="muted">{isFolder ? t("It is overwritten in the game folder's fakelib.")
      : t("The image is not changed: ShadowMount+ gets a backport folder for this title with the new emulator, which it lays over the game when it starts.")}</p>
  </div>;
  if (!await ask({ title: t("Replace the AMPR emulator?"), body, ok: t("Replace") })) return;
  try {
    await api("POST", "/api/jobs", { kind: "ampr_emu", input: e.path, ampr_emu: "replace" });
    toast(t("Queued: {name}", { name: e.name }), "ok"); refreshJobs();
  } catch (err: any) { toast(tMsg(err.message), "bad"); }
}

// A game first offers names made by the formats in Settings > Rename
// formats; the one picked can still be edited.
async function renameEntry(e: Entry) {
  let start = e.name;
  let names: NameSuggestion[] = [];
  if (isConvertible(e)) {
    try { names = (await api<{ suggestions: NameSuggestion[] }>("GET", "/api/names/suggest?path=" + encodeURIComponent(e.path))).suggestions || []; }
    catch (err) { names = []; } // an engine before rename formats
  }
  if (names.length) {
    const items: ChoiceItem<string>[] = [{ label: e.name, sub: t("Current name"), value: e.name }];
    const picked = await choose(t("Rename"), items.concat(names.map((s) => ({ label: s.name, value: s.name,
      sub: s.warning ? t("Too long for ShadowMount+") : s.shortened ? t("Shortened to fit ShadowMount+") : s.pattern }))));
    if (picked == null) return;
    start = picked;
  }
  const typed = await ask({ title: t("Rename"), input: start, ok: t("Rename") });
  if (!typed || typed === true) return;
  // A hidden name stays hidden (Show to ShadowMount+ again shows it).
  const name = isHiddenName(e.name) && !isHiddenName(typed) ? "." + typed : typed;
  if (name === e.name) return;
  // An image whose new name ShadowMount+ could not mount: asked first.
  if (e.kind !== "folder") {
    let warning = "";
    try { warning = (await api<{ warning: string }>("POST", "/api/names/check", { path: e.path, name })).warning; }
    catch (err) { warning = ""; } // an engine before rename formats
    if (warning && !await ask({ title: t("Too long for ShadowMount+"), body: <p>{tMsg(warning)}</p>, ok: t("Rename anyway") })) return;
  }
  try { await api("POST", "/api/fs/rename", { path: e.path, name }); toast(t("Renamed to {name}", { name }), "ok"); browse(cwd.value!, false, dir(e.path) + "/" + name); }
  catch (err: any) { toast(tMsg(err.message), "bad"); }
}

async function deleteEntry(e: Entry) {
  const body = <div>
    <div class="warnbox">
      <b>{e.kind === "folder" ? t("Delete this folder and everything in it?") : t("Delete this file?")}</b>
      <div class="mono" style="margin-top:.4rem">{shownPath(e.path)}</div>
      {e.size != null ? <div class="muted" style="margin-top:.2rem">{bytes(e.size)}</div> : null}
    </div>
    <p class="muted">{t("This can't be undone.")}</p>
  </div>;
  if (!await ask({ title: t("Delete"), body, ok: t("Delete"), danger: true })) return;
  try { await api("POST", "/api/fs/delete", { path: e.path }); toast(t("Deleted {name}", { name: e.name }), "ok"); if (tab.value === "images") loadImages(true); else browse(cwd.value!, false); loadInfo().catch(() => {}); }
  catch (err: any) { toast(tMsg(err.message), "bad"); }
}
