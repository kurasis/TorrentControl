// CommandBridge client: versioned JSON request/response over WebView2 web
// messages, plus native events. Responses are matched by requestId; events
// carry a monotonic sequence number. Anything else is ignored.
const PROTOCOL_VERSION = 1;
const pending = new Map();
const listeners = new Set();
let nextId = 1;
let lastSequence = 0n;

const webview = window.chrome?.webview;

if (webview) {
  webview.addEventListener("message", (event) => {
    if (typeof event.data !== "string") return;
    let message;
    try {
      message = JSON.parse(event.data);
    } catch {
      return;
    }
    if (message?.protocolVersion !== PROTOCOL_VERSION) return;
    if (typeof message.event === "string") {
      let seq;
      try {
        seq = BigInt(message.sequence);
      } catch {
        return;
      }
      if (seq <= lastSequence) return; // duplicate or reordered
      lastSequence = seq;
      for (const listener of listeners) listener(message.event, message.payload ?? {});
      return;
    }
    const entry = pending.get(message.requestId);
    if (!entry) return; // stale or unknown
    pending.delete(message.requestId);
    if (message.ok) entry.resolve(message.result);
    else {
      const err = new Error(message.error?.message ?? "Request failed");
      err.code = message.error?.code;
      err.retryable = Boolean(message.error?.retryable);
      entry.reject(err);
    }
  });
}

export function isAvailable() {
  return Boolean(webview);
}

export function onEvent(listener) {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

function send(operation, payload, draftRevision, attach) {
  if (!webview) return Promise.reject(new Error("The native bridge is not available"));
  const requestId = `req-${nextId++}`;
  const message = { protocolVersion: PROTOCOL_VERSION, requestId, operation, payload };
  if (draftRevision !== undefined && draftRevision !== null) message.draftRevision = String(draftRevision);
  return new Promise((resolve, reject) => {
    // Serialization can throw before anything reaches the native host.
    const text = JSON.stringify(message);
    pending.set(requestId, { resolve, reject });
    try {
      if (attach && typeof webview.postMessageWithAdditionalObjects === "function") {
        // Dropped files travel as native objects; the host reads their paths.
        webview.postMessageWithAdditionalObjects(text, attach);
      } else {
        webview.postMessage(text);
      }
    } catch (error) {
      pending.delete(requestId);
      reject(error);
    }
  });
}

export function request(operation, payload = {}, { draftRevision } = {}) {
  return send(operation, payload, draftRevision, null);
}

// Sends dropped File objects to the host, which resolves their native paths.
export function requestWithFiles(operation, files, payload = {}) {
  return send(operation, payload, undefined, files);
}
