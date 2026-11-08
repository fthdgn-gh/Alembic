// SPDX-License-Identifier: GPL-3.0-or-later
// One entry of a grid (Files and Images in grid view): the game's icon, a
// pill on it when something needs doing, the title, the file's name (when
// the title is the game's), its pills.
import type { ComponentChildren } from "preact";
import { t } from "../lib/i18n";

export function Tile(props: {
  dataKey: string; icon: ComponentChildren; title: string; name?: string; tip?: string; pills: ComponentChildren; badge?: ComponentChildren;
  selecting?: boolean; ticked?: boolean; dim?: boolean; tileRef?: any;
  onClick: () => void; onFocus?: () => void; onMore?: () => void;
}) {
  const p = props;
  return (
    <div ref={p.tileRef} class={"tile" + (p.ticked ? " sel" : "")} style={p.dim ? "opacity:.6" : undefined} tabIndex={0} data-key={p.dataKey}
      aria-checked={p.selecting ? !!p.ticked : undefined} onClick={p.onClick} onFocus={p.onFocus}
      onContextMenu={p.onMore ? (ev) => { ev.preventDefault(); p.onMore!(); } : undefined}>
      {p.selecting ? <span class={"tick" + (p.ticked ? " on" : "")}>{p.ticked ? "✓" : ""}</span> : null}
      <div class="art">{p.icon}{p.badge ? <div class="tbadge">{p.badge}</div> : null}</div>
      <div class="t" title={p.tip || p.title}>{p.title}</div>
      <div class="n">{p.name && p.name !== p.title ? p.name : "\u00a0"}</div>
      <div class="s">{p.pills}</div>
      {p.onMore ? <button class="more" tabIndex={-1} data-nonav title={t("Options")} onClick={(ev) => { ev.stopPropagation(); p.onMore!(); }}>⋯</button> : null}
    </div>
  );
}
