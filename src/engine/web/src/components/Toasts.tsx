// SPDX-License-Identifier: GPL-3.0-or-later
import { signal } from "@preact/signals";

interface Toast { id: number; text: string; kind: string }
const toasts = signal<Toast[]>([]);
let ids = 0;

// A short message at the bottom left; three at most.
export function toast(text: string, kind?: "ok" | "bad") {
  const item = { id: ++ids, text, kind: kind || "" };
  toasts.value = toasts.value.concat(item).slice(-3);
  setTimeout(() => { toasts.value = toasts.value.filter((x) => x !== item); }, kind === "bad" ? 7000 : 4000);
}

export function Toasts() {
  return <div id="toasts">{toasts.value.map((x) => <div key={x.id} class={"toast " + x.kind}>{x.text}</div>)}</div>;
}
