// SPDX-License-Identifier: GPL-3.0-or-later
// Images: every image, game folder and package in the folders ShadowMount+
// scans (hidden ones too), grouped by folder, with what Files offers for
// them except moving and renaming (actions.tsx, bulk.tsx with `lib`), and
// each game's AMPR emulator: an update when a newer build of it is out,
// keeping the game's emulator (ampr_emu or ampr-emu-ng).
import { useEffect, useRef, useState } from "preact/hooks";
import { signal } from "@preact/signals";
import type { ImageItem } from "../../api/types";
import { tab } from "../../state/view";
import { images, loadImages } from "../../state/images";
import { jobs, isActive } from "../../state/jobs";
import { shownPath } from "../../state/paths";
import { selecting, isSelected, toggleSelected } from "../../state/selection";
import { bytes, date } from "../../lib/format";
import { KIND_LABEL } from "../../lib/kinds";
import { t, plural } from "../../lib/i18n";
import { overlays } from "../../components/Overlay";
import { EntryIco } from "../../components/Icons";
import { PlatformPill } from "../../components/PlatformPill";
import { useMeta } from "../../hooks/useMeta";
import { focusFirst, zoneOf } from "../../input/focus";
import { openConvert } from "../../dialogs/ConvertDialog";
import { openActions } from "../files/actions";
import { amprPending, amprUpdatable, targetName, updateAll } from "./ampr";

export const focusImage = signal<ImageItem | null>(null); // the focused row (for the hints)

// The AMPR states worth a pill: something to do.
const IMAGE_STATE: Record<string, [string, string]> = {
  update: ["running", "update available"], missing: ["failed", "no emulator"],
  unknown: ["queued", "unknown build"], not_downloaded: ["queued", "not downloaded here"],
};
const USES_APR = ["current", "update", "missing", "unknown", "not_downloaded"];

// The AMPR emulator, for the row's second line ("" when the game doesn't use the APR).
function amprText(i: ImageItem): string {
  const a = i.ampr;
  if (!a || USES_APR.indexOf(a.status) < 0) return "";
  if (a.emulator) {
    const next = a.status === "update" || a.status === "not_downloaded" ? " → " + targetName(i) : "";
    return a.emulator.name + " · " + (a.where === "backport" ? t("in its backport") : t("in the game")) + next;
  }
  return a.status === "unknown" ? t("a libSceAmpr.sprx Alembic doesn't know") : t("no AMPR emulator");
}

export function openImage(i: ImageItem) {
  if (selecting.value) return toggleSelected(i);
  openConvert(i);
}

function Row(props: { i: ImageItem }) {
  const { i } = props;
  const ref = useRef<HTMLDivElement>(null);
  const m = useMeta(i, ref);
  const queued = amprPending.value.indexOf(i.path) >= 0;
  const st = queued ? ["queued", "queued"] : i.ampr ? IMAGE_STATE[i.ampr.status] : undefined;
  const ampr = amprText(i);
  const sel = selecting.value, ticked = sel && isSelected(i);
  const title = m && m.title ? m.title : i.name;
  return (
    <div ref={ref} class={"row" + (ticked ? " sel" : "")} style={i.hidden ? "opacity:.6" : undefined} tabIndex={0} data-key={"img:" + i.path}
      aria-checked={sel ? ticked : undefined} onClick={() => openImage(i)} onFocus={() => { focusImage.value = i; }}
      onContextMenu={(ev) => { ev.preventDefault(); openActions(i, i); }}>
      {sel ? <span class={"tick" + (ticked ? " on" : "")}>{ticked ? "✓" : ""}</span> : null}
      <EntryIco entry={i} meta={m} />
      <div style="min-width:0">
        <div class="t ellip" title={i.path}>{title}</div>
        <div class="s ellip">
          <PlatformPill meta={m} />
          <span class={"fmt " + (i.game ? "folder" : i.kind)}>{i.game ? t("FOLDER") : KIND_LABEL[i.kind] || ""}</span>
          {m && (m.packs === "lz4" || m.packs === "zstd") ? <span class="fmt packs">{m.packs.toUpperCase()}</span> : null}
          {i.hidden ? <span class="fmt">{t("HIDDEN")}</span> : null}
          {ampr || [m && m.title ? i.name : "", m && m.version ? "v" + m.version.replace(/^0+(?=\d)/, "") : "", m ? m.title_id : date(i.mtime)].filter(Boolean).join("  ·  ")}
        </div>
      </div>
      <div class="sz">{st ? <span class={"state " + st[0]}>{t(st[1])}</span> : i.kind !== "folder" ? bytes(i.size) : ""}</div>
      <button class="more" tabIndex={-1} data-nonav title={t("Options")} onClick={(ev) => { ev.stopPropagation(); openActions(i, i); }}>⋯</button>
    </div>
  );
}

export function ImagesPage(props: { hidden: boolean }) {
  const data = images.value;
  const [busy, setBusy] = useState(false);
  // Read again each time the tab opens (the engine reads the folders again
  // when its list is older than ten minutes).
  useEffect(() => { if (tab.value === "images") loadImages(); }, [tab.value]);
  // While the engine reads the folders, look again every second and a half.
  useEffect(() => {
    if (!data || !data.refreshing || tab.value !== "images") return;
    const id = setTimeout(() => loadImages(), 1500);
    return () => clearTimeout(id);
  }, [data, tab.value]);
  // A job ended (a conversion's output, an emulator put in place): read the
  // folders again. AMPR jobs queued from here stop showing "queued" then.
  const active = jobs.value.filter(isActive).length;
  const before = useRef(active);
  useEffect(() => {
    const ended = active < before.current;
    before.current = active;
    const waiting = jobs.value.some((j) => isActive(j) && j.spec.kind === "ampr_emu" && amprPending.value.indexOf(j.spec.input) >= 0);
    if (amprPending.value.length && !waiting) amprPending.value = [];
    if (ended && tab.value === "images") loadImages(true);
  }, [active]);
  useEffect(() => {
    if (tab.value === "images" && data && !overlays.value.length && zoneOf(document.activeElement) !== "images") setTimeout(focusFirst, 0);
  }, [data && data.updated]);
  const list = data ? data.images.slice().sort((a, b) => a.folder.localeCompare(b.folder) || a.name.replace(/^\./, "").localeCompare(b.name.replace(/^\./, ""))) : [];
  const todo = list.filter(amprUpdatable).length;
  async function again() { setBusy(true); await loadImages(true); setBusy(false); }
  const rows: any[] = [];
  let folder = "";
  for (const i of list) {
    if (i.folder !== folder) {
      folder = i.folder;
      rows.push(<div class="sect" style="margin:1rem .3rem .3rem">{shownPath(folder)}</div>);
    }
    rows.push(<Row key={i.path} i={i} />);
  }
  return (
    <section class="view scroll" id="view-images" hidden={props.hidden}>
      <div class="page" data-zone="images">
        <p class="muted" style="margin:.3rem .2rem 1rem">{t("Every image, game folder and package in the folders ShadowMount+ scans. For games that read their files through the console's APR, the AMPR emulator each one runs with: an update keeps the game's emulator and puts its newest build in place.")}</p>
        {data && data.smp_update_emulators ? <div class="note" style="margin-bottom:1rem">{t("ShadowMount+'s “update emulators” is on: when a game starts, it puts its own copy in place of a matching emulator. Turn it off in ShadowMount+ for games that should keep the one shown here.")}</div> : null}
        <div style="display:flex;align-items:center;gap:.5rem;margin-bottom:.4rem">
          <span class="muted" style="flex:1">
            {!data ? t("Looking for images…") : data.refreshing && !list.length ? t("Reading the games…")
              : [plural(list.length, "{n} item", "{n} items"), todo ? plural(todo, "{n} AMPR emulator to update", "{n} AMPR emulators to update") : "",
                data.refreshing ? t("Reading the games…") : ""].filter(Boolean).join(" · ")}
          </span>
          <button class="btn sm" data-key="images:again" disabled={busy || !data || data.refreshing} onClick={again}>{t("Check again")}</button>
          <button class="btn sm primary" data-key="images:all" disabled={!todo} onClick={updateAll}>{todo ? t("Update AMPR emulators ({n})", { n: todo }) : t("Update AMPR emulators")}</button>
        </div>
        <div class="list" data-column>
          {data && !list.length && !data.refreshing ? <div class="empty">{t("No images, game folders or packages in ShadowMount+'s folders.")}</div> : rows}
        </div>
        {data && data.dirs.length ? <p class="muted" style="margin:1rem .2rem">{t("Folders searched: {list}", { list: data.dirs.map(shownPath).join(", ") })}</p> : null}
      </div>
    </section>
  );
}
