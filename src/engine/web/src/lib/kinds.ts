// SPDX-License-Identifier: GPL-3.0-or-later
// What kinds of entries there are, and what can be converted.
import type { Entry } from "../api/types";

export const CONVERTIBLE = ["fpkg", "exfat", "ffpfsc", "ffpkg", "ffpfs"];
export const KIND_LABEL: Record<string, string> = { fpkg: "PKG", exfat: "EXFAT", ffpfsc: "FFPFSC", ffpkg: "FFPKG", ffpfs: "FFPFS", folder: "FOLDER", file: "FILE" };
export const isConvertible = (e: Entry) => CONVERTIBLE.indexOf(e.kind) >= 0 || !!e.game;

// ShadowMount+ skips names that start with "." and keeps one source per
// game while that source exists: hiding the old one makes it switch.
export const isHiddenName = (name: string) => name.length > 1 && name[0] === ".";
