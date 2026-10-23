// SPDX-License-Identifier: GPL-3.0-or-later
// Icons: drive, folder and file symbols, game icons with their format
// label until they load, and the controller's button glyphs.
import { useEffect, useState } from "preact/hooks";
import type { Entry, Meta } from "../api/types";
import { iconUrl } from "../api/meta";
import { KIND_LABEL } from "../lib/kinds";
import { pad, circleConfirms } from "../state/view";
import { t } from "../lib/i18n";

export const ICONS = {
  folder: () => <svg viewBox="0 0 24 24" fill="currentColor"><path d="M3 6.5A2.5 2.5 0 0 1 5.5 4h3.6c.6 0 1.2.3 1.6.7L12 6h6.5A2.5 2.5 0 0 1 21 8.5v9a2.5 2.5 0 0 1-2.5 2.5h-13A2.5 2.5 0 0 1 3 17.5z" /></svg>,
  drive: () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="3" y="7" width="18" height="10" rx="2" /><circle cx="17" cy="12" r="1" fill="currentColor" /></svg>,
  usb: () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="7" y="9" width="10" height="12" rx="1.5" /><path d="M9 9V4h6v5" /></svg>,
  server: () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="4" y="4" width="16" height="7" rx="1.5" /><rect x="4" y="13" width="16" height="7" rx="1.5" /><circle cx="8" cy="7.5" r=".9" fill="currentColor" /><circle cx="8" cy="16.5" r=".9" fill="currentColor" /></svg>,
  place: () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M4 7.5A1.5 1.5 0 0 1 5.5 6H10l2 2h6.5A1.5 1.5 0 0 1 20 9.5v8a1.5 1.5 0 0 1-1.5 1.5h-13A1.5 1.5 0 0 1 4 17.5z" /></svg>,
  clock: () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="12" cy="12" r="8" /><path d="M12 8v4l3 2" /></svg>,
  file: () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M7 3h7l4 4v14H7z" /><path d="M14 3v4h4" /></svg>,
};
export type IconName = keyof typeof ICONS;

// The square in front of a row: a folder or file symbol, or a format label
// ("EXFAT", "GAME") until the game's icon has loaded.
export function Ico(props: { kind: string; label?: string; icon?: string | null }) {
  const { kind, icon } = props;
  const [loaded, setLoaded] = useState<string | null>(null);
  useEffect(() => {
    if (!icon || loaded === icon) return;
    let live = true;
    const img = new Image();
    img.onload = () => { if (live) setLoaded(icon); };
    img.src = icon;
    return () => { live = false; };
  }, [icon]);
  if (icon && loaded === icon) return <div class={"ico " + kind + " has-img"}><img alt="" src={icon} /></div>;
  if (kind === "folder") return <div class="ico folder">{ICONS.folder()}</div>;
  if (kind === "file") return <div class="ico file">{ICONS.file()}</div>;
  return <div class={"ico " + kind}>{props.label}</div>;
}
// An entry's square, with its icon when the game info has one.
export function EntryIco(props: { entry: Entry; meta?: Meta | null }) {
  const e = props.entry, m = props.meta;
  const kind = e.kind === "folder" && !e.game ? "folder" : e.kind === "file" ? "file" : e.game ? "game" : e.kind;
  return <Ico kind={kind} label={e.game ? t("GAME") : KIND_LABEL[e.kind] || ""} icon={m && m.icon ? iconUrl(e) : null} />;
}


/* ---------- controller glyphs ---------- */
const GLYPH: Record<string, string> = {
  x: '<svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18" stroke="#8aa7ff" stroke-width="2.6" stroke-linecap="round"/></svg>',
  o: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="6.5" fill="none" stroke="#ff6f8f" stroke-width="2.4"/></svg>',
  t: '<svg viewBox="0 0 24 24"><path d="M12 5l7.5 13h-15z" fill="none" stroke="#45d6a8" stroke-width="2.3" stroke-linejoin="round"/></svg>',
  s: '<svg viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="1" fill="none" stroke="#e993dc" stroke-width="2.3"/></svg>',
  dpad: '<svg viewBox="0 0 24 24" fill="#c9d1e2"><path d="M9.5 3h5v5.5H9.5zM9.5 15.5h5V21h-5zM3 9.5h5.5v5H3zM15.5 9.5H21v5h-5.5z" opacity=".9"/></svg>',
};
const KEYS: Record<string, string> = { x: "Enter", o: "Esc", t: "A", s: "S", lr: "Q E", dpad: "←→" };
export function Glyph(props: { b: string }) {
  const b = props.b;
  if (b === "l2r2") return <span><span class="key">L2</span><span class="key">R2</span></span>;
  if (b === "lr") return pad.value ? <span><span class="key">L1</span><span class="key">R1</span></span> : <span><span class="key">Q</span><span class="key">E</span></span>;
  if (pad.value) return <span dangerouslySetInnerHTML={{ __html: GLYPH[circleConfirms() && (b === "x" || b === "o") ? (b === "x" ? "o" : "x") : b] }} />;
  return <span class="key">{KEYS[b]}</span>;
}

