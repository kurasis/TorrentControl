// Minimal DOM builder. Children that are strings become text nodes, so data
// from torrents, file names or the native side is never parsed as HTML.
export function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs ?? {})) {
    if (value === undefined || value === null || value === false) continue;
    if (key === "class") el.className = value;
    else if (key === "text") el.textContent = value;
    else if (key === "dataset") Object.assign(el.dataset, value);
    else if (key.startsWith("on") && typeof value === "function") el.addEventListener(key.slice(2), value);
    else if (key === "value") el.value = value;
    else if (key === "checked") el.checked = Boolean(value);
    else if (value === true) el.setAttribute(key, "");
    else el.setAttribute(key, String(value));
  }
  append(el, children);
  return el;
}

function append(el, children) {
  for (const child of children.flat(Infinity)) {
    if (child === null || child === undefined || child === false) continue;
    el.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
}

export function clear(el) {
  while (el.firstChild) el.firstChild.remove();
  return el;
}

export function replace(el, ...children) {
  clear(el);
  append(el, children);
  return el;
}

export function debounce(fn, ms) {
  let timer = null;
  const wrapped = (...args) => {
    clearTimeout(timer);
    timer = setTimeout(() => fn(...args), ms);
  };
  wrapped.flush = (...args) => {
    clearTimeout(timer);
    fn(...args);
  };
  return wrapped;
}

let toastTimer = null;
export function toast(message, { action, onAction, error = false } = {}) {
  const host = document.getElementById("toast");
  if (!host) return;
  replace(
    host,
    h("span", { text: message }),
    action ? h("button", { type: "button", class: "link", onclick: () => { host.hidden = true; onAction?.(); } }, action) : null,
  );
  host.className = error ? "toast error" : "toast";
  host.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { host.hidden = true; }, action ? 10000 : 5000);
}
