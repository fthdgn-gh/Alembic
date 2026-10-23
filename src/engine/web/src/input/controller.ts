// SPDX-License-Identifier: GPL-3.0-or-later
// The controller: D-pad (arrow keys), ✕ (Enter or a click), ○ (the browser's
// Back), □, L1/R1, L2/R2, as the PS5 browser sends them, from a gamepad, or
// from a keyboard and mouse on a PC. Everything ends in press(action).
import { overlays, topOverlay } from "../components/Overlay";
import { tab, focusEntry, cwd, pad, circleConfirms } from "../state/view";
import { openActions } from "../pages/files/actions";
import { IS_PS5 } from "../lib/env";
import { move, focusEl, focusFirst, zoneOf, type Dir } from "./focus";
import { goBack, leaveAll, depth, cycleTab } from "./history";


// L2 or R2 alone pages up or down; on the PS5 both together leave (leaveAll). A press
// waits a moment for the other trigger, so a chord never pages first. Held
// triggers arrive as streams of presses: the chord stays on while either
// keeps coming, and a held single trigger pages about every 120 ms.
const chord = { l2: 0, r2: 0, pending: { l2: 0, r2: 0 } as Record<string, number>, until: 0 };
function trigger(side: "l2" | "r2"): void {
  if (!IS_PS5) return press(side === "l2" ? "page-up" : "page-down"); // a PC: no chord, no wait
  const now = Date.now(), other = side === "l2" ? "r2" : "l2";
  chord[side] = now;
  if (now < chord.until) { chord.until = now + 400; return; }
  if (now - chord[other] < 250) {
    clearTimeout(chord.pending.l2); clearTimeout(chord.pending.r2); chord.pending.l2 = chord.pending.r2 = 0;
    chord.until = now + 400;
    leaveAll();
    return;
  }
  if (chord.pending[side]) return;
  chord.pending[side] = window.setTimeout(() => { chord.pending[side] = 0; press(side === "l2" ? "page-up" : "page-down"); }, 120);
}

// Actions from any input: keyboard, the PS5 browser's own mapping, a gamepad,
// or a click on a hint.
export function press(action: string): void {
  if (action === "up" || action === "down" || action === "left" || action === "right") return move(action);
  if (action === "ok") { const a = document.activeElement as HTMLElement | null; if (a && a !== document.body && !a.closest(".hints")) a.click(); else focusFirst(); return; }
  if (action === "back") return goBack();
  if (action === "l2" || action === "r2") return trigger(action);
  if (action === "page-up" || action === "page-down") { // inside the same list
    for (let i = 0; i < 8; i++) {
      const before = document.activeElement as HTMLElement, z = zoneOf(before);
      move(action === "page-up" ? "up" : "down");
      if (document.activeElement === before) break;
      if (zoneOf(document.activeElement) !== z) { focusEl(before); break; }
    }
    return;
  }
  if (action === "l1") { if (!overlays.value.length) cycleTab(-1); return; }
  if (action === "r1") { if (!overlays.value.length) cycleTab(1); return; }
  const top = topOverlay();
  // □ is the page's second action: the browser keeps △, Options, L3 and R3 for itself.
  if (action === "s") {
    if (top) { if (top.onSquare) top.onSquare(); return; }
    const z = zoneOf(document.activeElement);
    if (tab.value === "files" && focusEntry.value && cwd.value !== "/" && z === "list") openActions(focusEntry.value);
  }
}

export const lastNative: Record<string, number> = {};
// The PS5 browser's keys the page uses. △ (F1), Options (F3), L3 (F9) and R3
// (F10) belong to the browser: never mapped, never stopped.
const PS_KEYS: Record<string, string> = { F2: "s", F5: "l1", F6: "r1", F7: "l2", F8: "r2" };
const BROWSER_KEYS: Record<string, boolean> = { F1: true, F3: true, F9: true, F10: true };

// What the browser sends, for Settings → Controller test.
export const inputLog = { items: [] as { t: number; text: string }[], test: null as null | (() => void), closing: false };
export function logInput(text: string) {
  inputLog.items.unshift({ t: Date.now(), text });
  inputLog.items.length = Math.min(inputLog.items.length, 40);
  if (inputLog.test) inputLog.test();
}

// The PS5 browser turns ✕ into a mouse click at its cursor, wherever that is.
// While the D-pad is in use, a click acts on the focused item instead; moving
// the cursor (or a mouse) hands clicks back to the pointer.
const pointer = { x: -1, y: -1, mode: false };
function useController() { if (!pointer.mode) { pointer.mode = true; document.body.classList.add("pad"); } }
document.addEventListener("mousemove", (e) => {
  const moved = Math.abs(e.clientX - pointer.x) + Math.abs(e.clientY - pointer.y) > 4;
  pointer.x = e.clientX; pointer.y = e.clientY;
  if (moved && pointer.mode) { pointer.mode = false; document.body.classList.remove("pad"); logInput("pointer moved: clicks follow the cursor"); }
}, true);
// The press before that click would move the focus to whatever is under the
// cursor; keep it where the D-pad put it.
document.addEventListener("mousedown", (e) => {
  if (e.isTrusted && (pointer.mode || inputLog.test)) {
    const a = document.activeElement;
    if (!a || a === document.body || !a.contains(e.target as Node)) e.preventDefault();
  }
}, true);

const backHold = { lastAt: 0 };
document.addEventListener("keydown", (e) => {
  const target = e.target as HTMLElement | null, typing = !!target && target.tagName === "INPUT" && (target as HTMLInputElement).type !== "checkbox";
  const k = e.key, code = e.keyCode;
  logInput("key " + JSON.stringify(k) + " code=" + (e.code || "-") + " keyCode=" + code + (e.shiftKey ? " +shift" : "") + (e.repeat ? " (repeat)" : ""));
  let a: string | null = null;
  if (k === "ArrowUp" || code === 38) a = "up";
  else if (k === "ArrowDown" || code === 40) a = "down";
  else if (k === "ArrowLeft" || code === 37) a = typing ? null : "left";
  else if (k === "ArrowRight" || code === 39) a = typing ? null : "right";
  else if (k === "Escape" || code === 27) a = "back";
  else if ((k === "Backspace" || code === 8) && !typing) a = "back";
  else if ((k === "Enter" || code === 13) && !typing && target && target.tagName !== "BUTTON") a = "ok";
  // The PS5 browser sends the other buttons as function keys (owner's
  // Controller test): △ F1, □ F2, Options F3, L1 F5, R1 F6, L2 F7, R2 F8,
  // L3 F9, R3 F10. ○ is Escape, ✕ a click at the cursor. The browser also acts
  // on △, Options, L3 and R3 itself, so the page uses only ✕ ○ □, the D-pad,
  // L1/R1 and L2/R2.
  else if (PS_KEYS[k]) a = PS_KEYS[k];
  else if (!typing && (k === "q" || k === "Q")) a = "l1";
  else if (!typing && (k === "e" || k === "E")) a = "r1";
  else if (!typing && (k === "s" || k === "S" || k === "a" || k === "A" || k === "ContextMenu")) a = "s";
  // The function keys the page uses have browser meanings too (F5 reloads,
  // F6 jumps to the address bar): those must not reach the browser.
  if (PS_KEYS[k]) { e.preventDefault(); useController(); }
  if (BROWSER_KEYS[k]) { useController(); return; }
  if (k === "Enter" || code === 13) lastNative.ok = Date.now();
  if (inputLog.test && !((k === "Enter" || code === 13) && target && target.hasAttribute && target.hasAttribute("data-test-done"))) { e.preventDefault(); return; }
  if (!a) return;
  if (a === "back" && (k === "Escape" || code === 27)) {
    // A held ○ arrives as a stream of Escape presses, without e.repeat on the
    // PS5 (owner's Controller test): presses less than 250 ms apart are one
    // hold, which is one step back (leaving is L2 + R2).
    const now = Date.now(), gap = now - backHold.lastAt;
    backHold.lastAt = now;
    if (e.repeat || gap < 250) { e.preventDefault(); return; }
  }
  if (a === "up" || a === "down" || a === "left" || a === "right") useController();
  lastNative[a] = Date.now();
  if (a === "back" && !overlays.value.length && !(depth() > 0)) return; // nothing to go back to: let it be
  e.preventDefault();
  if (typing && a === "back") (target as HTMLElement).blur();
  press(a);
}, true);
document.addEventListener("keyup", (e) => {
  if (e.key === "Escape" || e.keyCode === 27) logInput("key up \"Escape\"");
}, true);
document.addEventListener("click", (e) => {
  lastNative.ok = Date.now();
  if (!e.isTrusted) return; // our own a.click()
  const tg = e.target as HTMLElement;
  logInput("click at " + e.clientX + "," + e.clientY + " on <" + tg.tagName.toLowerCase() + (tg.className ? "." + String(tg.className).split(" ")[0] : "") + ">" + (pointer.mode ? " → focused item" : ""));
  const a = document.activeElement as HTMLElement | null;
  if (inputLog.test) {
    const d = tg.closest && tg.closest("[data-test-done]");
    if (!d && !(pointer.mode && a && a.hasAttribute("data-test-done"))) { e.preventDefault(); e.stopPropagation(); return; }
  }
  if (!pointer.mode) return;
  if (!a || a === document.body || a.contains(tg)) return; // already the focused item
  e.preventDefault(); e.stopPropagation();
  if (a.tagName === "INPUT") { a.focus(); return; }
  a.click();
}, true);

// Gamepad API, where the browser has it. Buttons the browser already turns
// into keys (D-pad, ✕, ○) are only used when no key arrived for them.
const PAD: Record<number, string> = { 0: "ok", 1: "back", 2: "s", 3: "t", 4: "l1", 5: "r1", 6: "l2", 7: "r2", 9: "options", 12: "up", 13: "down", 14: "left", 15: "right" };
const padState = { held: {} as Record<string, { t?: number; next: number }>, raw: {} as Record<number, boolean>, timer: 0 };
const swapAction = (a: string) => (circleConfirms() && (a === "ok" || a === "back") ? (a === "ok" ? "back" : "ok") : a);
function padLoop() {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  let any = false;
  for (const p of Array.prototype.slice.call(pads) as (Gamepad | null)[]) {
    if (!p) continue;
    any = true;
    p.buttons.forEach((b, i) => {
      const down = !!(b && (b.pressed || b.value > 0.5));
      if (down && !padState.raw[i]) logInput("gamepad button " + i + (PAD[i] ? " (" + PAD[i] + ")" : "") + " · " + (p.mapping || "no mapping") + " · " + p.id.slice(0, 40));
      padState.raw[i] = down;
    });
    if (inputLog.test) continue;
    for (const key in PAD) {
      const i = Number(key), b = p.buttons[i], down = !!(b && (b.pressed || b.value > 0.5)), was = padState.held[i];
      const now = Date.now();
      if (down && !was) { padState.held[i] = { t: now, next: now + 380 }; padPress(swapAction(PAD[i]), now); }
      else if (down && was && i >= 12 && now >= was.next) { was.next = now + 110; padPress(PAD[i], now); }
      else if (!down && was) delete padState.held[i];
    }
    const ax = p.axes || [];
    const dirs: [Dir, boolean][] = [["left", ax[0] < -0.6], ["right", ax[0] > 0.6], ["up", ax[1] < -0.6], ["down", ax[1] > 0.6]];
    for (const d of dirs) {
      const key = "ax" + d[0], was = padState.held[key], now = Date.now();
      if (d[1] && !was) { padState.held[key] = { next: now + 380 }; press(d[0]); }
      else if (d[1] && was && now >= was.next) { was.next = now + 140; press(d[0]); }
      else if (!d[1] && was) delete padState.held[key];
    }
  }
  if (any && !pad.value) pad.value = true;
  padState.timer = any ? requestAnimationFrame(padLoop) : 0;
}
function padPress(action: string, at: number) {
  if (action === "up" || action === "down" || action === "left" || action === "right") useController();
  // The PS5 browser already sends keys for these; act only when none came.
  setTimeout(() => { if (!(lastNative[action] >= at - 60)) press(action); }, 90);
}
window.addEventListener("gamepadconnected", () => { if (!padState.timer) padState.timer = requestAnimationFrame(padLoop); });
if (navigator.getGamepads) setTimeout(() => {
  for (const x of Array.prototype.slice.call(navigator.getGamepads())) if (x) { padState.timer = requestAnimationFrame(padLoop); break; }
}, 500);
