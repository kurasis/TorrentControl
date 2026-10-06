// Views: pure functions of the page state that rebuild their container. Every
// value from the native side is inserted as text (see dom.js). Inputs carry
// stable ids so a re-render keeps focus and what the user is typing.
import { has, t } from "./i18n.js";
import { h, replace } from "./dom.js";
import { exactBytes, formatBytes, formatDuration, percent, PIECE_SIZES } from "./format.js";
import { collectionPane } from "./model-pages.js";
import { VirtualList } from "./virtual-list.js";
import { renderDiagnostics } from "./diagnostics.js";
import { renderMetadataEditor } from "./metadata-editor.js";

const TABS = ["files", "general", "trackers", "webSeeds", "metadata", "expert", "jobs"];
const TAB_LABEL = {
  files: "tabFiles",
  general: "tabGeneral",
  trackers: "tabTrackers",
  webSeeds: "tabWebSeeds",
  metadata: "tabMetadata",
  expert: "tabExpert",
  jobs: "tabJobs",
};
const ACTIVE = new Set(["Queued", "Scanning", "Hashing", "Pausing", "Paused", "Cancelling", "Validating", "Committing"]);

// The workspace holds inputs and virtual lists, so it is rebuilt only when
// something it shows has changed.
export function workspaceKey(state) {
  const job = state.tab === "jobs" && state.selectedJob ? state.jobs.get(state.selectedJob) : null;
  return JSON.stringify([
    state.settings.mode,
    state.settings.language,
    state.tab,
    state.draft?.revision,
    state.scan?.state,
    state.scan?.sourcesRevision,
    state.filter,
    state.selectedSource,
    state.torrent?.id,
    state.editor?.version,
    state.diagnosticVersion,
    state.profiles.map((p) => p.id),
    state.validation?.draftRevision,
    job ? [job.id, job.version] : null,
  ]);
}

function field(id, label, control, hint) {
  return h("div", { class: "field" },
    h("label", { for: id }, label),
    control,
    hint ? h("p", { class: "hint", id: `${id}-hint` }, hint) : null);
}

function textInput(id, fieldName, value, actions, attrs = {}) {
  return h("input", {
    id,
    type: "text",
    value: value ?? "",
    dataset: { field: fieldName },
    spellcheck: "false",
    oninput: (e) => actions.edit(fieldName, e.target.value),
    ...attrs,
  });
}

function checkbox(id, label, checked, onchange, attrs = {}) {
  return h("label", { class: "check", for: id },
    h("input", { id, type: "checkbox", checked, onchange: (e) => onchange(e.target.checked), ...attrs }),
    h("span", {}, label));
}

function select(id, value, options, onchange, attrs = {}) {
  const el = h("select", { id, onchange: (e) => onchange(e.target.value), ...attrs },
    options.map(([v, label]) => h("option", { value: v }, label)));
  el.value = String(value);
  return el;
}

function card(title, ...children) {
  return h("section", { class: "card" }, title ? h("h2", {}, title) : null, ...children);
}

function note(key) {
  return h("p", { class: "note" }, t(key));
}

// ---- Sources (both modes) ---------------------------------------------------------

function scanLine(state) {
  const scan = state.scan ?? {};
  if (scan.state === "scanning") return h("p", { class: "scan busy", role: "status" }, t("scanScanning"));
  if (scan.state === "failed") return h("p", { class: "scan error", role: "alert" }, t("scanFailed", { message: scan.error?.message ?? "" }));
  const s = state.validation?.summary;
  if (scan.state === "ready" && s?.realFiles !== undefined)
    return h("p", { class: "scan" }, t("scanReady", { files: s.realFiles, size: formatBytes(s.payloadBytes) }));
  return null;
}

function renderSources(state, actions) {
  const sources = state.draft?.sources ?? [];
  const revision = state.draft?.revision;
  const renderRow = (source) => h("div", { class: state.selectedSource === source.id ? "selected collection-row" : "collection-row" },
    h("span", { class: "kind" }, source.isDirectory ? t("folder") : t("file")),
    h("button", { type: "button", class: "link path", title: source.path, onclick: () => actions.selectSource(source.id) }, source.name || source.path),
    h("button", { type: "button", class: "icon", id: `remove-${source.id}`,
      "aria-label": t("removeSource", { name: source.name || source.path }), onclick: () => actions.removeSource(source.id, revision) }, "×"));
  const paged = state.draft?.pages?.sources;
  return card(t("sourcesHeading"),
    h("div", { class: "button-row" },
      h("button", { type: "button", id: "add-files", onclick: () => actions.selectSources("files") }, t("addFiles")),
      h("button", { type: "button", id: "add-folder", onclick: () => actions.selectSources("folder") }, t("addFolder")),
      h("span", { class: "hint drop-hint" }, t("dropHint"))),
    paged?.total > sources.length ? collectionPane(t("sourcesHeading"),
      (offset, limit) => actions.modelPage("draft", "sources", "", revision, offset, limit),
      renderRow, { id: "source-list" }) : sources.length === 0
      ? h("p", { class: "empty" }, t("noSources"))
      : h("div", { class: "sources", id: "source-list" }, sources.map(renderRow)),
    scanLine(state));
}

// ---- Simple mode ------------------------------------------------------------------

function profileOptions(state) {
  return state.profiles.map((p) => [p.id, has(`profile_${p.id}`) ? t(`profile_${p.id}`) : p.name]);
}

function renderOutput(state, actions) {
  const d = state.draft;
  return h("div", { class: "field" },
    h("label", { for: "choose-output" }, t("destinationLabel")),
    h("div", { class: "output-row" },
      h("span", { class: "path", id: "output-path", title: d.output || "" }, d.output || t("notChosen")),
      h("button", { type: "button", id: "choose-output", onclick: () => actions.chooseOutput() }, t("chooseOutput"))),
    checkbox("replace-existing", t("replaceExisting"), d.replaceExisting,
      (v) => actions.edit("replaceExisting", v, { immediate: true })));
}

function renderSimple(state, actions) {
  const d = state.draft;
  return card(null,
    field("draft-name", t("nameLabel"), textInput("draft-name", "name", d.name, actions, { placeholder: d.effectiveName ?? "", disabled: !!d.textFields?.name })),
    field("draft-profile", t("profileLabel"),
      h("div", { class: "button-row" }, select("draft-profile", d.profile, profileOptions(state), (v) => actions.pickProfile(v)),
        state.profilesTotal > state.profiles.length ? h("button", { type: "button", id: "browse-profiles", onclick: () => actions.browseProfiles() }, t("allProfiles")) : null)),
    renderOutput(state, actions));
}

// ---- Advanced mode ----------------------------------------------------------------

function renderTabs(state, actions) {
  const list = h("div", { class: "tabs", role: "tablist", "aria-label": t("modeAdvanced") },
    TABS.map((tab) => h("button", {
      type: "button",
      role: "tab",
      id: `tab-${tab}`,
      "aria-selected": String(state.tab === tab),
      "aria-controls": "tab-panel",
      tabindex: state.tab === tab ? "0" : "-1",
      onclick: () => actions.setTab(tab),
    }, t(TAB_LABEL[tab]))));
  list.addEventListener("keydown", (e) => {
    const i = TABS.indexOf(state.tab);
    let next = null;
    if (e.key === "ArrowRight") next = TABS[(i + 1) % TABS.length];
    else if (e.key === "ArrowLeft") next = TABS[(i - 1 + TABS.length) % TABS.length];
    else if (e.key === "Home") next = TABS[0];
    else if (e.key === "End") next = TABS[TABS.length - 1];
    if (!next) return;
    e.preventDefault();
    actions.setTab(next);
    requestAnimationFrame(() => requestAnimationFrame(() => document.getElementById(`tab-${next}`)?.focus()));
  });
  return list;
}

function renderFilesTab(state, actions) {
  const panel = h("div", {});
  const filter = h("input", {
    id: "files-filter",
    type: "search",
    value: state.filter,
    placeholder: t("filesFilterPlaceholder"),
  });
  let timer = null;
  filter.addEventListener("input", () => {
    clearTimeout(timer);
    timer = setTimeout(() => actions.setFilter(filter.value), 250);
  });
  panel.append(field("files-filter", t("filesFilter"), filter));

  const ready = state.scan?.state === "ready";
  const header = h("div", { class: "vlist-header", "aria-hidden": "true" },
    h("span", { class: "grow" }, t("colDestination")), h("span", { class: "num" }, t("colSize")));
  const files = h("div", { id: "manifest-list", class: "files" });
  panel.append(header, files);
  if (ready) {
    const list = new VirtualList(files, {
      label: t("tabFiles"),
      fetchPage: (offset, limit) => actions.manifestPage(offset, limit, state.filter),
      renderRow: (e) => h("div", { title: e.source },
        h("span", { class: "grow path" }, e.torrentPath),
        e.requiresHydration ? h("span", { class: "tag" }, "cloud") : null,
        h("span", { class: "num", title: exactBytes(e.length) }, formatBytes(e.length))),
      renderPlaceholder: () => h("div", { class: "placeholder" }, t("loadingRow")),
    });
    list.reset();
  }

  const skipped = state.validation?.summary;
  if (ready && skipped && (skipped.skipped > 0 || skipped.unreadable > 0)) {
    const box = h("div", { id: "skipped-list", class: "files short" });
    panel.append(h("h3", {}, t("skippedHeading")),
      h("div", { class: "vlist-header", "aria-hidden": "true" },
        h("span", { class: "grow" }, t("colSource")), h("span", {}, t("colReason"))),
      box);
    new VirtualList(box, {
      label: t("skippedHeading"),
      fetchPage: (offset, limit) => actions.skippedPage(offset, limit),
      renderRow: (item) => h("div", { class: item.blocking ? "blocking" : "" },
        h("span", { class: "grow path" }, item.path),
        h("span", { class: "reason" }, `${item.kind}: ${item.reason}`)),
    }).reset();
  }

  const source = state.selectedSourceData ?? state.draft.sources.find((s) => s.id === state.selectedSource);
  if (source) panel.append(renderSourceOptions(source, actions, state.draft.revision,
    state.sourceOptionsPending?.sourceId === source.id ? state.sourceOptionsPending.values : {}));
  return panel;
}

function renderSourceOptions(source, actions, revision, pending) {
  const options = {
    recursive: pending.recursive ?? source.recursive,
    followLinks: pending.followLinks ?? source.followLinks,
    skipCloud: pending.skipCloud ?? source.skipCloud,
  };
  const exclusions = h("textarea", { id: "source-exclusions", rows: "4", spellcheck: "false" });
  exclusions.value = pending.exclusionsText ?? (source.exclusions ?? []).join("\n");
  exclusions.addEventListener("input", () => actions.stageSourceOption(source.id, "exclusionsText", exclusions.value));
  const set = (key) => (v) => {
    options[key] = v;
    actions.stageSourceOption(source.id, key, v);
  };
  return card(t("sourceOptions", { name: source.name || source.path }),
    source.isDirectory ? checkbox("source-recursive", t("recursive"), options.recursive, set("recursive")) : null,
    source.isDirectory ? checkbox("source-follow", t("followLinks"), options.followLinks, set("followLinks")) : null,
    checkbox("source-cloud", t("skipCloud"), options.skipCloud, set("skipCloud")),
    h("button", { type: "button", id: "source-full-path", onclick: () => actions.readModelText("source", "path", source.id, revision, t("colSource")) }, t("details")),
    source.isDirectory ? source.exclusionsPaged ? editableRows("exclusions", source.id, revision, actions, t("exclusions")) : field("source-exclusions", t("exclusions"), exclusions) : null,
    h("button", {
      type: "button",
      id: "apply-source-options",
      onclick: () => actions.setSourceOptions(source.id, {
        ...options,
        ...(source.exclusionsPaged ? {} : { exclusions: exclusions.value.split("\n").map((s) => s.trim()).filter(Boolean) }),
      }, revision),
    }, t("applyOptions")));
}

function renderGeneralTab(state, actions) {
  const d = state.draft;
  const custom = state.profiles.find((p) => p.id === d.profile && !p.builtin);
  const pieceOptions = [["0", t("pieceAuto")], ...PIECE_SIZES.map((n) => [String(n), formatBytes(n)])];
  const fixed = Number(d.fixedDate || 0);
  const dateInput = h("input", {
    id: "draft-fixed-date",
    type: "datetime-local",
    step: "1",
    value: new Date(fixed * 1000).toISOString().slice(0, 19),
    disabled: d.creationDate !== "fixed",
    dataset: { field: "fixedDate" },
    onchange: (e) => {
      const seconds = Math.floor(Date.parse(`${e.target.value}Z`) / 1000);
      if (Number.isFinite(seconds) && seconds >= 0) actions.edit("fixedDate", seconds, { immediate: true });
    },
  });
  return h("div", {},
    field("draft-name", t("nameLabel"), textInput("draft-name", "name", d.name, actions, { placeholder: d.effectiveName ?? "", disabled: !!d.textFields?.name })),
    h("div", { class: "field" },
      h("label", { for: "draft-profile" }, t("profileLabel")),
      h("div", { class: "button-row" },
        select("draft-profile", d.profile, profileOptions(state), (v) => actions.pickProfile(v)),
        state.profilesTotal > state.profiles.length ? h("button", { type: "button", id: "browse-profiles", onclick: () => actions.browseProfiles() }, t("allProfiles")) : null,
        h("button", { type: "button", id: "save-profile", onclick: () => actions.saveProfile() }, t("saveAsProfile")),
        custom ? h("button", { type: "button", id: "export-profile", onclick: () => actions.exportProfile(custom.id) }, t("exportProfile")) : null,
        custom ? h("button", { type: "button", id: "delete-profile", class: "danger", onclick: () => actions.deleteProfile(custom.id) }, t("deleteProfile")) : null)),
    field("draft-format", t("formatLabel"),
      select("draft-format", d.format, [["hybrid", t("formatHybrid")], ["v1", t("formatV1")], ["v2", t("formatV2")]],
        (v) => actions.edit("format", v, { immediate: true }))),
    field("draft-piece", t("pieceSizeLabel"),
      select("draft-piece", d.pieceLength, pieceOptions, (v) => actions.edit("pieceLength", Number(v), { immediate: true }))),
    checkbox("draft-private", t("privateLabel"), d.private, (v) => actions.edit("private", v, { immediate: true })),
    renderOutput(state, actions),
    field("draft-date", t("creationDateLabel"),
      h("div", { class: "button-row" },
        select("draft-date", d.creationDate, [["now", t("dateNow")], ["omit", t("dateOmit")], ["fixed", t("dateFixed")]],
          (v) => actions.edit("creationDate", v, { immediate: true })),
        dateInput)),
    checkbox("draft-hydration", t("allowHydration"), d.allowHydration, (v) => actions.edit("allowHydration", v, { immediate: true })),
    checkbox("draft-resources", t("acceptResources"), d.acceptLargeResourceUse,
      (v) => actions.edit("acceptLargeResourceUse", v, { immediate: true })));
}

function renderTrackersTab(state, actions) {
  if (state.draft.pages?.trackers.total > state.draft.trackers.length || state.draft.trackers.some((row) => row.displayTruncated))
    return h("div", {}, editableRows("trackers", "", state.draft.revision, actions, t("tabTrackers")), renderDiagnostics(state, actions, "trackers"));
  const trackers = state.draft.trackers.map((tr) => ({ ...tr }));
  const commit = (immediate = true) => actions.edit("trackers", trackers.map((tr) => ({ ...tr })), { immediate });
  const move = (i, delta) => {
    const j = i + delta;
    if (j < 0 || j >= trackers.length) return;
    [trackers[i], trackers[j]] = [trackers[j], trackers[i]];
    commit();
  };
  const table = h("table", { class: "grid", id: "tracker-table" },
    h("thead", {}, h("tr", {},
      h("th", { scope: "col" }, t("trackerTier")),
      h("th", { scope: "col" }, t("trackerUrl")),
      h("th", { scope: "col" }, t("trackerEnabled")),
      h("th", { scope: "col" }, t("trackerStatus")),
      h("th", { scope: "col" }, h("span", { class: "sr-only" }, t("remove"))))),
    h("tbody", {}, trackers.map((tr, i) => h("tr", {},
      h("td", {}, h("input", {
        id: `tracker-tier-${i}`,
        type: "number",
        min: "0",
        max: "1000",
        class: "narrow",
        value: String(tr.tier),
        "aria-label": `${t("trackerTier")} ${i + 1}`,
        onchange: (e) => {
          tr.tier = Math.max(0, Math.min(1000, Number(e.target.value) || 0));
          commit();
        },
      })),
      h("td", {}, h("input", {
        id: `tracker-url-${i}`,
        type: "url",
        value: tr.url,
        spellcheck: "false",
        dataset: { field: "trackers" },
        "aria-label": `${t("trackerUrl")} ${i + 1}`,
        oninput: (e) => {
          tr.url = e.target.value;
          commit(false);
        },
      })),
      h("td", {}, h("input", {
        id: `tracker-on-${i}`,
        type: "checkbox",
        checked: tr.enabled,
        "aria-label": `${t("trackerEnabled")} ${i + 1}`,
        onchange: (e) => {
          tr.enabled = e.target.checked;
          commit();
        },
      })),
      h("td", { class: "muted" }, t("trackerUnchecked")),
      h("td", { class: "row-actions" },
        h("button", { type: "button", class: "icon", id: `tracker-up-${i}`, "aria-label": t("moveUp"), title: t("moveUp"), onclick: () => move(i, -1) }, "↑"),
        h("button", { type: "button", class: "icon", id: `tracker-down-${i}`, "aria-label": t("moveDown"), title: t("moveDown"), onclick: () => move(i, 1) }, "↓"),
        h("button", {
          type: "button",
          class: "icon",
          id: `tracker-remove-${i}`,
          "aria-label": t("remove"),
          title: t("remove"),
          onclick: () => {
            trackers.splice(i, 1);
            commit();
          },
        }, "×"))))));

  const paste = h("textarea", { id: "tracker-paste", rows: "5", spellcheck: "false", placeholder: "udp://tracker.example:1337/announce" });
  return h("div", {},
    table,
    h("div", { class: "button-row" },
      h("button", {
        type: "button",
        id: "add-tracker",
        onclick: () => {
          const tier = trackers.length ? Math.max(...trackers.map((tr) => tr.tier)) + 1 : 0;
          trackers.push({ url: "", tier, enabled: true });
          commit();
        },
      }, t("addTracker"))),
    field("tracker-paste", t("pasteTrackers"), paste, t("pasteHint")),
    h("button", {
      type: "button",
      id: "apply-paste",
      onclick: () => {
        let tier = trackers.length ? Math.max(...trackers.map((tr) => tr.tier)) + 1 : 0;
        let used = false;
        for (const raw of paste.value.split(/\r?\n/)) {
          const url = raw.trim();
          if (!url) {
            if (used) tier += 1;
            used = false;
            continue;
          }
          trackers.push({ url, tier, enabled: true });
          used = true;
        }
        commit();
      },
    }, t("applyPaste")),
    renderDiagnostics(state, actions, "trackers"));
}

function resolvedSeed(url, state) {
  const name = state.draft.effectiveName || "";
  const single = state.validation?.summary?.mode === "single-file";
  if (!url) return "";
  if (single) return url.endsWith("/") ? url + encodeURIComponent(name) : url;
  return `${url.endsWith("/") ? url : `${url}/`}${encodeURIComponent(name)}/…`;
}

function renderWebSeedsTab(state, actions) {
  if (state.draft.pages?.webSeeds.total > state.draft.webSeeds.length)
    return h("div", {}, editableRows("webSeeds", "", state.draft.revision, actions, t("tabWebSeeds")), renderDiagnostics(state, actions, "web-seeds"));
  const seeds = [...state.draft.webSeeds];
  const commit = (immediate = true) => actions.edit("webSeeds", [...seeds], { immediate });
  return h("div", {},
    h("ul", { class: "plain", id: "web-seed-list" }, seeds.map((url, i) => h("li", {},
      h("div", { class: "button-row" },
        h("input", {
          id: `web-seed-${i}`,
          type: "url",
          value: url,
          spellcheck: "false",
          dataset: { field: "webSeeds" },
          "aria-label": `${t("webSeedUrl")} ${i + 1}`,
          oninput: (e) => {
            seeds[i] = e.target.value;
            commit(false);
          },
        }),
        h("button", {
          type: "button",
          class: "icon",
          id: `web-seed-remove-${i}`,
          "aria-label": t("remove"),
          title: t("remove"),
          onclick: () => {
            seeds.splice(i, 1);
            commit();
          },
        }, "×")),
      url ? h("p", { class: "hint" }, `${t("resolvedPreview")}: ${resolvedSeed(url, state)}`) : null))),
    h("button", {
      type: "button",
      id: "add-web-seed",
      disabled: state.draft.private,
      onclick: () => {
        seeds.push("");
        commit();
      },
    }, t("addWebSeed")),
    renderDiagnostics(state, actions, "web-seeds"));
}

function renderMetadataTab(state, actions) {
  const d = state.draft;
  const comment = h("textarea", {
    id: "draft-comment",
    rows: "4",
    dataset: { field: "comment" },
    oninput: (e) => actions.edit("comment", e.target.value),
  });
  comment.value = d.comment ?? "";
  comment.disabled = !!d.textFields?.comment;
  return h("div", {},
    field("draft-comment", t("commentLabel"), comment),
    field("draft-creator", t("creatorLabel"), textInput("draft-creator", "creator", d.creator, actions, { disabled: !!d.textFields?.creator })),
    field("draft-source", t("sourceLabel"), textInput("draft-source", "source", d.source, actions, { disabled: !!d.textFields?.source })),
    ...Object.keys(d.textFields ?? {}).filter((key) => d.textFields[key] > 65536).map((key) => {
      const canReplace = ["name", "comment", "creator", "source"].includes(key);
      const replacement = h("input", { type: "text", maxlength: "4096", "aria-label": `${key}: ${t("replacementValue")}` });
      return h("div", { class: "field" }, h("p", { class: "note" }, `${key}: ${t("previewOnly")}`),
        h("button", { type: "button", onclick: () => actions.readModelText("draft", key, "", d.revision, key) }, t("details")),
        canReplace ? replacement : null, canReplace ? h("button", { type: "button", onclick: () => actions.edit(key, replacement.value, { immediate: true }) }, t("replaceValue")) : null);
    }),
    note("optionalFieldsLater"));
}

function kv(label, value, attrs = {}) {
  return [h("dt", {}, label), h("dd", attrs, value === undefined || value === null || value === "" ? "—" : String(value))];
}

function renderExpertTab(state, actions) {
  const tor = state.torrent;
  if (!tor) {
    return h("div", {},
      h("p", {}, t("expertIntro")),
      h("button", { type: "button", id: "expert-open", onclick: () => actions.openTorrent() }, t("openTorrent")),
      h("p", { class: "note" }, t("editorScopeHint")));
  }
  const files = h("div", { id: "torrent-files", class: "files" });
  const labelFor = (key) => t(({ name: "nameLabel", path: "resultSaved", comment: "commentLabel",
    createdBy: "creatorLabel", source: "sourceLabel", pieceLength: "pieceSizeLabel",
    trackers: "field_trackers", webSeeds: "field_webSeeds", problems: "problems" })[key] ?? key);
  new VirtualList(files, {
    label: t("summaryFiles"),
    fetchPage: (offset, limit) => actions.torrentFilesPage(tor.id, offset, limit),
    renderRow: (f) => h("div", { class: f.pad ? "pad" : "" },
      h("span", { class: "grow path" }, f.path),
      h("span", { class: "num", title: exactBytes(f.length) }, formatBytes(f.length))),
  }).reset();
  return h("div", {},
    h("h3", {}, t("torrentInfo")),
    h("dl", { class: "kv", id: "torrent-info" },
      kv(t("nameLabel"), tor.name),
      kv(t("resultSaved"), tor.path, { class: "path" }),
      kv(t("formatLabel"), tor.format),
      kv(t("pieceSizeLabel"), tor.pieceLength ? formatBytes(tor.pieceLength) : ""),
      kv(t("summaryFiles"), tor.realFiles),
      kv(t("summaryPadFiles"), tor.paddingFiles),
      kv(t("summaryPayload"), formatBytes(tor.payloadBytes)),
      kv(t("field_private"), tor.private ? "✓" : "—"),
      kv(t("commentLabel"), tor.comment),
      kv(t("creatorLabel"), tor.createdBy),
      kv(t("field_source"), tor.source),
      kv(t("field_trackers"), (tor.trackers ?? []).map((tr) => `[${tr.tier}] ${tr.url}`).join("\n"), { class: "pre" }),
      kv(t("field_webSeeds"), (tor.webSeeds ?? []).join("\n"), { class: "pre" })),
    ...Object.keys(tor.textFields ?? {}).filter((key) => key !== "magnet").map((key) =>
      h("button", { type: "button", id: `torrent-full-${key}`, onclick: () => actions.readModelText("torrent", key, tor.id, "", labelFor(key)) }, `${labelFor(key)}: ${t("details")}`)),
    ...["trackers", "webSeeds", "problems"].filter((key) => (tor[`${key}Total`] ?? 0) > 0).map((key) =>
      h("button", { type: "button", id: `torrent-all-${key}`, onclick: () => actions.browseCollection("torrent", key, tor.id, "", labelFor(key)) }, `${labelFor(key)}: ${t("allRows")} (${tor[`${key}Total`]})`)),
    h("h3", {}, t("identifiers")),
    h("dl", { class: "kv mono" },
      tor.infohashV1 ? kv("BTIH (v1)", tor.infohashV1) : null,
      tor.infohashV2 ? kv("BTMH (v2)", tor.infohashV2) : null),
    tor.problems?.length
      ? [h("h3", {}, t("problems")), h("ul", { class: "issues" }, tor.problems.map((p) => h("li", { class: "warning" }, p)))]
      : null,
    h("div", { class: "button-row" },
      h("button", { type: "button", id: "expert-verify", onclick: () => actions.verifyTorrent(tor.id) }, t("verifyPayload")),
      h("button", { type: "button", id: "expert-copy-magnet", onclick: () => actions.copyMagnet(tor.id) }, t("copyMagnet")),
      h("button", { type: "button", id: "expert-save-magnet", onclick: () => actions.saveMagnet(tor.id) }, t("saveMagnet")),
      h("button", { type: "button", id: "expert-open", onclick: () => actions.openTorrent() }, t("openTorrent"))),
    h("h3", {}, t("summaryFiles")),
    files,
    renderMetadataEditor(state, actions.renderEditor, actions.metadataSaved),
    renderDiagnostics(state, actions, "trackers", tor.id));
}

function renderJobsTab(state, actions) {
  const job = state.selectedJob ? state.jobs.get(state.selectedJob) : null;
  if (!job) return h("p", { class: "empty" }, t("noJobs"));
  const verify = job.verify;
  let logOffset = 0;
  const logBox = h("pre", { class: "log", id: "job-log", tabindex: "0" }, (job.log ?? []).join("\n"));
  const logLabel = h("p", { id: "job-log-page" });
  const logPrevious = h("button", { type: "button", id: "job-log-prev", onclick: () => loadLog(Math.max(0, logOffset - 20)) }, t("previous"));
  const logNext = h("button", { type: "button", id: "job-log-next", onclick: () => loadLog(logOffset + 20) }, t("next"));
  let logRequest = 0;
  async function loadLog(offset) {
    const requestId = ++logRequest;
    try {
      const page = await actions.jobTextPage(job.id, "log", offset, 20);
      if (!logBox.isConnected || requestId !== logRequest) return;
      logOffset = offset;
      replace(logBox, page.rows.map((row) => h("span", {}, row.text,
        row.displayTruncated ? h("button", { type: "button", onclick: () => actions.jobTextDetails(job.id, "log", row.index, page.version) }, t("details")) : null, "\n")));
      logLabel.textContent = t("logPage", { shown: `${offset + (page.rows.length ? 1 : 0)}–${offset + page.rows.length}`, total: page.total });
      logPrevious.disabled = offset === 0;
      logNext.disabled = offset + page.rows.length >= page.total;
    } catch { /* Keep the preview available for retry. */ }
  }
  loadLog(0);
  let verification = null;
  if (verify) {
    let errorsOnly = true;
    const checkbox = h("input", { type: "checkbox", id: "verify-errors-only", checked: true });
    const rows = h("div", { id: "verify-files", class: "files" });
    verification = h("div", { id: "verify-result" },
      h("p", { class: verify.ok ? "ok" : "error" }, verify.ok ? "OK" : "PAYLOAD_MISMATCH"),
      h("label", { class: "check", for: "verify-errors-only" }, checkbox, h("span", {}, t("verifyErrorsOnly"))), rows);
    const list = new VirtualList(rows, {
      label: t("verifyFiles"),
      fetchPage: (offset, limit) => actions.verifyFilesPage(job.id, offset, limit, errorsOnly),
      renderRow: (file) => h("div", { class: file.status === "ok" ? "ok" : "error" },
        h("span", { class: "path" }, `${file.path}: ${file.status}${file.message ? ` (${file.message})` : ""}`),
        h("button", { type: "button", onclick: () => actions.verifyFileDetails(job.id, file.index) }, t("details"))),
    });
    checkbox.addEventListener("change", () => { errorsOnly = checkbox.checked; list.reset(); });
    list.reset();
  }
  return h("div", {},
    h("h3", {}, job.name),
    h("dl", { class: "kv" },
      kv(t("trackerStatus"), t(`state_${job.state}`)),
      kv(t("summaryPayload"), `${formatBytes(job.bytesDone)} / ${formatBytes(job.bytesTotal)}`),
      kv(t("summaryFiles"), `${job.filesDone} / ${job.filesTotal}`),
      job.error ? kv(t("errorPrefix"), `${job.error.code}: ${job.error.message}`) : null,
      job.result ? kv(t("resultSaved"), job.result.output, { class: "path" }) : null),
    verification,
    job.result ? h("button", { type: "button", id: "job-show-result", onclick: () => actions.showResult(job.id) }, t("details")) : null,
    h("h3", {}, t("jobLog")),
    logBox, logLabel, h("div", { class: "button-row" }, logPrevious, logNext));
}

const TAB_RENDER = {
  files: renderFilesTab,
  general: renderGeneralTab,
  trackers: renderTrackersTab,
  webSeeds: renderWebSeedsTab,
  metadata: renderMetadataTab,
  expert: renderExpertTab,
  jobs: renderJobsTab,
};

export function renderWorkspace(container, state, actions) {
  if (!state.draft) return replace(container);
  if (state.settings.mode !== "advanced") {
    return replace(container, renderSources(state, actions), renderSimple(state, actions));
  }
  const tab = TABS.includes(state.tab) ? state.tab : "files";
  return replace(container,
    renderSources(state, actions),
    renderTabs(state, actions),
    h("div", { class: "tab-panel", id: "tab-panel", role: "tabpanel", "aria-labelledby": `tab-${tab}` },
      TAB_RENDER[tab](state, actions)));
}

// ---- Review -------------------------------------------------------------------------

function reviewValue(item) {
  if (item.field === "pieceLength") return formatBytes(item.value);
  if (item.value === true) return "✓";
  return String(item.value);
}

export function renderReview(container, state, actions) {
  const v = state.validation;
  const s = v?.summary ?? {};
  const issues = v?.issues ?? [];
  const rows = [];
  if (s.payloadBytes !== undefined) rows.push([t("summaryPayload"), formatBytes(s.payloadBytes), exactBytes(s.payloadBytes)]);
  if (s.logicalBytes !== undefined && s.paddingBytes !== "0")
    rows.push([t("summaryLogical"), formatBytes(s.logicalBytes), exactBytes(s.logicalBytes)]);
  if (s.realFiles !== undefined) rows.push([t("summaryFiles"), String(s.realFiles)]);
  if (s.paddingFiles) rows.push([t("summaryPadFiles"), String(s.paddingFiles)]);
  if (s.pieceLength)
    rows.push([t("summaryPiece"), s.pieceLengthAutomatic ? t("summaryPieceAuto", { size: formatBytes(s.pieceLength) }) : formatBytes(s.pieceLength)]);
  if (s.pieceCount) rows.push([t("summaryPieces"), s.pieceCount]);
  if (s.estimatedMetainfoBytes) rows.push([t("summaryMetainfo"), formatBytes(s.estimatedMetainfoBytes)]);
  if (s.skipped || s.unreadable) rows.push([t("summarySkipped"), String((s.skipped ?? 0) + (s.unreadable ?? 0))]);

  const canCreate = Boolean(v?.canCreate) && v.draftRevision === state.draft?.revision && state.pendingFields.size === 0;
  replace(container,
    h("h2", { id: "review-heading" }, t("reviewHeading")),
    h("p", { class: "review-name", title: state.draft?.effectiveName ?? "" }, state.draft?.effectiveName ?? ""),
    h("dl", { class: "kv", id: "summary" }, rows.map(([label, value, title]) => [h("dt", {}, label), h("dd", { title }, value)])),
    v?.review?.length
      ? [h("h3", {}, t("activeSettings")),
        h("ul", { class: "chips", id: "active-settings" }, v.review.map((item) =>
          h("li", {}, `${has(`field_${item.field}`) ? t(`field_${item.field}`) : item.field}: ${reviewValue(item)}`)))]
      : null,
    ...["issues", "review"].filter((key) => (v?.[`${key}Total`] ?? 0) > 0).map((key) => {
      const label = t(key === "issues" ? "problems" : "activeSettings");
      return h("button", { type: "button", id: `review-all-${key}`, onclick: () => actions.browseCollection("review", key, "", v.draftRevision, label) }, `${label}: ${t("allRows")} (${v[`${key}Total`]})`);
    }),
    issues.length
      ? h("ul", { class: "issues", id: "issues", "aria-live": "polite" }, issues.map((i) =>
        h("li", { class: i.severity === "error" ? "error" : "warning", dataset: { code: i.code } }, i.message)))
      : v ? h("p", { class: "ok", id: "issues" }, t("noIssues")) : null,
    h("div", { class: "button-row end" },
      h("button", {
        type: "button",
        id: "btn-batch",
        disabled: !(state.draft?.sources?.length > 0),
        onclick: () => actions.openBatch(),
      }, t("createBatch")),
      h("button", {
        type: "button",
        id: "btn-create",
        class: "primary",
        disabled: !canCreate,
        "aria-describedby": "issues",
        onclick: () => actions.create(),
      }, t("create"))));
}

// ---- Jobs panel -------------------------------------------------------------------------

export function renderJobs(container, state, actions) {
  const history = state.jobHistory;
  const jobs = history.ids.map((id) => state.jobs.get(id)).filter(Boolean);
  const anyFinished = jobs.some((j) => !ACTIVE.has(j.state));
  replace(container,
    h("div", { class: "panel-head" },
      h("h2", { id: "jobs-heading" }, t("jobsHeading")),
      anyFinished || history.total > 0 ? h("button", { type: "button", id: "clear-finished", onclick: () => actions.clearFinished() }, t("clearFinished")) : null),
    h("p", { id: "jobs-page-status", role: "status" }, t("collectionPage", { first: jobs.length ? history.offset + 1 : 0, last: history.offset + jobs.length, total: Math.max(history.total, jobs.length) })),
    h("div", { class: "button-row" },
      h("button", { type: "button", id: "jobs-first", disabled: history.offset === 0, onclick: () => actions.firstJobsPage() }, t("firstPage")),
      h("button", { type: "button", id: "jobs-previous", disabled: history.offset === 0, onclick: () => actions.previousJobsPage() }, t("previous")),
      h("button", { type: "button", id: "jobs-next", disabled: history.nextOffset === null, onclick: () => actions.nextJobsPage() }, t("next")),
      h("button", { type: "button", id: "jobs-latest", disabled: history.followNewest, onclick: () => actions.latestJobsPage() }, t("latestJobs")),
      h("button", { type: "button", id: "jobs-retry", onclick: () => actions.loadJobsPage() }, t("retry"))),
    jobs.length === 0
      ? h("p", { class: "empty" }, t("noJobs"))
      : h("ul", { class: "jobs", id: "job-list" }, jobs.map((job) => renderJob(job, state, actions))));
}

function renderJob(job, state, actions) {
  const pct = percent(job.bytesDone, job.bytesTotal);
  const active = ACTIVE.has(job.state);
  const details = [];
  if (job.state === "Hashing" && job.bytesPerSecond > 0) {
    details.push(t("speed", { speed: formatBytes(Math.round(job.bytesPerSecond)) }));
    if (job.etaSeconds !== null && job.etaSeconds !== undefined) details.push(t("eta", { time: formatDuration(job.etaSeconds) }));
  }
  if (job.error) details.push(job.error.message);
  return h("li", { class: `job state-${job.state}${state.selectedJob === job.id ? " selected" : ""}`, dataset: { job: job.id } },
    h("div", { class: "job-line" },
      h("span", { class: "job-name", title: job.name }, job.name),
      h("span", { class: "job-state" }, t(`state_${job.state}`))),
    h("progress", { max: "100", value: String(job.state.startsWith("Succeeded") ? 100 : pct), "aria-label": job.name }),
    h("div", { class: "job-line small" },
      h("span", { class: "path muted", title: job.currentFile ?? "" }, active ? job.currentFile ?? "" : ""),
      h("span", {}, details.join(" · "))),
    h("div", { class: "button-row" },
      job.state === "Hashing"
        ? h("button", { type: "button", id: `pause-${job.id}`, onclick: () => actions.pauseJob(job.id) }, t("pause"))
        : null,
      job.state === "Paused" || job.state === "Pausing"
        ? h("button", { type: "button", id: `resume-${job.id}`, onclick: () => actions.resumeJob(job.id) }, t("resume"))
        : null,
      active && job.state !== "Committing" && job.state !== "Cancelling"
        ? h("button", { type: "button", id: `cancel-${job.id}`, onclick: () => actions.cancelJob(job.id) }, t("cancel"))
        : null,
      h("button", {
        type: "button",
        id: `details-${job.id}`,
        onclick: () => {
          actions.selectJob(job.id);
          if (job.result) actions.showResult(job.id);
          else if (state.settings.mode === "advanced") actions.setTab("jobs");
        },
      }, t("details"))));
}

function editableRows(key, owner, revision, actions, label) {
  const pane = collectionPane(label, (offset, limit) => actions.modelPage("draft", key, owner, revision, offset, limit), (value, index) => {
    const row = typeof value === "object" ? { ...value } : value;
    if (row.displayTruncated && key === "trackers") {
      const replacement = h("input", { type: "text", maxlength: "4096", id: `paged-replace-trackers-${index}`, "aria-label": t("replacementValue") });
      return h("div", {}, h("p", { class: "note" }, t("previewOnly")),
        h("input", { type: "text", disabled: true, value: row.url, id: `paged-trackers-${index}` }),
        h("button", { type: "button", id: `paged-full-trackers-${index}`, onclick: () => actions.readModelText("draft", `/trackers/${index}/url`, "", revision, label) }, t("details")),
        replacement, h("button", { type: "button", id: `paged-apply-trackers-${index}`, onclick: () => actions.editRow(key, owner, index, "set",
          { url: replacement.value, tier: Math.min(999, Math.max(0, row.tier)), enabled: row.enabled }, revision) }, t("replaceValue")),
        h("button", { type: "button", onclick: () => actions.editRow(key, owner, index, "remove", null, revision) }, t("remove")));
    }
    const input = h("input", { type: "text", id: `paged-${key}-${index}`, value: key === "trackers" ? row.url : row,
      dataset: { rowEditor: "true" },
      onblur: () => actions.renderEditor(),
      "aria-label": `${label} ${index + 1}`, spellcheck: "false", onchange: (event) => {
        if (key === "trackers") row.url = event.target.value;
        actions.editRow(key, owner, index, "set", key === "trackers" ? row : event.target.value, revision);
      } });
    return h("div", { class: "collection-row" }, input,
      key === "trackers" ? h("input", { type: "number", id: `paged-tier-${index}`, value: row.tier, min: 0, max: 999,
        dataset: { rowEditor: "true" },
        onblur: () => actions.renderEditor(),
        "aria-label": t("colTier"), onchange: (event) => { row.tier = Number(event.target.value); actions.editRow(key, owner, index, "set", row, revision); } }) : null,
      key === "trackers" ? h("input", { type: "checkbox", id: `paged-enabled-${index}`, checked: row.enabled,
        "aria-label": t("colEnabled"), onchange: (event) => { row.enabled = event.target.checked; actions.editRow(key, owner, index, "set", row, revision); } }) : null,
      h("button", { type: "button", id: `paged-remove-${key}-${index}`, onclick: () => actions.editRow(key, owner, index, "remove", null, revision) }, t("remove")),
      key === "trackers" ? h("button", { type: "button", onclick: () => actions.editRow(key, owner, index, "up", null, revision) }, t("moveUp")) : null,
      key === "trackers" ? h("button", { type: "button", onclick: () => actions.editRow(key, owner, index, "down", null, revision) }, t("moveDown")) : null);
  }, { id: `paged-${key}` });
  const value = h("input", { type: "text", id: `paged-add-${key}-value`, "aria-label": label });
  return h("div", {}, pane, h("div", { class: "button-row" }, value,
    h("button", { type: "button", id: `paged-add-${key}`, onclick: () => actions.editRow(key, owner, 0, "append",
      key === "trackers" ? { url: value.value, tier: 0, enabled: true } : value.value, revision) }, t("addRow"))));
}
