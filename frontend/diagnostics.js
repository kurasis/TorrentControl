import { h } from "./dom.js";
import { t } from "./i18n.js";

export function renderDiagnostics(state, actions, kind, torrentId = "") {
  const policy = state.diagnosticPolicy;
  const run = state.diagnosticRun;
  const busy = run?.state === "running";
  const page = state.diagnosticPage;
  const input = h("input", { id: "diagnostics-proxy", type: "text", value: policy.httpProxy,
    placeholder: "http://proxy.example:8080", disabled: busy,
    oninput: (event) => { policy.httpProxy = event.target.value; } });
  const mode = h("select", { id: "diagnostics-network", disabled: busy, onchange: (event) => {
    policy.networkMode = event.target.value; actions.renderDiagnostics();
  } }, ["direct", "http-proxy"].map((value) => h("option", { value, selected: policy.networkMode === value }, t(`diagnosticNetwork_${value}`))));
  const check = (type, id) => actions.startDiagnostics(type, id);
  const checkboxes = [
    ["diagnostics-refresh", "refresh", "diagnosticRefresh"],
    ["diagnostics-retry", "udpRetry", "diagnosticRetry"],
  ].map(([id, property, text]) => h("label", { class: "check" }, h("input", { type: "checkbox", id, checked: policy[property], disabled: busy,
    onchange: (event) => { policy[property] = event.target.checked; } }), t(text)));
  return h("section", { id: "diagnostics-panel", "aria-label": t("diagnosticTitle") },
    h("h3", {}, t("diagnosticTitle")), h("p", { class: "note" }, t("diagnosticScope")),
    h("div", { class: "field" }, h("label", { for: "diagnostics-network" }, t("diagnosticNetwork")), mode),
    policy.networkMode === "http-proxy" ? h("div", { class: "field" }, h("label", { for: "diagnostics-proxy" }, t("diagnosticProxy")), input) : null,
    checkboxes,
    h("div", { class: "button-row" },
      h("button", { type: "button", id: `diagnostics-check-${kind}`, disabled: busy, onclick: () => check(kind, torrentId) }, t("diagnosticCheck")),
      torrentId ? h("button", { type: "button", id: "diagnostics-check-web-seeds", disabled: busy, onclick: () => check("web-seeds", torrentId) }, t("diagnosticCheckSeeds")) : null,
      h("button", { type: "button", id: "diagnostics-cancel", disabled: !busy, onclick: () => actions.cancelDiagnostics() }, t("cancel"))),
    kind === "trackers" && !torrentId ? h("div", { class: "button-row" },
      h("button", { type: "button", id: "catalog-fetch", disabled: busy, onclick: () => actions.updateCatalog() }, t("diagnosticCatalogFetch")),
      h("button", { type: "button", id: "catalog-review", disabled: busy, onclick: () => actions.reviewCatalog() }, t("diagnosticCatalogReview"))) : null,
    h("p", { id: "diagnostics-error", class: "error", role: "alert", hidden: !state.diagnosticError }, state.diagnosticError),
    run ? h("div", {},
      h("p", { id: "diagnostics-progress", role: "status" }, `${t(`diagnosticRun_${run.state}`)}: ${run.completed}/${run.total} · ${t(`diagnosticNetwork_${run.network}`)}`),
      h("p", { class: "note" }, t("diagnosticNoIntegrity")),
      h("ul", { class: "issues", id: "diagnostics-results" }, (page?.rows ?? []).map((row) => h("li", {},
        h("strong", {}, row.endpoint), ` · ${t(`diagnosticState_${row.state}`)} · ${row.operation ?? row.kind}`,
        h("p", { class: "note" }, `${t("diagnosticCheckedAt")}: ${new Date(Number(row.checkedAt) * 1000).toLocaleString()}${row.cached ? ` · ${t("diagnosticCached")}` : ""}`),
        row.file ? h("p", { class: "path" }, `${row.file} · ${t("diagnosticSample", { total: row.totalFiles })}`) : null,
        h("details", {}, h("summary", {}, t("diagnosticFamilies")), (row.attempts ?? []).map((a) =>
          h("p", {}, `IPv${a.family}: ${t(`diagnosticState_${a.state}`)} · ${a.latencyMs ?? "—"} ms · HTTP ${a.httpStatus ?? "—"} · DNS ${a.dnsMs ?? "—"} ms · TLS ${a.tlsMs ?? "—"} ms`)))))),
      h("div", { class: "button-row" },
        h("button", { type: "button", disabled: (state.diagnosticOffset ?? 0) === 0, onclick: () => actions.loadDiagnostics(Math.max(0, state.diagnosticOffset - 50)) }, t("editorPrevious")),
        h("button", { type: "button", disabled: (state.diagnosticOffset ?? 0) + 50 >= (page?.total ?? 0), onclick: () => actions.loadDiagnostics(state.diagnosticOffset + 50) }, t("editorNext")))) : null);
}
