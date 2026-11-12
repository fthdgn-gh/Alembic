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
export const pad = signal(IS_PS5); // show controller glyphs (a gamepad was seen)

// Consoles set to confirm with ○ (the engine reads the system setting). The
// browser already sends the keys to match; the page only shows the matching
// glyphs, and reads raw gamepad buttons (physical ✕ and ○) the same way.
export const circleConfirms = () => pad.value && !!info.value && info.value.enter_button === "circle";
