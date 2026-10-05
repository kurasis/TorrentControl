// Modal dialogs on the single <dialog> element: creation result, profile
// change preview, batch planning and confirmation. Content is text-only.
import { has, t } from "./i18n.js";
import { h, replace } from "./dom.js";
import { exactBytes, formatBytes } from "./format.js";

const dialog = () => document.getElementById("dialog");
let onClose = null;

function open(title, body, buttons, { wide = false, closed } = {}) {
  const d = dialog();
  if (d.open) {
    // The close event is queued, so detach it first: it would clear the new content.
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
  d.addEventListener("close", handleClose, { once: true });
  d.showModal();
  // Focus the first control after the title, or the default button.
  (d.querySelector("[autofocus]") ?? d.querySelector(".dialog-body button, .dialog-body input, .dialog-body select") ??
    d.querySelector(".dialog-buttons button"))?.focus();
}

function handleClose() {
  const callback = onClose;
  onClose = null;
  replace(dialog());
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
        h("ul", { class: "issues" },
          (r.warnings ?? []).map((w) => h("li", { class: "warning" }, w)),
          r.guaranteeNote ? h("li", { class: "warning" }, r.guaranteeNote) : null)]
      : null,
    layout.length
      ? [h("h3", {}, t("layoutHeading")),
        h("table", { class: "grid compact", id: "result-layout" },
          h("thead", {}, h("tr", {}, h("th", { scope: "col" }, t("colDestination")), h("th", { scope: "col" }, t("colSource")))),
          h("tbody", {}, layout.slice(0, 200).map((row) =>
            h("tr", {}, h("td", { class: "path" }, row.torrentPath), h("td", { class: "path" }, row.local)))))]
      : null,
  ], [button(t("close"), () => closeDialog(), { id: "result-close", class: "primary" })], { wide: true });
}

// ---- Profile change preview -----------------------------------------------------------

function describe(value) {
  if (value === null || value === undefined || value === "") return "—";
  if (Array.isArray(value)) return value.length ? value.map((v) => (typeof v === "object" ? v.url ?? JSON.stringify(v) : String(v))).join(", ") : "—";
  if (value === true) return "✓";
  if (value === false) return "✗";
  return String(value);
}

export function showProfileChange(plan, apply, cancel) {
  let applied = false;
  const changes = plan.changes ?? [];
  open(t("profileHeading", { name: plan.profile?.name ?? "" }),
    changes.length === 0
      ? h("p", {}, t("profileNoChanges"))
      : h("table", { class: "grid compact", id: "profile-changes" },
        h("tbody", {}, changes.map((c) => h("tr", { class: c.removesUserValue ? "removes" : "" },
          h("th", { scope: "row" }, has(`field_${c.field}`) ? t(`field_${c.field}`) : c.field),
          h("td", {}, describe(c.before)),
          h("td", { "aria-hidden": "true" }, "→"),
          h("td", {}, describe(c.after)),
          h("td", { class: "warning" }, c.removesUserValue ? t("profileRemovesUser") : ""))))),
    [
      button(t("close"), () => closeDialog(), { id: "profile-cancel" }),
      button(t("apply"), () => {
        applied = true;
        closeDialog();
      }, { id: "profile-apply", class: "primary", autofocus: true }),
    ],
    { closed: () => (applied ? apply() : cancel?.()) });
}

// ---- Batch ---------------------------------------------------------------------------------

export function showBatch(actions) {
  const ui = { mode: "perFile", policy: "rename", plan: null, overrides: {} };
  const preview = h("div", { id: "batch-preview", "aria-live": "polite" });
  const start = button(t("batchStart", { count: 0 }), async () => {
    start.disabled = true;
    const r = await actions.startBatch();
    if (r?.jobIds) closeDialog();
    else start.disabled = false;
  }, { id: "batch-start", class: "primary", disabled: true });

  const show = (plan) => {
    if (!plan || plan.cancelled) return;
    ui.plan = plan;
    const included = plan.items.filter((i) => i.included).length;
    start.textContent = t("batchStart", { count: included });
    start.disabled = included === 0;
    replace(preview,
      h("p", { class: "path" }, `${t("batchFolder")}: ${plan.outputDir}`),
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
            item.existsOnDisk ? h("span", { class: "tag" }, t("batchExists")) : null,
            item.duplicateInBatch ? h("span", { class: "tag" }, t("batchDuplicate")) : null),
          h("td", {}, policySelect(`batch-policy-${item.id}`, item.policy, (v) => override(item.id, { policy: v }))))))),
      plan.notes?.length ? h("ul", { class: "issues" }, plan.notes.map((n) => h("li", { class: "warning" }, n))) : null);
  };

  // The service re-resolves conflicts on every update, so all overrides are sent each time.
  const override = async (id, change) => {
    ui.overrides[id] = { ...ui.overrides[id], ...change };
    if (change.included === true) delete ui.overrides[id].included;
    show(await actions.updateBatch(ui.overrides));
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

function policySelect(id, value, onchange) {
  const el = h("select", { id, onchange: (e) => onchange(e.target.value) },
    h("option", { value: "rename" }, t("policyRename")),
    h("option", { value: "skip" }, t("policySkip")),
    h("option", { value: "replace" }, t("policyReplace")));
  el.value = value;
  return el;
}
