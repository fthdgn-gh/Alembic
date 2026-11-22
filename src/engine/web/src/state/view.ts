// SPDX-License-Identifier: GPL-3.0-or-later
// What the page shows: the section, the folder in Files, the focused row.
import { signal } from "@preact/signals";
import type { Entry, Listing } from "../api/types";
import { info } from "./engine";
import { IS_PS5 } from "../lib/env";
import { store } from "../lib/storage";

export const TABS = ["images", "files", "queue", "deps", "settings"] as const;
export type Tab = (typeof TABS)[number];

// The tab the page starts on (and leaving returns to): Images on a PS5
// engine, its library; Files on a PC's, where Images mostly holds Downloads.
// Before the engine answers, the console's browser is taken for a PS5's.
export function homeTab(): Tab {
  return info.value ? (info.value.platform === "ps5" ? "images" : "files") : IS_PS5 ? "images" : "files";
}
export const tab = signal<Tab>(homeTab());

// How Files and Images show their entries (the switch above each): a list,
// or a grid of tiles. Kept per browser; Files starts as a list, Images as a grid.
export type ViewMode = "list" | "grid";
export const viewModes = signal<{ files: ViewMode; images: ViewMode }>({
  files: store.get<ViewMode>("view.files") === "grid" ? "grid" : "list",
  images: store.get<ViewMode>("view.images") === "list" ? "list" : "grid",
});
export function setViewMode(which: "files" | "images", mode: ViewMode) {
  viewModes.value = { ...viewModes.value, [which]: mode };
  store.set("view." + which, mode);
}
// The folder Files shows: TOP is the list of drives ("": "/" can be a drive
// itself, "File system root", with Settings > Whole file system); null before
// the first listing.
export const TOP = "";
export const cwd = signal<string | null>(null);
export const listing = signal<Listing | null>(null);
export const focusEntry = signal<Entry | null>(null); // the focused row in Files

// Which entries Files shows (issue #6), kept per browser. Plain folders stay
// in every filter, to move around; the drives list is never filtered.
export type FileFilter = "all" | "packages" | "images" | "games" | "other";
export const FILE_FILTERS: FileFilter[] = ["all", "packages", "images", "games", "other"];
const IMAGE_KINDS = ["exfat", "ffpfsc", "ffpkg", "ffpfs"];
export const fileFilter = signal<FileFilter>(FILE_FILTERS.indexOf(store.get<FileFilter>("filter.files")!) >= 0 ? store.get<FileFilter>("filter.files")! : "all");
export function setFileFilter(f: FileFilter) {
  fileFilter.value = f;
  store.set("filter.files", f);
}
export const isPlainFolder = (e: Entry) => e.kind === "folder" && !e.game;
export function matchesFilter(e: Entry, f: FileFilter = fileFilter.value) {
  return f === "all" ? true : f === "packages" ? e.kind === "fpkg" : f === "images" ? IMAGE_KINDS.indexOf(e.kind) >= 0
    : f === "games" ? !!e.game : !e.game && e.kind !== "folder" && e.kind !== "fpkg" && IMAGE_KINDS.indexOf(e.kind) < 0;
}
// The rows Files shows for a listing.
export function shownEntries(data: Listing | null): Entry[] {
  if (!data) return [];
  if (data.path === TOP || fileFilter.value === "all") return data.entries;
  return data.entries.filter((e) => isPlainFolder(e) || matchesFilter(e));
}
export const pad = signal(IS_PS5); // show controller glyphs (a gamepad was seen)

// Consoles set to confirm with ○ (the engine reads the system setting). The
// browser already sends the keys to match; the page only shows the matching
// glyphs, and reads raw gamepad buttons (physical ✕ and ○) the same way.
export const circleConfirms = () => pad.value && !!info.value && info.value.enter_button === "circle";
