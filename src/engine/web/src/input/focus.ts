// SPDX-License-Identifier: GPL-3.0-or-later
// Where the focus is and where the D-pad moves it: the nearest focusable
// element in the pressed direction, by screen position, inside the open
// dialog or else the page. Lists ([data-column]) keep up and down to
// themselves, and coming back into one lands on its last focused item.
import { overlays, topOverlay } from "../components/Overlay";
import { tab } from "../state/view";

const $$ = (s: string, root?: Element | Document) => Array.prototype.slice.call((root || document).querySelectorAll(s)) as HTMLElement[];

const FOCUSABLE = "button, input, [tabindex]";
const zoneLast: Record<string, HTMLElement | null> = {};
export function setZoneLast(zone: string, el: HTMLElement | null) { zoneLast[zone] = el; }

// Rows whose value changes with ← and → (ChoiceRow).
export const steppers = new WeakMap<Element, (d: number) => void>();

export function scope(): Element {
  const top = topOverlay();
  return top && top.el ? top.el : document.body;
}
export function visible(el: HTMLElement): boolean {
  if ((el as HTMLButtonElement).disabled || el.getAttribute("tabindex") === "-1" || el.hasAttribute("data-nonav")) return false;
  const r = el.getBoundingClientRect();
  return r.width > 0 && r.height > 0 && el.offsetParent !== null;
}
function candidates() { return $$(FOCUSABLE, scope()).filter(visible); }
export function zoneOf(el: Element | null): string {
  const z = el && el.closest && el.closest("[data-zone]");
  return z ? z.getAttribute("data-zone") || "" : "";
}
document.addEventListener("focusin", (e) => {
  const z = zoneOf(e.target as Element);
  if (z) zoneLast[z] = e.target as HTMLElement;
  // Keep focus inside the open overlay.
  if (overlays.value.length && !scope().contains(e.target as Node)) focusFirst();
});

export function focusEl(el: HTMLElement | null | undefined) {
  if (!el) return;
  el.focus({ preventScroll: true });
  ensureVisible(el);
}
function ensureVisible(el: HTMLElement) {
  let p = el.parentElement;
  while (p && p !== document.body) {
    const st = getComputedStyle(p);
    if ((st.overflowY === "auto" || st.overflowY === "scroll") && p.scrollHeight > p.clientHeight) {
      const r = el.getBoundingClientRect(), pr = p.getBoundingClientRect(), pad = 12;
      if (r.top < pr.top + pad) p.scrollTop -= pr.top + pad - r.top;
      else if (r.bottom > pr.bottom - pad) p.scrollTop += r.bottom - pr.bottom + pad;
      return;
    }
    p = p.parentElement;
  }
}
function preferred(): HTMLElement | null {
  const el = scope().querySelector("[data-autofocus]") as HTMLElement | null;
  return el && visible(el) ? el : null;
}
export function focusFirst() {
  const s = scope();
  if (s === document.body) {
    const zone = ({ files: "list", queue: "queue", deps: "deps", settings: "settings" } as Record<string, string>)[tab.value];
    const last = zoneLast[zone];
    if (last && last.isConnected && visible(last)) return focusEl(last);
    const inZone = candidates().filter((c) => zoneOf(c) === zone);
    if (inZone.length) return focusEl(inZone.filter((c) => c.matches(".card.click, .row"))[0] || inZone[0]);
    const a = document.activeElement as HTMLElement | null;
    if (a && a !== document.body && visible(a)) return; // the view is still loading
  }
  const c = preferred() || candidates()[0];
  if (c) focusEl(c);
}

export type Dir = "up" | "down" | "left" | "right";
// The nearest focusable in the pressed direction, by screen position.
export function move(dir: Dir) {
  const cur = document.activeElement as HTMLElement | null;
  if (!cur || cur === document.body || !scope().contains(cur) || !visible(cur)) { focusFirst(); return; }
  const step = steppers.get(cur);
  if ((dir === "left" || dir === "right") && step) { step(dir === "left" ? -1 : 1); return; }
  const strict = !!cur.closest("[data-column]");
  const r = cur.getBoundingClientRect();
  const cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  let best: HTMLElement | null = null, bestScore = Infinity;
  for (const c of candidates()) {
    if (c === cur || c.contains(cur) || cur.contains(c)) continue;
    const q = c.getBoundingClientRect();
    const qx = (q.left + q.right) / 2, qy = (q.top + q.bottom) / 2;
    let main, ortho;
    if (dir === "down") { if (qy <= cy + 1 || q.top < r.top + 2) continue; main = Math.max(0, q.top - r.bottom); ortho = Math.max(0, q.left - r.right, r.left - q.right); }
    else if (dir === "up") { if (qy >= cy - 1 || q.bottom > r.bottom - 2) continue; main = Math.max(0, r.top - q.bottom); ortho = Math.max(0, q.left - r.right, r.left - q.right); }
    else if (dir === "right") { if (q.left < r.right - 8) continue; main = Math.max(0, q.left - r.right); ortho = Math.max(0, q.top - r.bottom, r.top - q.bottom); }
    else { if (q.right > r.left + 8) continue; main = Math.max(0, r.left - q.right); ortho = Math.max(0, q.top - r.bottom, r.top - q.bottom); }
    // Up and down stay in the file browser's columns: the end of a list is the end.
    if ((dir === "up" || dir === "down") && ortho > 0 && strict) continue;
    const align = dir === "up" || dir === "down" ? Math.abs(qx - cx) : Math.abs(qy - cy);
    const score = main + ortho * 4 + align * 0.05;
    if (score < bestScore) { bestScore = score; best = c; }
  }
  if (!best) return;
  // Coming back into a column: the item that had the focus there last.
  const z = zoneOf(best), last = zoneLast[z];
  if (z && z !== zoneOf(cur) && last && last.isConnected && visible(last) && (z === "list" || z === "rail")) best = last;
  focusEl(best);
}
