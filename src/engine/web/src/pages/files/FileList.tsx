// SPDX-License-Identifier: GPL-3.0-or-later
// The folder's contents: a row per entry, with game info once it has loaded.
import { useRef } from "preact/hooks";
import type { Entry } from "../../api/types";
import { cwd, listing, focusEntry, viewModes } from "../../state/view";
import { Tile } from "../../components/Tile";
import { rootOf, serverOf, serverAddress, driveName, offlinePeer, shownPath } from "../../state/paths";
import { isConvertible, KIND_LABEL } from "../../lib/kinds";
import { base, bytes, date } from "../../lib/format";
import { t } from "../../lib/i18n";
import { ICONS, EntryIco } from "../../components/Icons";
import { useMeta } from "../../hooks/useMeta";
import { PlatformPill } from "../../components/PlatformPill";
import { openConvert } from "../../dialogs/ConvertDialog";
import { browse } from "./browse";
import { openActions } from "./actions";
import { selecting, isSelected, toggleSelected } from "../../state/selection";

function openEntry(e: Entry) {
  if (cwd.value === "/" || (e.kind === "folder" && !e.game)) return browse(e.path);
  if (isConvertible(e)) return openConvert(e);
  openActions(e);
}

function Row(props: { e: Entry; top: boolean }) {
  const { e, top } = props;
  const ref = useRef<HTMLDivElement>(null);
  const m = useMeta(isConvertible(e) && !top ? e : null, ref);
  const fs = top ? serverOf(e.path) : null;
  const r = top ? rootOf(e.path) : null;
  const gone = top ? offlinePeer(e.path) : null; // a paired engine that is off
  const plain = top ? (r ? t("{free} free of {total}", { free: bytes(r.free), total: bytes(r.total as number) }) : fs ? serverAddress(fs, true) : gone ? t("offline") : shownPath(e.path))
    : [e.kind === "folder" ? "" : bytes(e.size), date(e.mtime)].filter(Boolean).join("  ·  ");
  const title = !top && m && m.title ? m.title : null;
  // Selecting: ✕ (a click) ticks the row instead of opening it.
  const sel = selecting.value && !top, ticked = sel && isSelected(e);
  return (
    <div ref={ref} class={"row" + (ticked ? " sel" : "")} tabIndex={0} data-key={e.path} aria-checked={sel ? ticked : undefined}
      onClick={() => (sel ? toggleSelected(e) : openEntry(e))} onFocus={() => { focusEntry.value = e; }}
      onContextMenu={top ? undefined : (ev) => { ev.preventDefault(); openActions(e); }}>
      {sel ? <span class={"tick" + (ticked ? " on" : "")}>{ticked ? "✓" : ""}</span> : null}
      {top ? <div class="ico folder">{fs ? ICONS.server() : /usb|ext/.test(e.path) ? ICONS.usb() : ICONS.drive()}</div> : <EntryIco entry={e} meta={m} />}
      <div style="min-width:0">
        <div class="t ellip" title={title || e.name}>{gone ? gone.name + " · " + base(e.path) : top ? driveName(e.path) : title || e.name}</div>
        <div class="s ellip">
          {m ? [
            <PlatformPill meta={m} />,
            <span class={"fmt " + (e.game ? "folder" : e.kind)}>{e.game ? t("FOLDER") : KIND_LABEL[e.kind] || ""}</span>,
            // Asset packs inside: which AMPR emulator the game needs.
            m.packs === "lz4" || m.packs === "zstd" ? <span class="fmt packs" title={m.packs === "zstd" ? t("zstd-packed") : t("LZ4-packed")}>{m.packs.toUpperCase()}</span> : null,
            [m.title ? e.name : "", m.version ? "v" + m.version.replace(/^0+(?=\d)/, "") : "", m.title_id].filter(Boolean).join("  ·  "),
          ] : plain}
        </div>
      </div>
      <div class="sz">{!top && e.kind !== "folder" ? bytes(e.size) : ""}</div>
      {top ? <span /> : <button class="more" tabIndex={-1} data-nonav title={t("Options")} onClick={(ev) => { ev.stopPropagation(); openActions(e); }}>⋯</button>}
    </div>
  );
}

// A drive, server or other engine's drive as a tile: its symbol, name and room.
function DriveCell(props: { e: Entry }) {
  const { e } = props;
  const fs = serverOf(e.path), r = rootOf(e.path), gone = offlinePeer(e.path);
  const room = r ? t("{free} free of {total}", { free: bytes(r.free), total: bytes(r.total as number) }) : fs ? serverAddress(fs, true) : gone ? t("offline") : shownPath(e.path);
  const used = r && r.total ? Math.max(0, Math.min(1, 1 - r.free / (r.total as number))) : -1;
  return (
    <Tile dataKey={e.path} icon={<div class="ico folder">{fs ? ICONS.server() : /usb|ext/.test(e.path) ? ICONS.usb() : ICONS.drive()}</div>}
      title={gone ? gone.name + " · " + base(e.path) : driveName(e.path)} onClick={() => browse(e.path)} onFocus={() => { focusEntry.value = e; }}
      pills={[used >= 0 ? <div class={"meter" + (used > 0.9 ? " low" : "")} style="margin:.1rem 0 .35rem"><div style={"width:" + Math.round(used * 100) + "%"} /></div> : null, room]} />
  );
}

// A tile of the grid view: the same entry, its icon large.
function Cell(props: { e: Entry }) {
  const { e } = props;
  const ref = useRef<HTMLDivElement>(null);
  const m = useMeta(isConvertible(e) ? e : null, ref);
  const sel = selecting.value, ticked = sel && isSelected(e);
  return (
    <Tile tileRef={ref} dataKey={e.path} icon={<EntryIco entry={e} meta={m} />} title={m && m.title ? m.title : e.name} name={e.name} tip={e.name}
      selecting={sel} ticked={ticked} onClick={() => (sel ? toggleSelected(e) : openEntry(e))} onFocus={() => { focusEntry.value = e; }}
      onMore={() => openActions(e)}
      pills={[m ? <PlatformPill meta={m} /> : null,
        <span class={"fmt " + (e.game ? "folder" : e.kind)}>{e.game ? t("FOLDER") : KIND_LABEL[e.kind] || (e.kind === "folder" ? t("FOLDER") : "")}</span>,
        m && (m.packs === "lz4" || m.packs === "zstd") ? <span class="fmt packs">{m.packs.toUpperCase()}</span> : null,
        e.kind !== "folder" ? bytes(e.size) : ""]} />
  );
}

export function FileList() {
  const data = listing.value;
  const grid = viewModes.value.files === "grid" && !!data;
  const top = !!data && data.path === "/";
  return (
    <div class={"list scroll" + (grid ? " grid" : "")} id="list" data-zone="list" data-column={grid ? undefined : true}>
      {!data ? null : !data.entries.length ? <div class="empty">{t("This folder is empty.")}</div>
        : grid ? data.entries.map((e) => (top ? <DriveCell key={e.path} e={e} /> : <Cell key={e.path} e={e} />))
        : data.entries.map((e) => <Row key={e.path} e={e} top={data.path === "/"} />)}
    </div>
  );
}
