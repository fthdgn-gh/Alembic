// SPDX-License-Identifier: GPL-3.0-or-later
// Pieces several screens use: icons, controller glyphs, form rows, game info.
import type { ComponentChildren, RefObject } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { signal } from "@preact/signals";
import type { Entry, Meta } from "./types";
import { KIND_LABEL, pad, circleConfirms } from "./state";
import { steppers } from "./nav";
import { t } from "./i18n";
import { api } from "./util";

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

/* ---------- game info: titles and icons, two requests at a time ---------- */
const meta = { cache: new Map<string, Meta | null>(), queue: [] as { e: Entry; res: (m: Meta | null) => void }[], active: 0, waiting: new Map<string, Promise<Meta | null>>() };
export const metaVersion = signal(0); // bumped when the cache is cleared
export const metaKey = (e: { path: string; mtime?: number; size?: number }) => e.path + "|" + (e.mtime || 0) + "|" + (e.size || 0);
export const iconUrl = (e: { path: string; mtime?: number }) => "/api/icon?path=" + encodeURIComponent(e.path) + "&v=" + (e.mtime || 0);
export function clearMeta() { meta.cache.clear(); meta.waiting.clear(); metaVersion.value++; }
export function cachedMeta(e: Entry): Meta | null | undefined { return meta.cache.get(metaKey(e)); }

export function getMeta(e: Entry): Promise<Meta | null> {
  const k = metaKey(e);
  if (meta.cache.has(k)) return Promise.resolve(meta.cache.get(k) as Meta | null);
  const w = meta.waiting.get(k);
  if (w) return w;
  const p = new Promise<Meta | null>((res) => { meta.queue.push({ e, res }); pump(); });
  meta.waiting.set(k, p);
  return p;
}
function pump() {
  while (meta.active < 2 && meta.queue.length) {
    const job = meta.queue.shift()!;
    meta.active++;
    api<Meta>("GET", "/api/meta?path=" + encodeURIComponent(job.e.path))
      .then((m) => { meta.cache.set(metaKey(job.e), m); job.res(m); })
      .catch(() => { meta.cache.set(metaKey(job.e), null); job.res(null); })
      .then(() => { meta.waiting.delete(metaKey(job.e)); meta.active--; pump(); });
  }
}

// The game info of `e`, asked for once `ref` comes near the screen.
export function useMeta(e: Entry | null, ref?: RefObject<Element>): Meta | null {
  const k = e ? metaKey(e) : "";
  const [m, setM] = useState<Meta | null>(() => (e ? cachedMeta(e) || null : null));
  const version = metaVersion.value;
  useEffect(() => {
    if (!e) { setM(null); return; }
    let live = true;
    const go = () => getMeta(e).then((x) => { if (live) setM(x); });
    if (meta.cache.has(k)) { setM(meta.cache.get(k) || null); return; }
    setM(null);
    const el = ref && ref.current;
    if (!el || !("IntersectionObserver" in window)) { go(); return () => { live = false; }; }
    const io = new IntersectionObserver((list) => { if (list.some((x) => x.isIntersecting)) { io.disconnect(); go(); } }, { rootMargin: "300px" });
    io.observe(el);
    return () => { live = false; io.disconnect(); };
  }, [k, version]);
  return m;
}

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

/* ---------- form rows ---------- */
export interface Option<T> { value: T; label: string }

// A row whose value changes with ← and → (and a click on its arrows or the row).
export function ChoiceRow<T>(props: { label: string; hint?: string | null; options: Option<T>[]; value: T; onChange: (v: T) => void; hidden?: boolean; dataKey?: string }) {
  const { options, value, onChange } = props;
  const i = Math.max(0, options.findIndex((o) => o.value === value));
  const ref = useRef<HTMLDivElement>(null);
  const step = (d: number) => { const n = i + d; if (n < 0 || n >= options.length) return; onChange(options[n].value); };
  useEffect(() => { if (ref.current) steppers.set(ref.current, step); });
  return (
    <div ref={ref} class="frow fx" tabIndex={0} hidden={props.hidden} data-key={props.dataKey} onClick={() => step(i === options.length - 1 ? -(options.length - 1) : 1)}>
      <div class="fl"><b>{props.label}</b>{props.hint ? <small>{props.hint}</small> : null}</div>
      <div class="val">
        <span class="arr" onClick={(e) => { e.stopPropagation(); step(-1); }}>‹</span>
        <span class="valtext">{options[i] ? options[i].label : ""}</span>
        <span class="arr" onClick={(e) => { e.stopPropagation(); step(1); }}>›</span>
      </div>
    </div>
  );
}

export function SwitchRow(props: { label: string; hint?: string | null; on: boolean; onChange: (on: boolean) => void; hidden?: boolean; dataKey?: string }) {
  return (
    <button class={"frow" + (props.on ? " on" : "")} role="switch" aria-checked={!!props.on} hidden={props.hidden} data-key={props.dataKey} onClick={() => props.onChange(!props.on)}>
      <div class="fl"><b>{props.label}</b>{props.hint ? <small>{props.hint}</small> : null}</div>
      <span class="sw" />
    </button>
  );
}

// Two columns of label and value.
export function Facts(props: { rows: [string, ComponentChildren, string?][] }) {
  return <div class="facts">{props.rows.map(([k, v, cls]) => [<div>{k}</div>, <div class={cls || undefined}>{v}</div>])}</div>;
}
