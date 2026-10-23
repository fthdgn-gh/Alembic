// SPDX-License-Identifier: GPL-3.0-or-later
// The folder's contents: a row per entry, with game info once it has loaded.
import { useRef } from "preact/hooks";
import type { Entry } from "../../api/types";
import { cwd, listing, focusEntry } from "../../state/view";
import { rootOf, ftpOf, driveName } from "../../state/paths";
import { isConvertible, KIND_LABEL } from "../../lib/kinds";
import { bytes, date } from "../../lib/format";
import { t } from "../../lib/i18n";
import { ICONS, EntryIco } from "../../components/Icons";
import { useMeta } from "../../hooks/useMeta";
import { openConvert } from "../../dialogs/ConvertDialog";
import { browse } from "./browse";
import { openActions } from "./actions";

function openEntry(e: Entry) {
  if (cwd.value === "/" || (e.kind === "folder" && !e.game)) return browse(e.path);
  if (isConvertible(e)) return openConvert(e);
  openActions(e);
}

function Row(props: { e: Entry; top: boolean }) {
  const { e, top } = props;
  const ref = useRef<HTMLDivElement>(null);
  const m = useMeta(isConvertible(e) && !top ? e : null, ref);
  const fs = top ? ftpOf(e.path) : null;
  const r = top ? rootOf(e.path) : null;
  const plain = top ? (r ? t("{free} free of {total}", { free: bytes(r.free), total: bytes(r.total as number) }) : fs ? "FTP · " + fs.user + "@" + fs.host + ":" + fs.port : e.path)
    : [e.kind === "folder" ? "" : bytes(e.size), date(e.mtime)].filter(Boolean).join("  ·  ");
  const title = !top && m && m.title ? m.title : null;
  return (
    <div ref={ref} class="row" tabIndex={0} data-key={e.path} onClick={() => openEntry(e)} onFocus={() => { focusEntry.value = e; }}
      onContextMenu={top ? undefined : (ev) => { ev.preventDefault(); openActions(e); }}>
      {top ? <div class="ico folder">{fs ? ICONS.server() : /usb|ext/.test(e.path) ? ICONS.usb() : ICONS.drive()}</div> : <EntryIco entry={e} meta={m} />}
      <div style="min-width:0">
        <div class="t ellip" title={title || e.name}>{top ? driveName(e.path) : title || e.name}</div>
        <div class="s ellip">
          {m ? [
            <span class={"fmt " + (e.game ? "folder" : e.kind)}>{e.game ? t("FOLDER") : KIND_LABEL[e.kind] || ""}</span>,
            [m.title ? e.name : "", m.version ? "v" + m.version.replace(/^0+(?=\d)/, "") : "", m.title_id].filter(Boolean).join("  ·  "),
          ] : plain}
        </div>
      </div>
      <div class="sz">{!top && e.kind !== "folder" ? bytes(e.size) : ""}</div>
      {top ? <span /> : <button class="more" tabIndex={-1} data-nonav title={t("Options")} onClick={(ev) => { ev.stopPropagation(); openActions(e); }}>⋯</button>}
    </div>
  );
}

export function FileList() {
  const data = listing.value;
  return (
    <div class="list scroll" id="list" data-zone="list" data-column>
      {!data ? null : !data.entries.length ? <div class="empty">{t("This folder is empty.")}</div>
        : data.entries.map((e) => <Row key={e.path} e={e} top={data.path === "/"} />)}
    </div>
  );
}
