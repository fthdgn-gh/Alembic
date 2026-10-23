// SPDX-License-Identifier: GPL-3.0-or-later
// Dialogs: sheets over the page. Each has its own history entry, so ○ (the
// browser's Back) closes it; closeOverlay(value) goes back, and the entry's
// popstate resolves the promise openOverlay returned.
import type { ComponentChildren, VNode } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { signal } from "@preact/signals";
import { focusEl, focusFirst, goBack, pushNav } from "./nav";
import { tab, cwd } from "./state";
import { t } from "./i18n";
import { IS_PS5 } from "./util";

// A button hint: glyph ("x", "o", "s", "lr", "dpad", "l2r2"), label, action.
export type Hint = [string, string, (() => void)?];

export interface Overlay {
  id: number;
  render: () => VNode;
  hints?: Hint[];
  onSquare?: () => void;
  back: Element | null; // where the focus was before it opened
  result: unknown;
  resolve: (v: any) => void;
  el?: HTMLElement;
}

export const overlays = signal<Overlay[]>([]);
export const topOverlay = (): Overlay | null => { const o = overlays.value; return o.length ? o[o.length - 1] : null; };
let ids = 0;

export function openOverlay<T = unknown>(render: () => VNode, opts?: { hints?: Hint[]; onSquare?: () => void }): Promise<T | null> {
  let resolve!: (v: T | null) => void;
  const p = new Promise<T | null>((res) => { resolve = res; });
  const o: Overlay = { id: ++ids, render, hints: opts && opts.hints, onSquare: opts && opts.onSquare, back: document.activeElement, result: null, resolve };
  overlays.value = overlays.value.concat(o);
  pushNav({ overlay: true, view: tab.value, path: cwd.value });
  setTimeout(() => focusFirst(), 0);
  return p;
}

export function closeOverlay(value?: unknown) {
  const o = topOverlay();
  if (!o) return;
  o.result = value === undefined ? null : value;
  history.back();
}

// The overlay's history entry is gone (popstate): remove it and resolve.
export function popOverlay() {
  const list = overlays.value, o = list[list.length - 1];
  overlays.value = list.slice(0, -1);
  // The element focus came from, or (redrawn meanwhile) the one with its data-key.
  const back = o.back as HTMLElement | null;
  const key = back && back.getAttribute && back.getAttribute("data-key");
  const again = back && !back.isConnected && key ? (Array.prototype.slice.call(document.querySelectorAll("[data-key]")) as HTMLElement[]).filter((x) => x.getAttribute("data-key") === key)[0] : null;
  if (back && back.isConnected) focusEl(back); else if (again) focusEl(again); else setTimeout(focusFirst, 0);
  o.resolve(o.result);
}

// Every open overlay; only the top one takes input.
export function Overlays() {
  const list = overlays.value;
  // What an overlay covers loses its scrollbars (style.css: body.covered).
  useEffect(() => { document.body.classList.toggle("covered", list.length > 0); }, [list.length]);
  return (
    <>
      {list.map((o, i) => (
        <div key={o.id} class={"overlay" + (i < list.length - 1 ? " under" : "")} ref={(el) => { o.el = el || undefined; }}
          onClick={(ev) => { if (ev.target === ev.currentTarget && i === list.length - 1) closeOverlay(null); }}>
          {o.render()}
        </div>
      ))}
    </>
  );
}

export function Sheet(props: { title?: string; sub?: string; cls?: string; foot?: ComponentChildren; children?: ComponentChildren }) {
  return (
    <div class={"sheet " + (props.cls || "")}>
      {props.title ? <div class="sh-head">{props.title}</div> : null}
      {props.sub ? <div class="sh-sub mono">{props.sub}</div> : null}
      <div class="sh-body scroll">{props.children}</div>
      {props.foot ? <div class="sh-foot">{props.foot}</div> : null}
    </div>
  );
}

interface AskOptions { title: string; body?: ComponentChildren; input?: string; ok?: string; danger?: boolean }

// A question with OK and Cancel, or a line of text to enter: resolves with
// true, the text, or null.
export function ask(o: AskOptions): Promise<string | true | null> {
  return openOverlay<string | true>(() => <Ask {...o} />, { hints: [["x", t("Choose")], ["o", t("Cancel"), goBack]] });
}
function Ask(o: AskOptions) {
  const [value, setValue] = useState(o.input != null ? o.input : "");
  const input = useRef<HTMLInputElement>(null);
  const hasInput = o.input != null;
  // Typing first on a PC; on the console the keyboard opens only when asked.
  useEffect(() => { if (hasInput && !IS_PS5) setTimeout(() => input.current && input.current.select(), 10); }, []);
  const ok = () => closeOverlay(hasInput ? value.trim() : true);
  return (
    <Sheet title={o.title} cls="narrow" foot={<>
      <button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class={"btn " + (o.danger ? "danger solid" : "primary")} data-autofocus={hasInput && !IS_PS5 ? undefined : ""} onClick={ok}>{o.ok || t("OK")}</button>
    </>}>
      <div>
        {o.body || null}
        {hasInput ? (
          <div style="margin-top:.6rem">
            <input ref={input} class="input" value={value} spellcheck={false} autocapitalize="off" data-autofocus={IS_PS5 ? undefined : ""}
              onInput={(e) => setValue((e.target as HTMLInputElement).value)}
              onKeyDown={(e) => { if (e.key === "Enter") { e.preventDefault(); ok(); } }} />
          </div>
        ) : null}
      </div>
    </Sheet>
  );
}

// A choice, or a heading between groups (not a choice: the D-pad passes over it).
export type ChoiceItem<T> = { label: string; sub?: string | null; value: T; selected?: boolean; danger?: boolean } | { heading: string };

// A list of choices; resolves with the chosen item's value.
export function choose<T>(title: string, items: ChoiceItem<T>[], sub?: string): Promise<T | null> {
  const firstChoice = items.findIndex((x) => !("heading" in x));
  const anySelected = items.some((x) => !("heading" in x) && x.selected);
  return openOverlay<T>(() => (
    <div class="sheet narrow">
      <div class="sh-head">{title}</div>
      {sub ? <div class="sh-sub mono">{sub}</div> : null}
      <div class="sh-body scroll">
        <div class="menu-list">
          {items.map((it, i) => "heading" in it ? <div class="menu-head">{it.heading}</div> : (
            <button class={it.danger ? "danger" : ""} data-autofocus={it.selected || (i === firstChoice && !anySelected) ? "" : undefined} onClick={() => closeOverlay(it.value)}>
              {it.label}{it.sub ? <small>{it.sub}</small> : null}
            </button>
          ))}
        </div>
      </div>
    </div>
  ), { hints: [["x", t("Choose")], ["o", t("Cancel"), goBack]] });
}
