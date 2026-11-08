// SPDX-License-Identifier: GPL-3.0-or-later
// The Images tab's list (GET /api/images): every image, game folder and
// package in ShadowMount+'s folders, as Files lists them, with each game's
// AMPR emulator.
import { signal } from "@preact/signals";
import type { ImagesList, PeerImages, ImageItem } from "../api/types";
import { api } from "../api/client";
import { tMsg } from "../lib/i18n";
import { toast } from "../components/Toasts";

export const images = signal<ImagesList | null>(null);
export const peerImages = signal<PeerImages[]>([]); // the paired engines' lists

// Every row the tab shows: this engine's, then the paired engines'.
export function allImages(): ImageItem[] {
  const own = images.value ? images.value.images : [];
  return own.concat(...peerImages.value.map((p) => p.images || []));
}

// `refresh`: the engine reads the folders again (after a change).
export async function loadImages(refresh = false) {
  const q = refresh ? "?refresh=1" : "";
  try { images.value = await api<ImagesList>("GET", "/api/images" + q); }
  catch (e: any) { toast(tMsg(e.message), "bad"); }
  // Engines before 0.5.5 have no /api/peers/images: no other engines then.
  try { peerImages.value = await api<PeerImages[]>("GET", "/api/peers/images" + q); } catch (e) { peerImages.value = []; }
}
