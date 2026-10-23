// SPDX-License-Identifier: GPL-3.0-or-later
// History: ○ is the browser's Back on the PS5, so sections, folders and
// dialogs each get a history entry, and going back is history.back().
import { overlays, popOverlay } from "../components/Overlay";
import { toast } from "../components/Toasts";
import { tab, cwd, circleConfirms, TABS, type Tab } from "../state/view";
import { browse } from "../pages/files/browse";
import { t } from "../lib/i18n";
import { focusFirst } from "./focus";
import { inputLog, lastNative, logInput } from "./controller";

interface NavState { view?: string; path?: string | null; from?: string | null; overlay?: boolean; depth?: number }
export function depth(): number { return history.state && history.state.depth ? history.state.depth : 0; }
export function pushNav(s: NavState) { s.depth = depth() + 1; history.pushState(s, ""); }
export function goBack() {
  if (overlays.value.length || depth() > 0) { lastNative.back = Date.now(); history.back(); }
}

// Leaving (L2 + R2): everything open is closed and the page goes back to
// its first entry, where one more Back leaves the browser (a page cannot
// close it). One step at a time, checking each: on the console a jump of
// several steps (history.go(-n)) went back only one, and the next ○ then
// went back a page instead of leaving.
const hold = { unwinding: false, steps: 0, timer: 0 };
export function leaveAll() {
  if (inputLog.test || hold.unwinding) return; // the controller test listens to every button
  const open = overlays.value;
  overlays.value = [];
  for (const o of open) o.resolve(null);
  if (depth() > 0) { hold.unwinding = true; hold.steps = 0; stepBack(); } else afterLeave();
}
function stepBack() {
  hold.steps++;
  // A browser that stops answering: say how to leave anyway.
  clearTimeout(hold.timer);
  hold.timer = window.setTimeout(() => { if (hold.unwinding) { hold.unwinding = false; afterLeave(); } }, 1500);
  history.back();
}
function afterLeave() {
  if (tab.value !== "files") showTab("files", false);
  if (cwd.value !== "/") browse("/", false);
  toast(t("Press {b} once more to leave Alembic", { b: circleConfirms() ? "✕" : "○" }), "ok");
  try { window.close(); } catch (e) { /* works only where the browser allows it */ }
}
window.addEventListener("popstate", (e) => {
  if (hold.unwinding) {
    if (depth() > 0 && hold.steps < 100) { stepBack(); return; }
    clearTimeout(hold.timer);
    hold.unwinding = false;
    afterLeave();
    return;
  }
  lastNative.back = Date.now();
  logInput("history back/forward (popstate)");
  if (inputLog.test && !inputLog.closing) { history.pushState({ overlay: true, depth: depth() + 1 }, ""); return; }
  inputLog.closing = false;
  if (overlays.value.length) { popOverlay(); return; }
  const s: NavState = e.state || { view: "files", path: "/" };
  if (s.overlay) { history.back(); return; } // a stale overlay entry
  showTab((s.view || "files") as any, false);
  if ((s.view || "files") === "files" && s.path && s.path !== cwd.value) browse(s.path, false);
});

export function showTab(next: Tab, push?: boolean) {
  if (push !== false && next !== tab.value) pushNav({ view: next, path: cwd.value });
  tab.value = next;
  if (!overlays.value.length) setTimeout(focusFirst, 0);
}
export function cycleTab(d: number) { showTab(TABS[(TABS.indexOf(tab.value) + d + TABS.length) % TABS.length]); }
