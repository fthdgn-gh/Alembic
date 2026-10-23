// SPDX-License-Identifier: GPL-3.0-or-later
import type { RefObject } from "preact";
import { useEffect, useState } from "preact/hooks";
import type { Entry, Meta } from "../api/types";
import { cachedMeta, getMeta, metaKey, metaVersion } from "../api/meta";

// The game info of `e`, asked for once `ref` comes near the screen.
export function useMeta(e: Entry | null, ref?: RefObject<Element>): Meta | null {
  const k = e ? metaKey(e) : "";
  const [m, setM] = useState<Meta | null>(() => (e ? cachedMeta(e) || null : null));
  const version = metaVersion.value;
  useEffect(() => {
    if (!e) { setM(null); return; }
    let live = true;
    const go = () => getMeta(e).then((x) => { if (live) setM(x); });
    const known = cachedMeta(e);
    if (known !== undefined) { setM(known); return; }
    setM(null);
    const el = ref && ref.current;
    if (!el || !("IntersectionObserver" in window)) { go(); return () => { live = false; }; }
    const io = new IntersectionObserver((list) => { if (list.some((x) => x.isIntersecting)) { io.disconnect(); go(); } }, { rootMargin: "300px" });
    io.observe(el);
    return () => { live = false; io.disconnect(); };
  }, [k, version]);
  return m;
}
