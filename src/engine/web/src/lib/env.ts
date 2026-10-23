// SPDX-License-Identifier: GPL-3.0-or-later
// Where the page runs.

// The PS5's own browser: controller glyphs, L2 + R2 leaves.
export const IS_PS5 = /PlayStation/i.test(navigator.userAgent);
// A browser on the engine's own computer: the only one shown a pairing code.
export const LOCAL_PAGE = /^(127\.0\.0\.1|localhost)$/.test(location.hostname);
