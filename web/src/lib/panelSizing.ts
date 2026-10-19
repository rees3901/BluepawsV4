export const PANEL_BASE_WIDTH = 460;
export const PANEL_SHRINK_DELAY_MS = 1500;
export const PANEL_SHRINK_TOLERANCE = 12;

export function panelResizeDirection(current: number, required: number) {
  return required > current ? "grow" : current - required > PANEL_SHRINK_TOLERANCE ? "shrink" : "hold";
}

// Measure intrinsic content independently of the panel's current width. Using
// stretched row widths here would prevent the panel ever shrinking again.
export function measurePanelContentWidth(panel: HTMLElement) {
  let required = PANEL_BASE_WIDTH;
  const host = document.createElement("div");
  host.className = "device-card";
  host.setAttribute("aria-hidden", "true");
  host.inert = true;
  host.style.cssText = "position:fixed;left:-100000px;top:0;width:max-content;max-width:none;visibility:hidden;pointer-events:none;overflow:visible;";
  document.body.append(host);
  const number = (value: string) => parseFloat(value) || 0;
  const intrinsic = (element: HTMLElement) => {
    const clone = element.cloneNode(true) as HTMLElement;
    clone.removeAttribute("id");
    clone.querySelectorAll("[id]").forEach(child => child.removeAttribute("id"));
    clone.querySelectorAll(".gnss-detail").forEach(child => child.remove());
    clone.style.cssText += ";width:max-content;min-width:0;max-width:none;flex-wrap:nowrap;white-space:nowrap;overflow:visible;";
    clone.style.font = getComputedStyle(element).font;
    if (clone.classList.contains("card-grid")) clone.style.gridTemplateColumns = "max-content max-content";
    host.append(clone);
    const width = clone.getBoundingClientRect().width;
    clone.remove();
    return width;
  };
  try {
    panel.querySelectorAll<HTMLElement>(".card-summary").forEach(summary => {
      if (!summary.getClientRects().length) return;
      const style = getComputedStyle(summary);
      const siblings = Array.from(summary.children).filter(child => !child.classList.contains("card-identity")) as HTMLElement[];
      const chrome = siblings.reduce((sum, item) => {
        const itemStyle = getComputedStyle(item);
        return sum + Math.max(item.offsetWidth, item.classList.contains("card-avatar-wrap") ? 78 : 0)
          + number(itemStyle.marginLeft) + number(itemStyle.marginRight);
      }, 0) + siblings.length * number(style.columnGap) + number(style.paddingLeft) + number(style.paddingRight)
        + panel.offsetWidth - summary.offsetWidth;
      // Badge groups may flow between rows, but never truncate a name or split
      // the telemetry/age line just because the desktop panel started narrow.
      summary.querySelectorAll<HTMLElement>(".card-indicators, .card-name, .card-fault-row").forEach(row => {
        required = Math.max(required, Math.ceil(intrinsic(row) + chrome + 4));
      });
    });
    panel.querySelectorAll<HTMLElement>(".device-card.expanded .card-grid").forEach(grid => {
      if (grid.getClientRects().length) required = Math.max(required, Math.ceil(intrinsic(grid) + panel.offsetWidth - grid.offsetWidth + 4));
    });
  } finally { host.remove(); }
  return required;
}

export function observePanelSizing(panel: HTMLElement, update: (width: number) => void) {
  let width = PANEL_BASE_WIDTH, pending = width, frame = 0;
  let shrink: ReturnType<typeof setTimeout> | undefined;
  const apply = (next: number) => { width = next; update(next); };
  const measure = () => {
    if (window.innerWidth <= 768) return;
    pending = measurePanelContentWidth(panel);
    const direction = panelResizeDirection(width, pending);
    if (direction === "grow") {
      clearTimeout(shrink); shrink = undefined; apply(pending);
    } else if (direction === "shrink") {
      // Do not restart on every one-second clock tick. Shrink to the latest
      // target once settled; growth always wins immediately.
      shrink ??= setTimeout(() => { shrink = undefined; if (width - pending > PANEL_SHRINK_TOLERANCE) apply(pending); }, PANEL_SHRINK_DELAY_MS);
    } else { clearTimeout(shrink); shrink = undefined; }
  };
  const schedule = () => { cancelAnimationFrame(frame); frame = requestAnimationFrame(measure); };
  const resize = new ResizeObserver(schedule);
  resize.observe(panel);
  const mutations = new MutationObserver(schedule);
  mutations.observe(panel, { subtree: true, childList: true, characterData: true, attributes: true, attributeFilter: ["class", "open", "hidden", "aria-hidden"] });
  window.addEventListener("resize", schedule);
  document.fonts?.addEventListener("loadingdone", schedule);
  apply(width); measure();
  return () => {
    resize.disconnect(); mutations.disconnect(); cancelAnimationFrame(frame); clearTimeout(shrink);
    window.removeEventListener("resize", schedule);
    document.fonts?.removeEventListener("loadingdone", schedule);
  };
}
