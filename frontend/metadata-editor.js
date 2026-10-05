import { request } from "./bridge.js";
import { h } from "./dom.js";
import { t } from "./i18n.js";

const keyId = (key) => key.t === "bytes" ? key.hex.toLowerCase()
  : Array.from(new TextEncoder().encode(key.utf8), (b) => b.toString(16).padStart(2, "0")).join("");
const keyText = (key) => key.utf8 ?? `0x${key.hex}`;
const taggedText = (text) => ({ t: "str", utf8: text });
const lines = (text) => text.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);

export async function loadMetadata(state, invalidate, scope = "top", offset = 0) {
  const torrentId = state.torrent.id;
  const [page, registry] = await Promise.all([
    request("getTorrentFields", { torrentId, scope, offset, limit: 50 }),
    state.editor?.registry ? { fields: state.editor.registry } : request("getFieldRegistry"),
  ]);
  if (state.torrent?.id !== torrentId) return;
  const old = state.editor;
  state.editor = { ...(old?.torrentId === torrentId ? old : { changes: [], version: 0 }),
    torrentId, scope, offset, rows: page.rows, total: page.total, registry: registry.fields,
    selection: null, dirty: false, buffer: "", remove: false, error: "", busy: false };
  state.editor.version++;
  invalidate("workspace");
}

function formKind(selection) {
  const f = selection?.descriptor;
  if (!f || f.support !== "form") return "typed";
  const v = selection.value;
  // Imported byte strings may be binary even for commonly textual keys.
  // Show the tagged value instead of substituting an empty decoded string.
  if (v && (v.t === "bytes" || ((f.validation === "text" || f.validation === "tracker-url") && v.t !== "str"))) return "typed";
  if (f.validation === "text" || f.validation === "tracker-url") return "text";
  if (f.validation === "unix-seconds") return "int";
  if (f.validation === "private-one") return "private";
  if (f.validation === "tracker-tiers") return "tiers";
  return "urls";
}

function bufferFor(selection) {
  const v = selection.value;
  const kind = formKind(selection);
  if (kind === "text") return v?.utf8 ?? "";
  if (kind === "int") return v?.v ?? "0";
  if (kind === "private") return v?.v === "1";
  if (kind === "tiers") return (v?.items ?? []).map((tier) => (tier.items ?? []).map((s) => s.utf8 ?? "").join("\n")).join("\n\n");
  if (kind === "urls") return v?.t === "str" ? v.utf8 : (v?.items ?? []).map((s) => s.utf8 ?? "").join("\n");
  return JSON.stringify(v ?? { t: "str", utf8: "" }, null, 2);
}

function valueFor(editor) {
  if (editor.remove) return null;
  const kind = formKind(editor.selection);
  if (kind === "text") return taggedText(editor.buffer);
  if (kind === "int") return { t: "int", v: editor.buffer };
  if (kind === "private") return editor.buffer ? { t: "int", v: "1" } : null;
  if (kind === "tiers") return { t: "list", items: editor.buffer.trim().split(/\r?\n\s*\r?\n/)
    .map((tier) => ({ t: "list", items: lines(tier).map(taggedText) })) };
  if (kind === "urls") {
    const urls = lines(editor.buffer);
    // Preserve the imported BEP 19 string/list shape when possible.
    if (editor.selection.descriptor.key === "url-list" && editor.selection.value?.t === "str" && urls.length === 1)
      return taggedText(urls[0]);
    return { t: "list", items: urls.map(taggedText) };
  }
  return JSON.parse(editor.buffer);
}

function stage(state) {
  const e = state.editor;
  if (!e.selection || !e.dirty) return;
  const value = valueFor(e);
  const id = `${e.scope}:${keyId(e.selection.key)}`;
  e.changes = e.changes.filter((change) => change.id !== id);
  e.changes.push({ id, scope: e.scope, key: e.selection.key, value });
  e.dirty = false;
  state.editorPreview = null;
}

export function renderMetadataEditor(state, invalidate, onSaved) {
  const e = state.editor;
  if (!e) return h("p", {}, t("editorLoading"));
  const run = async (action) => {
    if (e.busy) return;
    e.busy = true;
    e.error = "";
    e.version++;
    invalidate("workspace");
    try { await action(); }
    catch (err) { e.error = `${err.code ?? t("errorPrefix")}: ${err.message}`; }
    finally { e.busy = false; e.version++; invalidate("workspace"); }
  };
  const select = (key) => run(async () => {
    stage(state);
    const selected = await request("getTorrentField", { torrentId: e.torrentId, scope: e.scope, key });
    const staged = e.changes.find((c) => c.id === `${e.scope}:${keyId(selected.key)}`);
    if (staged) selected.value = staged.value;
    e.selection = selected;
    e.buffer = bufferFor(selected);
    e.remove = !!staged && staged.value === null;
    e.dirty = false;
  });
  const edit = (value) => {
    e.buffer = value;
    e.dirty = true;
    state.editorPreview = null;
    // Keep the input node/focus while typing; only the preview is disabled.
    document.getElementById("editor-preview-panel")?.remove();
  };
  const s = e.selection;
  const f = s?.descriptor;
  const kind = formKind(s);
  const input = kind === "private"
    ? h("input", { type: "checkbox", id: "editor-value", checked: e.buffer, disabled: !s?.editable || e.busy,
      onchange: (event) => edit(event.target.checked) })
    : h(kind === "text" || kind === "int" ? "input" : "textarea", {
      id: "editor-value", value: e.buffer, rows: kind === "typed" ? 10 : 5,
      readonly: !s?.editable, disabled: e.busy,
      oninput: (event) => edit(event.target.value), spellcheck: false,
    });
  const preview = state.editorPreview;
  const displayedValue = (value) => value === null ? t("editorAbsent") : JSON.stringify(value, null, 2);
  const hashRows = (hashes) => Object.entries(hashes).map(([format, hash]) => h("p", { class: "mono path" }, `${format}: ${hash}`));
  const scopeControl = h("select", { id: "editor-scope", disabled: e.busy, onchange: (event) => run(async () => {
    stage(state); await loadMetadata(state, invalidate, event.target.value);
  }) }, ["top", "info"].map((scope) => h("option", { value: scope, selected: e.scope === scope }, scope)));
  const addKnown = h("select", { id: "editor-add-known", disabled: e.busy, onchange: (event) => {
    if (event.target.value) select(taggedText(event.target.value));
  } }, h("option", { value: "" }, t("editorAddField")), e.registry.filter((field) => field.scope === e.scope && field.editable)
    .map((field) => h("option", { value: field.key }, `${field.key} (${field.reference})`)));
  const extension = h("input", { id: "editor-extension-key", type: "text", placeholder: t("editorKeyHint"), disabled: e.busy });
  return h("section", { id: "metadata-editor", "aria-label": t("editorTitle"),
    dataset: { torrentId: e.torrentId, scope: e.scope, selectedKey: s ? keyId(s.key) : "" } },
    h("h3", {}, t("editorTitle")), h("p", { class: "note" }, t("editorScopeHint")),
    h("div", { class: "field" }, h("label", { for: "editor-scope" }, t("editorScope")), scopeControl),
    h("div", { class: "button-row" }, addKnown, extension,
      h("button", { type: "button", id: "editor-add-extension", disabled: e.busy, onclick: () => {
        const key = extension.value.startsWith("0x") ? { t: "bytes", hex: extension.value.slice(2) } : taggedText(extension.value);
        select(key);
      } }, t("editorExtension"))),
    h("ul", { class: "editor-fields" }, e.rows.map((row) => h("li", {},
      h("button", { type: "button", disabled: e.busy || row.key.truncated, onclick: () => select(row.key) }, keyText(row.key)),
      h("span", { class: "note" }, ` ${row.descriptor?.support ?? t("editorUnknown")} · ${row.descriptor?.type ?? row.value.t} · ${row.descriptor?.reference ?? t("editorNoStandard")}`)))),
    h("div", { class: "button-row" },
      h("button", { type: "button", disabled: e.busy || e.offset === 0, onclick: () => run(async () => {
        stage(state); await loadMetadata(state, invalidate, e.scope, Math.max(0, e.offset - 50));
      }) }, t("editorPrevious")),
      h("span", {}, `${e.offset + 1}–${Math.min(e.offset + 50, e.total)} / ${e.total}`),
      h("button", { type: "button", disabled: e.busy || e.offset + 50 >= e.total, onclick: () => run(async () => {
        stage(state); await loadMetadata(state, invalidate, e.scope, e.offset + 50);
      }) }, t("editorNext"))),
    s ? h("div", { class: "editor-selection" },
      h("h4", {}, keyText(s.key)),
      h("p", { class: "note" }, `${f?.reference ?? t("editorNoStandard")} · ${f?.validation ?? t("editorUnknown")} · ${e.scope === "info" ? t("editorHashChanges") : t("editorHashKept")}`),
      !s.editable ? h("p", { class: "warning" }, t("editorProtected")) : null,
      h("label", { for: "editor-value" }, kind === "typed" ? t("editorTypedHint") : keyText(s.key)), input,
      kind === "tiers" ? h("p", { class: "note" }, t("editorTiersHint")) : null,
      h("label", { class: "check" }, h("input", { type: "checkbox", id: "editor-remove", checked: e.remove,
        disabled: !s.editable || e.busy, onchange: (event) => { e.remove = event.target.checked; edit(e.buffer); } }), t("editorRemove")),
      s.signed ? h("label", { class: "check" }, h("input", { type: "checkbox", id: "editor-remove-signatures",
        checked: e.removeSignatures, disabled: e.busy, onchange: (event) => {
          e.removeSignatures = event.target.checked; state.editorPreview = null; e.version++; invalidate("workspace");
        } }), t("editorSignatures")) : null) : null,
    h("p", { id: "editor-pending" }, t("editorPending", { count: e.changes.length })),
    h("button", { type: "button", id: "editor-preview", disabled: e.busy, onclick: () => run(async () => {
      stage(state);
      state.editorPreview = await request("previewTorrentEdit", { torrentId: e.torrentId,
        outer: e.changes.filter((c) => c.scope === "top").map(({ key, value }) => ({ key, value })),
        info: e.changes.filter((c) => c.scope === "info").map(({ key, value }) => ({ key, value })),
        removeSignatures: !!e.removeSignatures });
    }) }, t("editorPreview")),
    h("p", { id: "editor-error", role: "alert", class: "error", hidden: !e.error }, e.error),
    preview ? h("div", { id: "editor-preview-panel" },
      h("h4", {}, t("editorPreview")),
      (preview.changes ?? []).map((change) => h("details", {},
        h("summary", {}, `${change.scope}.${keyText(change.key)}`),
        h("p", {}, t("editorBefore")), h("pre", { class: "path" }, displayedValue(change.before)),
        h("p", {}, t("editorAfter")), h("pre", { class: "path" }, displayedValue(change.after)))),
      h("p", { class: preview.infoChanged ? "warning" : "ok" }, preview.infoChanged ? t("editorHashChanges") : t("editorHashKept")),
      h("h5", {}, t("editorBefore")), hashRows(preview.oldHashes),
      h("h5", {}, t("editorAfter")), hashRows(preview.newHashes),
      preview.signaturesRemoved ? h("p", { class: "warning" }, t("editorSignaturesRemoved")) : null,
      h("button", { type: "button", id: "editor-output", disabled: e.busy, onclick: () => run(async () => {
        const chosen = await request("chooseEditorOutput", { token: preview.token });
        if (!chosen.cancelled) { state.editorPreview = chosen; e.replace = false; }
      }) }, t("editorSaveAs")),
      preview.outputChosen ? [h("p", { class: "path" }, preview.output),
        preview.requiresReplace ? h("label", { class: "check" }, h("input", { type: "checkbox", id: "editor-replace", checked: e.replace,
          disabled: e.busy, onchange: (event) => { e.replace = event.target.checked; e.version++; invalidate("workspace"); } }), t("editorReplace")) : null,
        h("button", { type: "button", id: "editor-save", disabled: e.busy || (preview.requiresReplace && !e.replace), onclick: () => run(async () => {
          const result = await request("saveTorrentEdit", { token: preview.token, replaceExisting: !!e.replace });
          await onSaved(result);
        }) }, t("editorSave"))] : null) : null);
}
