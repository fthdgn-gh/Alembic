// SPDX-License-Identifier: GPL-3.0-or-later
// Dialogs: sheets over the page. Each has its own history entry, so ○ (the
// browser's Back) closes it; closeOverlay(value) goes back, and the entry's
// popstate resolves the promise openOverlay returned.
import type { ComponentChildren, VNode } from "preact";
import { useEffect } from "preact/hooks";
import { signal } from "@preact/signals";
import { focusEl, focusFirst } from "../input/focus";
import { pushNav } from "../input/history";
import { tab, cwd } from "../state/view";

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
