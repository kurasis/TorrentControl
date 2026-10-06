import { h, replace } from "./dom.js";
import { t } from "./i18n.js";
const positions = new Map();

// Native pages can stop before the requested row count because of their byte
// budget. Follow the returned cursor, including for Previous after short pages.
export function collectionPane(label, fetchPage, renderRow, { id = "model-page", onPage } = {}) {
  const saved = positions.get(id);
  let offset = saved?.offset ?? 0, next = null, total = 0, serial = 0;
  let failedOffset = null;
  const history = saved?.history ?? [];
  function remember(value) {
    positions.delete(id); positions.set(id, { offset: value, history });
    if (positions.size > 32) positions.delete(positions.keys().next().value);
  }
  const rows = h("div", { id: `${id}-rows` });
  const status = h("p", { id: `${id}-status`, role: "status" });
  const error = h("p", { class: "error", role: "alert" });
  const previous = h("button", { type: "button", id: `${id}-previous`, onclick: () => load(history.pop() ?? 0) }, t("previous"));
  const following = h("button", { type: "button", id: `${id}-next`, onclick: () => { history.push(offset); load(next); } }, t("next"));
  const last = h("button", { type: "button", id: `${id}-last`, onclick: () => { history.push(offset); load(Math.max(0, total - 1)); } }, t("lastPage"));
  const retry = h("button", { type: "button", id: `${id}-retry`, onclick: () => load(failedOffset ?? offset) }, t("retry"));
  const first = h("button", { type: "button", id: `${id}-first`, onclick: () => { history.length = 0; load(0); } }, t("firstPage"));
  const root = h("section", { id, class: "model-page", "aria-label": label }, status, rows, error,
    h("div", { class: "button-row" }, first, previous, following, last, retry));
  for (const control of [first, previous, following, last, retry]) control.disabled = true;
  async function load(requested) {
    if (!root.isConnected) return;
    const token = ++serial;
    // Preserve intent even when a validation redraw replaces this pane before
    // the native read replies. A new pane resumes this cursor and history.
    remember(requested);
    for (const control of [first, previous, following, last, retry]) control.disabled = true;
    try {
      const page = await fetchPage(requested, 50);
      if (token !== serial || !root.isConnected) return;
      if (requested >= page.total && page.total > 0) { load(page.total - 1); return; }
      offset = page.offset ?? requested;
      next = page.nextOffset ?? null;
      total = page.total;
      remember(offset);
      replace(rows, page.items.map((item, index) => renderRow(item, offset + index, page)));
      status.textContent = t("collectionPage", { first: total ? offset + 1 : 0, last: offset + page.items.length, total });
      error.textContent = "";
      failedOffset = null;
      onPage?.(page);
    } catch (failure) {
      if (token !== serial || !root.isConnected) return;
      error.textContent = `${failure.code ?? "ERROR"}: ${failure.message}`;
      failedOffset = requested;
    } finally {
      if (token === serial) {
        previous.disabled = !history.length;
        first.disabled = offset === 0;
        following.disabled = next === null;
        last.disabled = total === 0 || offset >= total - 1;
        retry.disabled = false;
      }
    }
  }
  requestAnimationFrame(() => load(offset));
  return root;
}

export function textPane(fetchPage) {
  let offset = 0, next = null, serial = 0;
  const history = [];
  const text = h("pre", { class: "pre model-text", id: "model-text-value", tabindex: "0" });
  const status = h("p", { role: "status" });
  const previous = h("button", { type: "button", id: "model-text-previous", onclick: () => load(history.pop() ?? 0) }, t("previous"));
  const following = h("button", { type: "button", id: "model-text-next", onclick: () => { history.push(offset); load(next); } }, t("next"));
  const root = h("div", {}, text, status, h("div", { class: "button-row" }, previous, following,
    h("button", { type: "button", onclick: () => load(offset) }, t("retry"))));
  async function load(requested) {
    const token = ++serial;
    try {
      const page = await fetchPage(requested);
      if (!root.isConnected || token !== serial) return;
      offset = page.offset; next = page.nextOffset;
      text.textContent = page.text;
      status.textContent = t("textPage", { first: offset, last: next ?? page.totalBytes, total: page.totalBytes });
      previous.disabled = !history.length; following.disabled = next === null;
    } catch (error) { if (root.isConnected && token === serial) status.textContent = `${error.code}: ${error.message}`; }
  }
  requestAnimationFrame(() => load(0));
  return root;
}
