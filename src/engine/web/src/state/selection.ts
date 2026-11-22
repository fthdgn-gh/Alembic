// SPDX-License-Identifier: GPL-3.0-or-later
// Selecting several entries in Files or Images (□ > Select): while
// selecting, ✕ ticks a row, □ opens the actions for every ticked one and ○
// ends it. A selection belongs to one folder (or the Images list): opening
// another ends it.
import { signal, computed } from "@preact/signals";
import type { Entry } from "../api/types";
import { cwd, listing, tab, fileFilter, shownEntries, isPlainFolder } from "./view";
import { allImages } from "./images";

// The rows the selection is made from: the folder's shown ones in Files, the list's in Images.
const rows = (): Entry[] => (tab.value === "images" ? allImages() : shownEntries(listing.value));
// What Select all ticks: the same, less a filter's plain folders (shown only to move around).
const allRows = (): Entry[] => rows().filter((e) => tab.value === "images" || fileFilter.value === "all" || !isPlainFolder(e));

export const selecting = signal(false);
export const selected = signal<string[]>([]); // paths, in the order they were ticked

// The ticked entries, in list order.
export const selectedEntries = computed<Entry[]>(() => {
  const s = selected.value;
  return rows().filter((e) => s.indexOf(e.path) >= 0);
});

export function isSelected(e: Entry) { return selected.value.indexOf(e.path) >= 0; }

export function toggleSelected(e: Entry) {
  const s = selected.value;
  selected.value = s.indexOf(e.path) >= 0 ? s.filter((p) => p !== e.path) : s.concat([e.path]);
}
export function selectAll() { selected.value = allRows().map((e) => e.path); }
export function selectableCount() { return rows().length; }
// Select all would tick more than is ticked now.
export function canSelectAll() { const s = selected.value; return allRows().some((e) => s.indexOf(e.path) < 0); }
export function clearSelected() { selected.value = []; }

// Starts selecting with `e` ticked. ○ is the browser's Back, so selecting
// gets a history entry of its own: going back ends it (history.ts).
export function startSelecting(e: Entry | null) {
  selected.value = e ? [e.path] : [];
  if (selecting.value) return;
  selecting.value = true;
  history.pushState({ view: tab.value, path: cwd.value, select: true, depth: ((history.state && history.state.depth) || 0) + 1 }, "");
}
// Ends it (after a bulk action, say). Its history entry, now stale, is
// stepped over: here when it is the current one, else when Back reaches it.
export function stopSelecting() {
  if (!selecting.value) return;
  endSelecting();
  if (history.state && history.state.select) history.back();
}
export function endSelecting() {
  selecting.value = false;
  selected.value = [];
}
