// Virtualized list (specification section 4.1, U02). Only the rows in view
// (plus a small margin) exist in the DOM, whatever the total; rows are
// fetched in pages from the native side and cached.
const PAGE = 250;
const MAX_CACHED_PAGES = 8;
const MAX_IN_FLIGHT = 4;

export class VirtualList {
  // fetchPage(offset, limit) -> Promise<{ total, items }>
  // renderRow(item, index) -> HTMLElement (the list positions it)
  constructor(container, { rowHeight = 28, fetchPage, renderRow, renderPlaceholder, label }) {
    this.container = container;
    this.rowHeight = rowHeight;
    this.fetchPage = fetchPage;
    this.renderRow = renderRow;
    this.renderPlaceholder = renderPlaceholder;
    this.total = 0;
    this.pages = new Map();
    this.loading = new Set();
    this.generation = 0;

    container.classList.add("vlist");
    container.setAttribute("role", "list");
    if (label) container.setAttribute("aria-label", label);
    container.tabIndex = 0;
    this.spacer = document.createElement("div");
    this.spacer.className = "vlist-spacer";
    container.append(this.spacer);
    this.rows = new Map();
    container.addEventListener("scroll", () => this.render(), { passive: true });
    container.addEventListener("keydown", (e) => this.onKey(e));
    this.resizeObserver = new ResizeObserver(() => this.render());
    this.resizeObserver.observe(container);
  }

  // Drops cached pages (new data source or filter) and reloads.
  reset() {
    this.generation += 1;
    this.pages.clear();
    this.loading.clear();
    for (const row of this.rows.values()) row.remove();
    this.rows.clear();
    this.total = 0;
    this.container.scrollTop = 0;
    return this.load(0);
  }

  async load(page) {
    if (this.pages.has(page) || this.loading.has(page) || this.loading.size >= MAX_IN_FLIGHT) return;
    const generation = this.generation;
    this.loading.add(page);
    try {
      const result = await this.fetchPage(page * PAGE, PAGE);
      if (generation !== this.generation) return; // stale
      this.total = Number(result.total ?? 0);
      this.pages.set(page, result.items ?? []);
      while (this.pages.size > MAX_CACHED_PAGES) this.pages.delete(this.pages.keys().next().value);
      for (const [index, row] of this.rows) {
        if (Math.floor(index / PAGE) === page) {
          row.remove();
          this.rows.delete(index);
        }
      }
    } catch {
      // Leave the page unloaded; scrolling retries it.
    } finally {
      if (generation === this.generation) this.loading.delete(page);
    }
    this.render();
  }

  item(index) {
    const key = Math.floor(index / PAGE);
    const page = this.pages.get(key);
    if (page) { this.pages.delete(key); this.pages.set(key, page); }
    return page ? page[index % PAGE] : undefined;
  }

  render() {
    if (!this.container.isConnected) {
      this.resizeObserver.disconnect();
      this.pages.clear();
      return;
    }
    this.spacer.style.height = `${this.total * this.rowHeight}px`;
    const height = this.container.clientHeight || 400;
    const first = Math.max(0, Math.floor(this.container.scrollTop / this.rowHeight) - 10);
    const last = Math.min(this.total, Math.ceil((this.container.scrollTop + height) / this.rowHeight) + 10);

    for (const [index, row] of this.rows) {
      if (index < first || index >= last) {
        row.remove();
        this.rows.delete(index);
      }
    }
    for (let i = first; i < last; i++) {
      if (this.rows.has(i)) continue;
      const item = this.item(i);
      let row;
      if (item === undefined) {
        this.load(Math.floor(i / PAGE));
        row = this.renderPlaceholder ? this.renderPlaceholder(i) : document.createElement("div");
        row.dataset.placeholder = "1";
      } else {
        row = this.renderRow(item, i);
      }
      row.classList.add("vlist-row");
      row.setAttribute("role", "listitem");
      row.style.top = `${i * this.rowHeight}px`;
      row.style.height = `${this.rowHeight}px`;
      row.dataset.index = String(i);
      this.container.append(row);
      this.rows.set(i, row);
    }
  }

  onKey(e) {
    const page = Math.max(1, Math.floor(this.container.clientHeight / this.rowHeight) - 1);
    const moves = { ArrowDown: 1, ArrowUp: -1, PageDown: page, PageUp: -page };
    if (e.key in moves) {
      this.container.scrollTop += moves[e.key] * this.rowHeight;
      e.preventDefault();
    } else if (e.key === "Home") {
      this.container.scrollTop = 0;
      e.preventDefault();
    } else if (e.key === "End") {
      this.container.scrollTop = this.total * this.rowHeight;
      e.preventDefault();
    }
  }
}
