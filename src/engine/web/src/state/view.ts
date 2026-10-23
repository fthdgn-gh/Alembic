// SPDX-License-Identifier: GPL-3.0-or-later
// What the page shows: the section, the folder in Files, the focused row.
import { signal } from "@preact/signals";
import type { Entry, Listing } from "../api/types";
import { info } from "./engine";
import { IS_PS5 } from "../lib/env";

export const TABS = ["files", "queue", "deps", "settings"] as const;
export type Tab = (typeof TABS)[number];

export const tab = signal<Tab>("files");
export const cwd = signal<string | null>(null);
export const listing = signal<Listing | null>(null);
export const focusEntry = signal<Entry | null>(null); // the focused row in Files
export const pad = signal(IS_PS5); // show controller glyphs (a gamepad was seen)

// Consoles set to confirm with ○ (the engine reads the system setting). The
// browser already sends the keys to match; the page only shows the matching
// glyphs, and reads raw gamepad buttons (physical ✕ and ○) the same way.
export const circleConfirms = () => pad.value && !!info.value && info.value.enter_button === "circle";
