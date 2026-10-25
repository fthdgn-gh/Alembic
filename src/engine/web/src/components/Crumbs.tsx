// SPDX-License-Identifier: GPL-3.0-or-later
// Breadcrumbs: Drives › drive › folder › ... (Files and the folder picker).
import { rootOf, ftpOf, driveName, shownPath, peerRoot } from "../state/paths";
import { base } from "../lib/format";
import { t } from "../lib/i18n";

export function Crumbs(props: { path: string | null; onPick: (p: string) => void; id?: string; style?: string; zone?: string }) {
  const path = props.path;
  const parts: string[] = [];
  let rootPath = "";
  if (path && path !== "/") {
    const r = rootOf(path), f = r ? null : ftpOf(path);
    // An engine whose drives are not known (offline): its name, then the path on it.
    rootPath = r ? r.root : f ? f.path : peerRoot(path) || path;
    parts.push(rootPath);
    let acc = rootPath;
    for (const p of path.slice(rootPath.length).split("/").filter(Boolean)) { acc += "/" + p; parts.push(acc); }
  }
  return (
    <div class="crumbs" id={props.id} data-zone={props.zone} style={props.style}>
      <button data-key="c:/" onClick={() => props.onPick("/")}>{t("Drives")}</button>
      {parts.map((p) => [
        <span class="sep">›</span>,
        <button data-key={"c:" + p} onClick={() => props.onPick(p)} title={shownPath(p)}>{p === rootPath ? driveName(p) : base(p)}</button>,
      ])}
    </div>
  );
}
