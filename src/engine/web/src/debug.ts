// SPDX-License-Identifier: GPL-3.0-or-later
// For tools/screenshots, which drives the page over DevTools.
import { listing, cwd } from "./state/view";
import { browse } from "./pages/files/browse";
import { showTab } from "./input/history";
import { openConvert } from "./dialogs/ConvertDialog";
import { closeOverlay } from "./components/Overlay";

(window as any).__alembic = {
  state: { get listing() { return listing.value; }, get cwd() { return cwd.value; } },
  browse, showTab, openConvert, closeOverlay,
};
