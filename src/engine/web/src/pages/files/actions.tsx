// SPDX-License-Identifier: GPL-3.0-or-later
// What can be done to a file or folder: its Options menu and each action.
import type { Entry, Plan } from "../../api/types";
import { api } from "../../api/client";
import { cwd, tab } from "../../state/view";
import { info, locations, peerDrives, loadInfo } from "../../state/engine";
import { shownPath } from "../../state/paths";
import { refreshJobs } from "../../state/jobs";
import { isConvertible, isHiddenName } from "../../lib/kinds";
import { base, bytes, dir } from "../../lib/format";
import { t, tMsg } from "../../lib/i18n";
import { ask, choose } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { openConvert } from "../../dialogs/ConvertDialog";
import { openDest } from "../../dialogs/FolderPicker";
import { getNeeds } from "../../dialogs/getNeeds";
import { browse } from "./browse";

export async function openActions(e: Entry) {
  if (cwd.value === "/") return;
  const conv = isConvertible(e);
  const items: { label: string; sub?: string; danger?: boolean; value: () => void }[] = [];
  if (e.kind === "folder" && e.path !== cwd.value) items.push({ label: t("Open"), value: () => browse(e.path) });
  if (conv) items.push({ label: t("Convert…"), sub: t("To FFPFSC, FFPKG, exFAT or a folder"), value: () => openConvert(e) });
  if (e.kind === "fpkg") items.push({ label: t("Install"), sub: t("Hand it to the console's installer"), value: () => installPkg(e) });
  if (conv && e.kind !== "fpkg") items.push({ label: t("Add the AMPR emulator…"), sub: t("For games that stop at start from an image or folder"), value: () => addAmpr(e) });
  if (conv && e.kind !== "fpkg") items.push(isHiddenName(e.name)
    ? { label: t("Show to ShadowMount+ again"), sub: t("Renames it back to {name}", { name: e.name.slice(1) }), value: () => toggleHidden(e.path) }
    : { label: t("Hide from ShadowMount+"), sub: t("Renames it to .{name}, so ShadowMount+ uses another copy of this game", { name: e.name }), value: () => toggleHidden(e.path) });
  // Places (this engine's or a paired one's): shared with every paired engine.
  if (e.kind === "folder" && cwd.value !== "/") {
    const isPlace = locations.value.some((l) => l.custom && l.path === e.path);
    items.push(isPlace ? { label: t("Remove from Places"), value: () => setPlace(e.path, false) }
      : { label: t("Add to Places"), sub: t("Shows it in Places and Save to, here and on paired engines"), value: () => setPlace(e.path, true) });
  }
  // A paired engine's usual destination (Settings > Other engines), for a copy without converting.
  const peerPlace = locations.value.find((l) => !!l.peer) || peerDrives.value.map((d) => ({ path: d.root, label: d.engine }))[0];
  if (peerPlace && !e.shared) items.push({ label: t("Send to another engine…"), sub: t("A copy as it is, without converting"), value: () => openDest(e, "copy", peerPlace.path) });
  items.push({ label: t("Copy to…"), value: () => openDest(e, "copy") }, { label: t("Move to…"), value: () => openDest(e, "move") },
    { label: t("Rename…"), value: () => renameEntry(e) }, { label: t("Delete…"), danger: true, value: () => deleteEntry(e) });
  const fn = await choose(e.name, items);
  if (fn) fn();
}


export async function setPlace(path: string, add: boolean) {
  try {
    locations.value = await api("POST", "/api/places", { path, remove: !add });
    toast(add ? t("Added to Places") : t("Removed from Places"), "ok");
  } catch (e: any) { toast(tMsg(e.message), "bad"); }
}

async function installPkg(e: Entry) {
  if (info.value && info.value.platform !== "ps5") { toast(t("Installing needs the engine running on a PS5"), "bad"); return; }
  const body = <div><div class="mono">{shownPath(e.path)}</div>
    <p class="muted">{t("The console installs it in the background; follow it in Downloads. Keep the file until the install has finished.")}</p></div>;
  if (!await ask({ title: t("Install this package?"), body, ok: t("Install") })) return;
  try { await api("POST", "/api/jobs", { kind: "install", input: e.path }); toast(t("Install queued"), "ok"); refreshJobs(); }
  catch (err: any) { toast(tMsg(err.message), "bad"); }
}

// ShadowMount+ skips names that start with "." and keeps one source per
// game while that source exists: hiding the old one makes it switch.

export async function toggleHidden(path: string, quiet?: boolean) {
  const n = base(path), name = isHiddenName(n) ? n.slice(1) : "." + n;
  try {
    await api("POST", "/api/fs/rename", { path, name });
    let scanned = true;
    try { await api("POST", "/api/smp/scan"); } catch (e) { scanned = false; }
    if (!quiet) toast(isHiddenName(name) ? (scanned ? t("Hidden from ShadowMount+; it is rescanning") : t("Hidden from ShadowMount+"))
      : (scanned ? t("Visible to ShadowMount+ again; it is rescanning") : t("Visible to ShadowMount+ again")), "ok");
    if (tab.value === "files" && cwd.value === dir(path)) browse(cwd.value, false, dir(path) + "/" + name);
    return true;
  } catch (err: any) { toast(tMsg(err.message), "bad"); return false; }
}

// ShadowMount+ backport folder (the image is not changed).
async function addAmpr(e: Entry) {
  const isFolder = e.kind === "folder";
  let plan: Plan;
  try { plan = await api<Plan>("POST", "/api/plan", { kind: "ampr_emu", input: e.path }); }
  catch (err: any) { toast(tMsg(err.message), "bad"); return; }
  if (plan.ships_emulator) { toast(t("This game already has the AMPR emulator"), "ok"); return; }
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

async function renameEntry(e: Entry) {
  const name = await ask({ title: t("Rename"), input: e.name, ok: t("Rename") });
  if (!name || name === true || name === e.name) return;
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
  try { await api("POST", "/api/fs/delete", { path: e.path }); toast(t("Deleted {name}", { name: e.name }), "ok"); browse(cwd.value!, false); loadInfo().catch(() => {}); }
  catch (err: any) { toast(tMsg(err.message), "bad"); }
}
