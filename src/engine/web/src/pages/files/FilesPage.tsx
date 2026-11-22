// SPDX-License-Identifier: GPL-3.0-or-later
// Files: drives and places on the left, the folder's contents on the right.
import { useEffect, useRef } from "preact/hooks";
import { cwd, listing, TOP, fileFilter, setFileFilter, FILE_FILTERS, matchesFilter, isPlainFolder, type FileFilter } from "../../state/view";
import { peerDrives } from "../../state/engine";
import { rootOf, serverOf, onServerText, driveName, offlinePeer } from "../../state/paths";
import { api } from "../../api/client";
import { base, bytes, join } from "../../lib/format";
import { IS_PS5 } from "../../lib/env";
import { t, plural, tMsg } from "../../lib/i18n";
import { ask, choose } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { Crumbs } from "../../components/Crumbs";
import { ViewSwitch } from "../../components/ViewSwitch";
import { openConvert } from "../../dialogs/ConvertDialog";
import { browse, refresh, refocusList } from "./browse";
import { Rail } from "./Rail";
import { FileList } from "./FileList";
import { openActions } from "./actions";
import { shares, shareFile, kindOfName } from "./shares";
import { selecting, selectedEntries, selectableCount, stopSelecting } from "../../state/selection";

function Summary() {
  const data = listing.value;
  if (!data) return <div class="summary" id="summary" />;
  if (selecting.value) {
    const sel = selectedEntries.value, size = sel.reduce((n, e) => n + (e.kind === "folder" ? 0 : e.size || 0), 0);
    return <div class="summary sel" id="summary">{t("{n} of {total} selected", { n: sel.length, total: selectableCount() }) + (size ? "  ·  " + bytes(size) : "")}</div>;
  }
  // A filter: how many of the folder's files and games it shows.
  const f = fileFilter.value;
  if (f !== "all" && data.path !== TOP) {
    const items = data.entries.filter((e) => !isPlainFolder(e));
    return <div class="summary" id="summary">{t("{n} of {total} shown", { n: items.filter((e) => matchesFilter(e, f)).length, total: items.length }) + "  ·  " + filterLabel(f)}</div>;
  }
  const files = data.entries.filter((e) => e.kind !== "folder"), folders = data.entries.length - files.length;
  const total = files.reduce((n, e) => n + (e.size || 0), 0);
  const r: any = data.path === TOP ? null : rootOf(data.path);
  const off = offlinePeer(data.path);
  const text = data.path === TOP ? t("Pick a drive") : off ? t("{engine} is offline", { engine: off.name }) :
    [folders ? plural(folders, "{n} folder", "{n} folders") : "", files.length ? plural(files.length, "{n} file", "{n} files") + " · " + bytes(total) : "",
      r && (r.total || !r.engine) ? t("{size} free on {drive}", { size: bytes(r.free), drive: driveName(r.root) }) : serverOf(data.path) ? onServerText(serverOf(data.path)!) : ""].filter(Boolean).join("  ·  ") || t("Empty");
  return <div class="summary" id="summary">{text}</div>;
}

export function FilesPage(props: { hidden: boolean }) {
  const pick = useRef<HTMLInputElement>(null);
  // The drive list shows paired engines' drives: ask for it again when they
  // change (one went offline, or came back).
  const drives = peerDrives.value.map((d) => d.root).join("\n");
  const seen = useRef(drives);
  useEffect(() => {
    if (seen.current === drives) return;
    seen.current = drives;
    if (cwd.value === TOP) browse(TOP, false);
  }, [drives]);
  const atTop = cwd.value === TOP;
  const shown = shares.value;
  return (
    <section class="view files" id="view-files" hidden={props.hidden}>
      <Rail />
      <div class="main">
        <div class="bar1">
          <div class="where">
            <Crumbs id="crumbs" zone="crumbs" path={cwd.value} onPick={(p) => browse(p)} />
            <Summary />
          </div>
          <div class="tools" data-zone="tools">
            <button class="btn sm" id="btn-share" hidden={IS_PS5} onClick={() => pick.current && pick.current.click()}>{t("From this computer…")}</button>
            <input ref={pick} type="file" id="share-pick" accept=".pkg,.exfat,.ffpfsc,.ffpkg,.ffpfs" hidden onChange={onPick} />
            {/* The folder you are in: Add to Places, Copy to..., and the rest. */}
            <button class="btn sm" id="btn-here" title={t("Options for this folder")} disabled={atTop}
              onClick={() => { if (cwd.value) openActions({ path: cwd.value, name: base(cwd.value), kind: "folder" }); }}>⋯</button>
            <button class="btn sm" id="btn-mkdir" disabled={atTop} onClick={newFolder}>{t("New folder")}</button>
            <button class="btn sm" data-key="files:filter" disabled={atTop} onClick={pickFilter}>
              {t("Show: {what}", { what: filterLabel(fileFilter.value) })}</button>
            <button class="btn sm ghost" id="btn-refresh" title={t("Refresh")} onClick={refresh}>{t("Refresh")}</button>
            <ViewSwitch which="files" />
          </div>
        </div>
        <div class="note sharebar" id="sharebar" hidden={!shown.length}>
          {shown.map((s) => t("Sharing {name} with the console ({sent} sent). Keep this page open until its job is done.", { name: s.file.name, sent: bytes(s.sent) })).join(" ")}
        </div>
        <FileList />
      </div>
    </section>
  );
}

const filterLabel = (f: FileFilter) =>
  f === "packages" ? t("Packages") : f === "images" ? t("Images") : f === "games" ? t("Game folders") : f === "other" ? t("Other files") : t("All");

// Which entries the list shows; a selection ends (it was made from other rows).
async function pickFilter() {
  const hints: Record<FileFilter, string> = {
    all: t("Every file and folder"), packages: t("FPKG packages"), images: t("FFPKG, FFPFSC, FFPFS and exFAT"),
    games: t("Dump folders"), other: t("Everything else"),
  };
  const f = await choose<FileFilter>(t("Show"), FILE_FILTERS.map((x) => ({ label: filterLabel(x), sub: hints[x], value: x, selected: x === fileFilter.value })),
    t("Folders stay, to move around"));
  if (!f || f === fileFilter.value) return;
  if (selecting.value) stopSelecting();
  setFileFilter(f);
  refocusList();
}

async function newFolder() {
  const name = await ask({ title: t("New folder"), input: t("New folder"), ok: t("Create") });
  if (!name || name === true) return;
  try { await api("POST", "/api/fs/mkdir", { path: join(cwd.value!, name) }); browse(cwd.value!, false, join(cwd.value!, name)); }
  catch (e: any) { toast(tMsg(e.message), "bad"); }
}

async function onPick(ev: Event) {
  const input = ev.target as HTMLInputElement;
  const file = input.files && input.files[0];
  input.value = "";
  if (!file) return;
  try {
    const path = await shareFile(file);
    openConvert({ name: file.name, path, kind: kindOfName(file.name), size: file.size, shared: true });
  } catch (err: any) { toast(tMsg(err.message), "bad"); }
}
