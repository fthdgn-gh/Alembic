// SPDX-License-Identifier: GPL-3.0-or-later
// For tools/screenshots, which drives the page over DevTools.
import { listing, cwd } from "./state";
import { browse } from "./files";
import { showTab } from "./app";
import { openConvert } from "./convert";
import { closeOverlay } from "./overlay";

(window as any).__alembic = {
  state: { get listing() { return listing.value; }, get cwd() { return cwd.value; } },
  browse, showTab, openConvert, closeOverlay,
};
