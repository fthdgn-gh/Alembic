// SPDX-License-Identifier: GPL-3.0-or-later
// The Images tab's list (GET /api/images): every image, game folder and
// package in ShadowMount+'s folders, as Files lists them, with each game's
// AMPR emulator.
import { signal } from "@preact/signals";
import type { ImagesList } from "../api/types";
import { api } from "../api/client";
import { tMsg } from "../lib/i18n";
import { toast } from "../components/Toasts";

export const images = signal<ImagesList | null>(null);

// `refresh`: the engine reads the folders again (after a change).
export async function loadImages(refresh = false) {
  try { images.value = await api<ImagesList>("GET", "/api/images" + (refresh ? "?refresh=1" : "")); }
  catch (e: any) { toast(tMsg(e.message), "bad"); }
}
