// Modal dialogs on the single <dialog> element: creation result, profile
// change preview, batch planning and confirmation. Content is text-only.
import { has, t } from "./i18n.js";
import { h, replace } from "./dom.js";
import { exactBytes, formatBytes } from "./format.js";
import { collectionPane, textPane } from "./model-pages.js";

const dialog = () => document.getElementById("dialog");
let onClose = null;

function open(title, body, buttons, { wide = false, closed } = {}) {
  const d = dialog();
  if (d.open) {
    // Resolve the previous owner immediately. Its queued close event is
    // ignored by handleClose while a replacement dialog is open.
    d.removeEventListener("close", handleClose);
    const previous = onClose;
    onClose = null;
    d.close();
    previous?.();
  }
  onClose = closed ?? null;
  d.className = wide ? "wide" : "";
  d.setAttribute("aria-labelledby", "dialog-title");
  replace(d,
    h("h2", { id: "dialog-title" }, title),
    h("div", { class: "dialog-body" }, body),
    h("div", { class: "button-row end dialog-buttons" }, buttons));
  d.addEventListener("close", handleClose);
  d.showModal();
  // Focus the first control after the title, or the default button.
  (d.querySelector("[autofocus]") ?? d.querySelector(".dialog-body button, .dialog-body input, .dialog-body select") ??
    d.querySelector(".dialog-buttons button"))?.focus();
}

function handleClose() {
  const d = dialog();
  if (d.open) return;
  d.removeEventListener("close", handleClose);
  const callback = onClose;
  onClose = null;
  replace(d);
  callback?.();
}

export function closeDialog() {
  const d = dialog();
  if (d.open) d.close();
}

function button(label, onclick, attrs = {}) {
  return h("button", { type: "button", onclick, ...attrs }, label);
}

// ---- Confirmation ---------------------------------------------------------------

export function showConfirm(title, message = "") {
  return new Promise((resolve) => {
    let answer = false;
    open(title, message ? h("p", {}, message) : null, [
      button(t("close"), () => closeDialog(), { id: "confirm-no" }),
      button(t("apply"), () => {
        answer = true;
        closeDialog();
      }, { id: "confirm-yes", class: "primary", autofocus: true }),
    ], { closed: () => resolve(answer) });
  });
}

// HTML dialogs work even when the native host disables script prompts.
export function showSaveProfile(save) {
  let busy = false;
  let closed = false;
  const d = dialog();
  const preventCancel = (event) => { if (busy) event.preventDefault(); };
  const error = h("p", { id: "profile-save-error", role: "alert", class: "error", hidden: true });
  const input = h("input", { id: "profile-name", type: "text", required: true, maxlength: 200, autofocus: true,
    oninput: () => input.setCustomValidity("") });
  const cancel = button(t("close"), () => closeDialog(), { id: "profile-name-cancel" });
  const submit = button(t("saveProfile"), null, { id: "profile-name-save", type: "submit", form: "profile-name-form", class: "primary" });
  const form = h("form", { id: "profile-name-form", onsubmit: async (event) => {
    event.preventDefault();
    if (busy) return;
    const name = input.value.trim();
    input.setCustomValidity(!name ? t("profileNameRequired")
      : new TextEncoder().encode(name).length > 200 ? t("profileNameTooLong") : "");
    if (!input.reportValidity()) return;
    busy = true;
    input.disabled = cancel.disabled = submit.disabled = true;
    error.hidden = true;
    try {
      await save(name);
      if (!closed) closeDialog();
    } catch (err) {
      error.textContent = `${t("errorPrefix")}: ${err.message}`;
      error.hidden = false;
    } finally {
      busy = false;
      input.disabled = cancel.disabled = submit.disabled = false;
      if (!closed && !error.hidden) input.focus();
    }
  } }, h("div", { class: "field" }, h("label", { for: "profile-name" }, t("profileNamePrompt")), input), error);
  open(t("saveAsProfile"), form, [cancel, submit], { closed: () => {
    closed = true;
    d.removeEventListener("cancel", preventCancel);
  } });
  d.addEventListener("cancel", preventCancel);
}

export function showMagnetCopy(magnet) {
  const input = h("textarea", { id: "magnet-copy-text", readonly: true, rows: 5,
    "aria-label": t("copyMagnet"), autofocus: true });
  input.value = magnet;
  open(t("copyMagnet"), [h("p", {}, t("copyMagnetManual")), input],
    [button(t("close"), () => closeDialog(), { id: "magnet-copy-close" })]);
  input.select();
}

export function showVerifyFile(file) {
  open(t("verifyFiles"), [
    h("p", { class: "path", id: "verify-file-path" }, file.path),
    h("p", {}, file.status),
    file.message ? h("p", { id: "verify-file-message" }, file.message) : null,
    h("p", {}, `v1: ${file.badV1Pieces ?? "0"}; v2: ${file.badV2Pieces ?? "0"}`),
  ], [button(t("close"), () => closeDialog(), { class: "primary" })], { wide: true });
}

export function showJobText(kind, text) {
  open(t(kind === "log" ? "jobLog" : "warnings"), [h("pre", { class: "log" }, text)],
    [button(t("close"), () => closeDialog(), { class: "primary" })], { wide: true });
}

export function showJobLayoutRow(row) {
  open(t("layoutHeading"), [h("p", { class: "path" }, row.torrentPath), h("p", { class: "path" }, row.local)],
    [button(t("close"), () => closeDialog(), { class: "primary" })], { wide: true });
}

// ---- Result -------------------------------------------------------------------------

export function showResult(job, actions, settings) {
  const r = job.result;
  const confirmBox = h("div", { class: "confirm-inline", id: "client-confirm", hidden: true });
  const openClient = () => {
    if (settings.openClientWithoutAsking) return actions.openInClient(job.id, false);
    const dontAsk = h("input", { type: "checkbox", id: "client-dont-ask" });
    replace(confirmBox,
      h("p", {}, t("clientConfirm")),
      h("label", { class: "check", for: "client-dont-ask" }, dontAsk, h("span", {}, t("dontAskAgain"))),
      h("div", { class: "button-row" },
        button(t("close"), () => { confirmBox.hidden = true; }, { id: "client-no" }),
        button(t("openInClient"), () => {
          confirmBox.hidden = true;
          actions.openInClient(job.id, dontAsk.checked);
        }, { id: "client-yes", class: "primary" })));
    confirmBox.hidden = false;
    confirmBox.querySelector("#client-yes").focus();
  };

  const layout = r.layout ?? [];
  open(job.state === "SucceededWithWarnings" ? `${t("resultHeading")} (${t("state_SucceededWithWarnings")})` : t("resultHeading"), [
    h("dl", { class: "kv" },
      h("dt", {}, t("resultSaved")), h("dd", { class: "path", id: "result-output" }, r.output),
      h("dt", {}, t("nameLabel")), h("dd", {}, r.name),
      h("dt", {}, t("formatLabel")), h("dd", {}, r.format),
      h("dt", {}, t("summaryPayload")), h("dd", { title: exactBytes(r.payloadBytes) }, formatBytes(r.payloadBytes)),
      h("dt", {}, t("summaryFiles")), h("dd", {}, String(r.realFiles)),
      r.paddingFiles ? [h("dt", {}, t("summaryPadFiles")), h("dd", {}, String(r.paddingFiles))] : null,
      h("dt", {}, t("summaryPiece")), h("dd", {}, formatBytes(r.pieceLength)),
      h("dt", {}, t("summaryPieces")), h("dd", {}, String(r.numPieces))),
    h("h3", {}, t("identifiers")),
    h("dl", { class: "kv mono", id: "result-identifiers" },
      r.infohashV1 ? [h("dt", {}, "BTIH (v1)"), h("dd", {}, r.infohashV1)] : null,
      r.infohashV2 ? [h("dt", {}, "BTMH (v2)"), h("dd", {}, r.infohashV2)] : null),
    h("div", { class: "button-row" },
      button(t("copyMagnet"), () => actions.copyMagnet(job.id), { id: "result-copy-magnet" }),
      button(t("saveMagnet"), () => actions.saveMagnet(job.id), { id: "result-save-magnet" }),
      button(t("showInFolder"), () => actions.showInFolder(job.id), { id: "result-show" }),
      button(t("openInClient"), openClient, { id: "result-open-client" }),
      button(t("tabExpert"), () => actions.openJobResult(job.id), { id: "result-expert" })),
    confirmBox,
    r.warnings?.length || r.guaranteeNote
      ? [h("h3", {}, t("warnings")),
        h("ul", { class: "issues", id: "result-warning-rows" },
          (r.warnings ?? []).map((w) => h("li", { class: "warning" }, w)),
          r.guaranteeNote ? h("li", { class: "warning" }, r.guaranteeNote) : null),
        r.warningsTruncated || r.warningsTotal > 5 ? h("div", { class: "button-row" },
          button(t("previous"), () => loadWarnings(Math.max(0, warningOffset - 20)), { id: "result-warning-prev" }),
          button(t("next"), () => loadWarnings(warningOffset + 20), { id: "result-warning-next" })) : null]
      : null,
    layout.length
      ? [h("h3", {}, t("layoutHeading")),
        h("table", { class: "grid compact", id: "result-layout" },
          h("thead", {}, h("tr", {}, h("th", { scope: "col" }, t("colDestination")), h("th", { scope: "col" }, t("colSource")))),
          h("tbody", { id: "result-layout-rows" }, layout.slice(0, 200).map((row) =>
            h("tr", {}, h("td", { class: "path" }, row.torrentPath), h("td", { class: "path" }, row.local))))),
        h("p", { id: "result-layout-count" }),
        h("div", { class: "button-row" },
          button(t("previous"), () => loadLayout(Math.max(0, layoutOffset - 50)), { id: "result-layout-prev" }),
          button(t("next"), () => loadLayout(layoutOffset + 50), { id: "result-layout-next" }))]
      : null,
  ], [button(t("close"), () => closeDialog(), { id: "result-close", class: "primary" })], { wide: true });
  let layoutOffset = 0;
  let layoutRequest = 0;
  const body = document.getElementById("result-layout-rows");
  async function loadLayout(offset) {
    if (!body?.isConnected) return;
    try {
      const requestId = ++layoutRequest;
      const page = await actions.jobLayoutPage(job.id, offset, 50);
      if (!body.isConnected || requestId !== layoutRequest) return;
      layoutOffset = offset;
      replace(body, page.rows.map((row) => h("tr", {}, h("td", { class: "path" }, row.torrentPath), h("td", { class: "path" }, row.local,
        row.displayTruncated ? button(t("details"), () => actions.jobLayoutDetails(job.id, row.index)) : null))));
      document.getElementById("result-layout-count").textContent = t("layoutPage", {
        shown: `${offset + 1}–${offset + page.rows.length}`, total: page.total, realFiles: page.realFiles,
      });
      document.getElementById("result-layout-prev").disabled = offset === 0;
      document.getElementById("result-layout-next").disabled = offset + page.rows.length >= page.total;
    } catch {
      // Keep the preview and allow retry without hiding the creation result.
    }
  }
  if (body) loadLayout(0);
  let warningOffset = 0;
  let warningRequest = 0;
  const warningRows = document.getElementById("result-warning-rows");
  async function loadWarnings(offset) {
    if (!warningRows?.isConnected) return;
    try {
      const requestId = ++warningRequest;
      const page = await actions.jobTextPage(job.id, "warnings", offset, 20);
      if (!warningRows.isConnected || requestId !== warningRequest) return;
      warningOffset = offset;
      replace(warningRows, page.rows.map((row) => h("li", { class: "warning" }, row.text,
        row.displayTruncated ? button(t("details"), () => actions.jobTextDetails(job.id, "warnings", row.index, page.version)) : null)),
        r.guaranteeNote ? h("li", { class: "warning" }, r.guaranteeNote) : null);
      document.getElementById("result-warning-prev").disabled = offset === 0;
      document.getElementById("result-warning-next").disabled = offset + page.rows.length >= page.total;
    } catch { /* Keep the preview available for retry. */ }
  }
  if (r.warningsTruncated || r.warningsTotal > 5) loadWarnings(0);
}

// ---- Profile change preview -----------------------------------------------------------

function describe(value) {
  if (value === null || value === undefined || value === "") return "—";
  if (Array.isArray(value)) return value.length ? value.map((v) => (typeof v === "object" ? v.url ?? JSON.stringify(v) : String(v))).join(", ") : "—";
  if (value === true) return "✓";
  if (value === false) return "✗";
  return String(value);
}

export function showProfileChange(plan, apply, cancel, actions) {
  let applied = false;
  const changes = plan.changes ?? [];
  open(t("profileHeading", { name: plan.profile?.name ?? "" }),
    [changes.length === 0
      ? h("p", {}, t("profileNoChanges"))
      : h("table", { class: "grid compact", id: "profile-changes" },
        h("tbody", {}, changes.map((c) => h("tr", { class: c.removesUserValue ? "removes" : "" },
          h("th", { scope: "row" }, has(`field_${c.field}`) ? t(`field_${c.field}`) : c.field),
          h("td", {}, describe(c.before)),
          h("td", { "aria-hidden": "true" }, "→"),
          h("td", {}, describe(c.after)),
          h("td", { class: "warning" }, c.removesUserValue ? t("profileRemovesUser") : ""))))),
      ...changes.filter((change) => change.paged).map((change) => h("details", {},
        h("summary", {}, `${change.field}: ${t("allRows")}`),
        ...["before", "after"].map((side) => change[`${side}Total`] > 0 ?
          collectionPane(`${change.field} ${side}`,
            (offset, limit) => actions.modelPage("profilePlan", `${change.field}.${side}`, plan.profile.id, plan.draftRevision, offset, limit),
            (row, index) => {
              const detail = h("div", {});
              const keys = typeof row === "object" ? Object.keys(row).filter((key) => typeof row[key] === "string") : [""];
              return h("div", {}, h("pre", {}, describe([row])),
                keys.map((key) => button(key || t("details"), () => replace(detail, textPane((offset) =>
                  actions.modelText("profilePlan", `/${change.field}/${side}/${index}${key ? `/${key}` : ""}`, plan.profile.id, plan.draftRevision, offset))))), detail);
            }, { id: `profile-${change.field}-${side}` }) : typeof change[side] === "string" ?
          h("div", {}, h("h4", {}, side), textPane((offset) =>
            actions.modelText("profilePlan", `/${change.field}/${side}`, plan.profile.id, plan.draftRevision, offset))) : null))) ],
    [
      button(t("close"), () => closeDialog(), { id: "profile-cancel" }),
      button(t("apply"), () => {
        applied = true;
        closeDialog();
      }, { id: "profile-apply", class: "primary", autofocus: true }),
    ],
    { closed: () => (applied ? apply() : cancel?.()) });
}

export function showCollection(label, fetchPage, details, choose) {
  const pane = collectionPane(label, fetchPage, (row, index) => {
    const content = typeof row === "object" ? row.name ?? JSON.stringify(row) : String(row);
    const controls = [];
    if (choose) controls.push(button(t("apply"), () => choose(row), { id: `model-choose-${index}` }));
    if (details) {
      for (const key of typeof row === "object" ? Object.keys(row).filter((key) => typeof row[key] === "string") : [""]) {
        controls.push(button(key || t("details"), () => details(index, key ? `/${key}` : "")));
      }
    }
    return h("div", { class: "collection-row" }, h("pre", {}, content), h("div", { class: "button-row" }, controls));
  });
  open(label, [details ? h("p", { class: "note" }, t("previewOnly")) : null, pane], [button(t("close"), closeDialog)], { wide: true });
}

export function showModelText(label, fetchPage) {
  open(label, textPane(fetchPage), [button(t("close"), closeDialog)], { wide: true });
}

// ---- Batch ---------------------------------------------------------------------------------

export function showBatch(actions) {
  const ui = { mode: "perFile", policy: "rename", plan: null, overrides: {} };
  const preview = h("div", { id: "batch-preview", "aria-live": "polite" });
  const start = button(t("batchStart", { count: 0 }), async () => {
    start.disabled = true;
    const r = await actions.startBatch(ui.plan?.revision);
    if (r?.jobIds) closeDialog();
    else start.disabled = false;
  }, { id: "batch-start", class: "primary", disabled: true });

  const show = (plan) => {
    if (!plan || plan.cancelled) return;
    ui.plan = plan;
    const included = plan.includedTotal ?? plan.items.filter((i) => i.included).length;
    start.textContent = t("batchStart", { count: included });
    start.disabled = included === 0;
    replace(preview,
      h("p", { class: "path" }, `${t("batchFolder")}: ${plan.outputDir}`),
      plan.total > plan.items.length ? collectionPane(t("batchHeading"),
        (offset, limit) => actions.modelPage("batch", "items", "", plan.revision, offset, limit),
        (item) => h("div", { class: "collection-row" },
          h("input", { type: "checkbox", id: `batch-include-${item.id}`, checked: item.included,
            "aria-label": `${t("batchInclude")}: ${item.name}`, onchange: (e) => override(item.id, { included: e.target.checked }) }),
          h("span", { class: "path" }, item.name), h("span", { class: "path", title: item.output }, item.output),
          policySelect(`batch-policy-${item.id}`, item.policy, (value) => override(item.id, { policy: value })),
          batchRoots(item, plan, actions)), { id: "batch-items" }) :
      h("table", { class: "grid compact", id: "batch-items" },
        h("thead", {}, h("tr", {},
          h("th", { scope: "col" }, t("batchInclude")),
          h("th", { scope: "col" }, t("batchName")),
          h("th", { scope: "col" }, t("batchOutput")),
          h("th", { scope: "col" }, t("batchPolicy")))),
        h("tbody", {}, plan.items.map((item) => h("tr", {},
          h("td", {}, h("input", {
            type: "checkbox",
            id: `batch-include-${item.id}`,
            checked: item.included,
            "aria-label": `${t("batchInclude")}: ${item.name}`,
            onchange: (e) => override(item.id, { included: e.target.checked }),
          })),
          h("td", { class: "path" }, item.name),
          h("td", { class: "path", title: item.output },
            item.output.split(/[\\/]/).pop(),
            batchRoots(item, plan, actions),
            item.existsOnDisk ? h("span", { class: "tag" }, t("batchExists")) : null,
            item.duplicateInBatch ? h("span", { class: "tag" }, t("batchDuplicate")) : null),
          h("td", {}, policySelect(`batch-policy-${item.id}`, item.policy, (v) => override(item.id, { policy: v }))))))),
      plan.notesTotal > 0 ? collectionPane(t("batchHeading"),
        (offset, limit) => actions.modelPage("batch", "notes", "", plan.revision, offset, limit),
        (note, index) => {
          const detail = h("div", {});
          return h("div", {}, h("p", { class: "warning" }, note), button(t("details"), () => replace(detail,
            textPane((offset) => actions.modelText("batch", `/notes/${index}`, "", plan.revision, offset)))), detail);
        }, { id: "batch-notes" }) :
      plan.notes?.length ? h("ul", { class: "issues" }, plan.notes.map((n) => h("li", { class: "warning" }, n))) : null);
  };

  // The service re-resolves conflicts on every update, so all overrides are sent each time.
  const override = async (id, change) => {
    // Native state keeps all previous row overrides, including unloaded rows.
    show(await actions.updateBatch({ [id]: change }, ui.plan.revision));
  };
  const plan = async (chooseFolder = false) => {
    ui.overrides = {};
    show(await actions.planBatch(ui.mode, ui.policy, chooseFolder));
  };

  const mode = h("select", { id: "batch-mode", onchange: (e) => { ui.mode = e.target.value; plan(); } },
    h("option", { value: "perFile" }, t("batchPerFile")),
    h("option", { value: "perChildFolder" }, t("batchPerFolder")),
    h("option", { value: "single" }, t("batchSingle")));
  mode.value = ui.mode;

  open(t("batchHeading"), [
    h("div", { class: "field" }, h("label", { for: "batch-mode" }, t("batchMode")), mode),
    h("div", { class: "field" }, h("label", { for: "batch-default-policy" }, t("batchPolicy")),
      policySelect("batch-default-policy", ui.policy, (v) => { ui.policy = v; plan(); })),
    h("div", { class: "button-row" }, button(t("batchChooseFolder"), () => plan(true), { id: "batch-folder" })),
    preview,
  ], [button(t("close"), () => closeDialog(), { id: "batch-close" }), start], { wide: true });
  plan();
}

function batchRoots(item, plan, actions) {
  if (!item.sourceRootsTotal) return null;
  const content = h("div", {});
  const root = h("details", {}, h("summary", {}, t("batchSources", { count: item.sourceRootsTotal })), content);
  root.addEventListener("toggle", () => {
    if (!root.open || content.childNodes.length) return;
    replace(content, collectionPane(t("sourcesHeading"),
      (offset, limit) => actions.modelPage("batch", "sourceRoots", item.id, plan.revision, offset, limit),
      (path, index) => {
        const text = h("div", {});
        return h("div", {}, h("span", { class: "path" }, path), button(t("details"), () => replace(text,
          textPane((offset) => actions.modelText("batch", `/sourceRoots/${index}`, item.id, plan.revision, offset)))), text);
      }, { id: `batch-sources-${item.id}` }));
  });
  return root;
}

function policySelect(id, value, onchange) {
  const el = h("select", { id, onchange: (e) => onchange(e.target.value) },
    h("option", { value: "rename" }, t("policyRename")),
    h("option", { value: "skip" }, t("policySkip")),
    h("option", { value: "replace" }, t("policyReplace")));
  el.value = value;
  return el;
}
