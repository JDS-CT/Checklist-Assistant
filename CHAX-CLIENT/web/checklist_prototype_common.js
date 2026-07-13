// Shared prototype logic for checklist assistant variants.
// Provides role toggles, edit gating, and draft row/section creation with required fields.
(function () {
  const SAVE_ICONS = {
    pending: "&#8987;", // hourglass
    unsaved: "&#10060;", // cross mark
    saved: "&#10003;", // check mark
  };
  const TOKEN_STORE_KEY = "chax_oauth_token";
  const PKCE_STORE_KEY = "chax_pkce";
  const HOST_KEY = "chax_host";
  const PORT_KEY = "chax_port";
  const SHOW_NA_ROWS_KEY = "chax_show_na_rows";
  const WRAP_ROW_TEXT_KEY = "chax_wrap_row_text";
  const CONFIG = window.CHAX_CONFIG || {};
  const DEV_MODE = !!CONFIG.devMode;
  const SHOW_PRINCIPAL = !!CONFIG.showEntityPrincipal;
  const PUBLIC_CLIENT_ID = CONFIG.oauthClientId || "chax-ui-client";
  const PUBLIC_CLIENT_SECRET = CONFIG.oauthClientSecret || "chax-ui-secret";

  function escapeHtml(text) {
    return (text || "").replace(/[&<>]/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[ch]));
  }

  function escapeHtmlAttr(text) {
    return (text || "").replace(/[&<>"']/g, (ch) => {
      switch (ch) {
        case "&":
          return "&amp;";
        case "<":
          return "&lt;";
        case ">":
          return "&gt;";
        case "\"":
          return "&quot;";
        case "'":
          return "&#39;";
        default:
          return ch;
      }
    });
  }

  const BASE32_TOKEN_RE = /^[0-9A-HJKMNP-TV-Z]+$/;
  const SPEC_INPUT_HINT =
    "Verify supports: <=x, >=x, <x, >x, =x, !=x; ranges a..b, [a,b], (a,b], [a,b). Compatibility: [a - b] unit (for example [2.5 - 3.5] bar).";

  function normalizeBase32Id(value) {
    return (value || "").trim().toUpperCase();
  }

  function isValidBase32Id(value, expectedLen) {
    const normalized = normalizeBase32Id(value);
    return normalized.length === expectedLen && BASE32_TOKEN_RE.test(normalized);
  }

  function copyToClipboard(value, label) {
    const text = value || "";
    if (!text) {
      alert(`${label || "Value"} is empty.`);
      return;
    }
    if (navigator.clipboard?.writeText) {
      navigator.clipboard.writeText(text).catch(() => {
        fallbackCopy(text);
      });
    } else {
      fallbackCopy(text);
    }
  }

  function fallbackCopy(text) {
    const temp = document.createElement("textarea");
    temp.value = text;
    document.body.appendChild(temp);
    temp.select();
    document.execCommand("copy");
    document.body.removeChild(temp);
  }

  function loadAuthState() {
    try {
      const raw = sessionStorage.getItem(TOKEN_STORE_KEY);
      if (!raw) return null;
      return JSON.parse(raw);
    } catch {
      return null;
    }
  }

  function saveAuthState(state) {
    if (!state) {
      sessionStorage.removeItem(TOKEN_STORE_KEY);
      return;
    }
    sessionStorage.setItem(TOKEN_STORE_KEY, JSON.stringify(state));
  }

  function clearPkceState() {
    sessionStorage.removeItem(PKCE_STORE_KEY);
  }

  function resolveMarkdownUrl(value, basePath) {
    const trimmed = String(value || "").trim();
    if (!trimmed) return "";
    const lowered = trimmed.toLowerCase();
    if (lowered.startsWith("javascript:")) return "";
    if (lowered.startsWith("http://") || lowered.startsWith("https://") || lowered.startsWith("data:")) {
      return trimmed;
    }
    if (lowered.startsWith("blob:")) return trimmed;
    if (trimmed.startsWith("/")) return trimmed;
    if (!basePath) return trimmed;
    const normalized = trimmed.replace(/^\.?\//, "");
    return basePath + normalized;
  }

  function inlineMd(text, basePath) {
    const raw = String(text || "");
    const segments = raw.split(/(`[^`]*`)/g);
    return segments
      .map((segment) => {
        if (segment.startsWith("`") && segment.endsWith("`")) {
          return `<code>${escapeHtml(segment.slice(1, -1))}</code>`;
        }
        return inlineMdNoCode(segment, basePath);
      })
      .join("");
  }

  function inlineMdNoCode(text, basePath) {
    const images = [];
    let withImages = String(text || "").replace(/!\[([^\]]*)\]\(([^)]+)\)/g, (match, alt, srcRaw) => {
      const src = resolveMarkdownUrl(String(srcRaw || "").split(/\s+/)[0], basePath);
      if (!src) {
        return alt || "";
      }
      const token = `@@IMG${images.length}@@`;
      images.push(buildMarkdownMediaTag(src, alt || ""));
      return token;
    });

    let escaped = escapeHtml(withImages);
    escaped = escaped.replace(/\*\*(.+?)\*\*/g, "<strong>$1</strong>").replace(/\*(.+?)\*/g, "<em>$1</em>");
    escaped = escaped.replace(/@@IMG(\d+)@@/g, (match, idx) => images[Number(idx)] || "");
    return escaped;
  }

  function buildMarkdownMediaTag(src, alt) {
    const escapedAlt = escapeHtmlAttr(alt || "");
    const escapedSrc = escapeHtmlAttr(src);
    const lowered = src.toLowerCase().split("?")[0].split("#")[0];
    if (lowered.endsWith(".mp4") || lowered.endsWith(".webm") || lowered.endsWith(".mkv")) {
      return `<video src="${escapedSrc}" controls preload="metadata" aria-label="${escapedAlt}"></video>`;
    }
    else if (lowered.endsWith(".avif")) {
      const base = src.slice(0, -5);
      const png = escapeHtmlAttr(`${base}.png`);
      const jpg = escapeHtmlAttr(`${base}.jpg`);
      return `<picture><source type="image/avif" srcset="${escapedSrc}"><img src="${png}" data-fallback-jpg="${jpg}" alt="${escapedAlt}" loading="lazy" onerror="if(this.dataset.fallbackJpg){this.onerror=null;this.src=this.dataset.fallbackJpg;}"></picture>`;
    }
    return `<img src="${escapedSrc}" alt="${escapedAlt}" loading="lazy">`;
  }

  function renderMarkdownText(text, basePath) {
    const lines = (text || "").split(/\r?\n/);
    const out = [];
    let listType = null;
    for (const line of lines) {
      const trimmed = line.trim();
      const orderedMatch = /^\d+\.\s+/.test(trimmed);
      const unorderedMatch = /^[-*+]\s+/.test(trimmed);
      if (orderedMatch || unorderedMatch) {
        const nextType = orderedMatch ? "ol" : "ul";
        if (listType && listType !== nextType) {
          out.push(`</${listType}>`);
          listType = null;
        }
        if (!listType) {
          out.push(`<${nextType}>`);
          listType = nextType;
        }
        const itemText = orderedMatch
          ? trimmed.replace(/^\d+\.\s+/, "")
          : trimmed.replace(/^[-*+]\s+/, "");
        out.push(`<li>${inlineMd(itemText, basePath)}</li>`);
        continue;
      }
      if (listType) {
        out.push(`</${listType}>`);
        listType = null;
      }
      if (trimmed.length === 0) {
        out.push("<br>");
        continue;
      }
      const headingMatch = /^(#{1,6})\s+/.exec(trimmed);
      if (headingMatch) {
        const level = headingMatch[1].length;
        out.push(
          `<h${level}>${inlineMd(trimmed.replace(/^#{1,6}\s+/, ""), basePath)}</h${level}>`
        );
        continue;
      }
      if (/^>\s?/.test(trimmed)) {
        out.push(`<blockquote>${inlineMd(trimmed.replace(/^>\s?/, ""), basePath)}</blockquote>`);
        continue;
      }
      out.push(`<p>${inlineMd(trimmed, basePath)}</p>`);
    }
    if (listType) out.push(`</${listType}>`);
    return out.join("");
  }

  const MUTABLE_STATUSES = new Set(["Pass", "Fail", "NA", "Other"]);

  function normalizeMutableStatus(status) {
    const trimmed = String(status || "").trim();
    return MUTABLE_STATUSES.has(trimmed) ? trimmed : "";
  }

  function indicatorFor(status, comment) {
    const normalizedStatus = normalizeMutableStatus(status);
    const trimmed = (comment || "").trim();
    switch (normalizedStatus) {
      case "Pass":
        return { symbol: "&#9989;", tooltip: "Meets the required spec.", cls: "state-saved" };
      case "Fail":
        if (!trimmed) {
          return { symbol: "&#10060;", tooltip: "Please update the status or add a comment.", cls: "state-unsaved" };
        }
        return { symbol: "&#9888;", tooltip: "Flagged: ensure the comment describes findings.", cls: "state-pending" };
      case "Other":
        if (!trimmed) {
          return { symbol: "&#9888;", tooltip: "If 'Other' is selected, add a comment.", cls: "state-pending" };
        }
        return { symbol: "&#127344;", tooltip: "Non-standard state: describe in the comment.", cls: "state-pending" };
      case "NA":
        return { symbol: "&#8788;", tooltip: "Not Applicable: omitted from the final report.", cls: "" };
      default:
        return { symbol: "", tooltip: "No valid status selected.", cls: "" };
    }
  }

  function isPrefillPredicateToken(predicate) {
    const token = String(predicate || "").trim();
    return token.includes("SearchPrefill");
  }

  function prefillIndicatorFor(status) {
    if (!status || !status.mode) {
      return { variant: "ind-muted", tooltip: "Prefill dataset status unavailable." };
    }
    const path = status.path ? `\n${status.path}` : "";
    const slugHint = status.matched_slug_id ? ` (${status.matched_slug_id})` : "";
    switch (status.mode) {
      case "address":
        return { variant: "ind-good", tooltip: `Prefill CSV: address_id.csv${path}` };
      case "slug":
        return { variant: "ind-good", tooltip: `Prefill CSV: slug_id.csv${slugHint}${path}` };
      case "slug_predecessor":
        return { variant: "ind-good", tooltip: `Prefill CSV: predecessor slug${slugHint}${path}` };
      case "slug_legacy":
        return { variant: "ind-good", tooltip: `Prefill CSV: legacy slug${slugHint}${path}` };
      case "slug_predecessor_legacy":
        return { variant: "ind-good", tooltip: `Prefill CSV: legacy predecessor${slugHint}${path}` };
      case "checklist":
        return { variant: "ind-warn", tooltip: `Prefill CSV: checklist fallback${path}` };
      case "missing":
        return { variant: "ind-bad", tooltip: "Prefill CSV missing." };
      default:
        return { variant: "ind-muted", tooltip: "Prefill dataset status unavailable." };
    }
  }

  function renderPrefillIndicator(status) {
    const indicator = prefillIndicatorFor(status);
    return `<span class="tiny-indicator prefill-indicator ${indicator.variant}" title="${escapeHtml(
      indicator.tooltip
    )}"></span>`;
  }

  function renderRelationships(list, emptyLabel, prefillStatus) {
    const label = emptyLabel || "No relationships.";
    if (!list || list.length === 0) {
      return `<div class="note">${escapeHtml(label)}</div>`;
    }
    return list
      .map((rel) => {
        const predicate = rel.predicate || "";
        const target = rel.target || "";
        const indicator =
          prefillStatus && isPrefillPredicateToken(predicate) ? renderPrefillIndicator(prefillStatus) : "";
        return `<div class="note">${indicator}${escapeHtml(predicate)} <span class="rel-arrow rel-arrow-outgoing" aria-hidden="true">&rarr;</span> ${escapeHtml(target)}</div>`;
      })
      .join("");
  }

  function renderIncomingRelationships(list, emptyLabel) {
    const label = emptyLabel || "No incoming relationships.";
    if (!list || list.length === 0) {
      return `<div class="note">${escapeHtml(label)}</div>`;
    }
    return list
      .map((rel) => {
        const predicate = rel.predicate || "";
        const source = rel.source || rel.target || "";
        return `<div class="note">${escapeHtml(predicate)} <span class="rel-arrow rel-arrow-incoming" aria-hidden="true">&larr;</span> ${escapeHtml(source)}</div>`;
      })
      .join("");
  }

  function createDraftRow(checklistName, sectionName, draftCounter) {
    return {
      uid: `draft-${draftCounter}`,
      addressId: "",
      slugId: "",
      checklist: checklistName || "",
      section: sectionName || "",
      action: "",
      spec: "",
      result: "",
      status: "",
      comment: "",
      instructions: "",
      procedure: "",
      entityId: "",
      instanceId: "",
      prefillDataset: null,
      relationships: [],
      incomingRelationships: [],
      templateRelationships: [],
      verifyIndicator: { variant: "ind-muted", tooltip: "Save row to enable Verify diagnostics." },
      isNew: true,
    };
  }

  function findRow(tableData, rowId) {
    for (const section of tableData) {
      const idx = section.rows.findIndex((r) => r.uid === rowId);
      if (idx !== -1) {
        return { section, sectionIndex: tableData.indexOf(section), rowIndex: idx, row: section.rows[idx] };
      }
    }
    return null;
  }

  function findRowByAddress(tableData, addressId) {
    for (const section of tableData) {
      const match = section.rows.find((r) => r.addressId === addressId);
      if (match) return match;
    }
    return null;
  }

  function checklistPrototypeInit(config) {
    let authState = loadAuthState();
    const hostInput = document.getElementById("hostInput");
    const portInput = document.getElementById("portInput");
    const healthBtn = document.getElementById("healthBtn");
    const healthStatus = document.getElementById("healthStatus");
    const checklistSelect = document.getElementById("checklistSelect");
    const checklistFilter = document.getElementById("checklistFilter");
    const checklistStatus = document.getElementById("checklistStatus");
    const checklistCopyBtn = document.getElementById("checklistCopyBtn");
    const checklistDeleteBtn = document.getElementById("checklistDeleteBtn");
    const mdWorkspaceStatus = document.getElementById("mdWorkspaceStatus");
    const mdWorkspaceSelect = document.getElementById("mdWorkspaceSelect");
    const mdWorkspaceInfo = document.getElementById("mdWorkspaceInfo");
    const refreshMdWorkspaceBtn = document.getElementById("refreshMdWorkspaceBtn");
    const openMdWorkspaceDirBtn = document.getElementById("openMdWorkspaceDirBtn");
    const copyMdWorkspaceDirBtn = document.getElementById("copyMdWorkspaceDirBtn");
    const importMdWorkspaceBtn = document.getElementById("importMdWorkspaceBtn");
    const mdWorkspaceApplyData = document.getElementById("mdWorkspaceApplyData");
    const mdWorkspaceReplaceInstance = document.getElementById("mdWorkspaceReplaceInstance");
    const exportMdWorkspaceBtn = document.getElementById("exportMdWorkspaceBtn");
    const mdWorkspaceExportIncludeData = document.getElementById("mdWorkspaceExportIncludeData");
    const mdWorkspaceExportPack = document.getElementById("mdWorkspaceExportPack");
    const jsonlImportFile = document.getElementById("jsonlImportFile");
    const importJsonlBtn = document.getElementById("importJsonlBtn");
    const jsonlImportStatus = document.getElementById("jsonlImportStatus");
    const jsonlImportInfo = document.getElementById("jsonlImportInfo");
    const scriptsStatus = document.getElementById("scriptsStatus");
    const scriptsSelect = document.getElementById("scriptsSelect");
    const scriptsInfo = document.getElementById("scriptsInfo");
    const scriptsRunInfo = document.getElementById("scriptsRunInfo");
    const refreshScriptsBtn = document.getElementById("refreshScriptsBtn");
    const openScriptsDirBtn = document.getElementById("openScriptsDirBtn");
    const copyScriptsDirBtn = document.getElementById("copyScriptsDirBtn");
    const runScriptBtn = document.getElementById("runScriptBtn");
    const stopScriptBtn = document.getElementById("stopScriptBtn");
    const oauthLoginBtn = document.getElementById("oauthLoginBtn");
    const oauthLogoutBtn = document.getElementById("oauthLogoutBtn");
    const tokenStatusSpan = document.getElementById("tokenStatus");
    const tokenScopesSpan = document.getElementById("tokenScopes");
    const devEntityPanel = document.getElementById("devEntityPanel");
    const reportPath = document.getElementById("reportPath");
    const reportDir = document.getElementById("reportDir");
    const reportJsonlMode = document.getElementById("reportJsonlMode");
    const reportJsonlIncludePrincipals = document.getElementById("reportJsonlIncludePrincipals");
    const reportJsonlSlugOnly = document.getElementById("reportJsonlSlugOnly");
    const generateReportBtn = document.getElementById("generateReportBtn");
    const generateHtmlReportBtn = document.getElementById("generateHtmlReportBtn");
    const openReportDirBtn = document.getElementById("openReportDirBtn");
    const listBtn = document.getElementById("listBtn");
    const newChecklistBtn = document.getElementById("newChecklistBtn");
    const slugTableBody = document.getElementById("slugTableBody");
    const checklistView = document.getElementById("checklistView");
    const flowView = document.getElementById("flowView");
    const checklistViewTab = document.getElementById("checklistViewTab");
    const flowViewTab = document.getElementById("flowViewTab");
    const refreshFlowBtn = document.getElementById("refreshFlowBtn");
    const exportFlowBtn = document.getElementById("exportFlowBtn");
    const flowModeSelect = document.getElementById("flowModeSelect");
    const flowFilterInput = document.getElementById("flowFilterInput");
    const flowSummary = document.getElementById("flowSummary");
    const flowCanvas = document.getElementById("flowCanvas");
    const flowEdges = document.getElementById("flowEdges");
    const flowInspector = document.getElementById("flowInspector");
    const flowContextMenu = document.getElementById("flowContextMenu");
    const instanceSelect = document.getElementById("instanceSelect");
    const instanceFilter = document.getElementById("instanceFilter");
    const instanceCopyBtn = document.getElementById("instanceCopyBtn");
    const instanceStatus = document.getElementById("instanceStatus");
    const instanceDeleteBtn = document.getElementById("instanceDeleteBtn");
    const modeSelect = document.getElementById("modeSelect");
    const editToggle = document.getElementById("editToggle");
    const showNaRowsToggle = document.getElementById("showNaRowsToggle");
    const wrapRowTextToggle = document.getElementById("wrapRowTextToggle");
    const entityToggle = document.getElementById("entityToggle");
    const entityPanel = document.getElementById("entityPanel");
    const entitySelect = document.getElementById("entitySelect");
    const refreshEntitiesBtn = document.getElementById("refreshEntities");
    const useSelectedEntityBtn = document.getElementById("useSelectedEntity");
    const activeEntityIdSpan = document.getElementById("activeEntityId");
    const activeEntityPrincipalSpan = document.getElementById("activeEntityPrincipal");
    const entityPrincipalInput = document.getElementById("entityPrincipalInput");
    const entityDisplayInput = document.getElementById("entityDisplayInput");
    const simulateLoginBtn = document.getElementById("simulateLoginBtn");
    const modeHint = document.getElementById("modeHint");
    const variantTag = document.getElementById("variantTag");
    const saver = window.createSaveController ? window.createSaveController(setSaveState, saveRow, 1500, 5000) : null;
    const rootPrincipalConfig =
      (config?.templatePrincipal || config?.rootInstancePrincipal ||
        CONFIG.templatePrincipal || CONFIG.rootInstancePrincipal || "template||default").trim();
    const ROOT_PRINCIPAL = rootPrincipalConfig || "template||default";
    const ROOT_PRINCIPAL_LOWER = ROOT_PRINCIPAL.toLowerCase();
    const SHOW_INSTANCE_PRINCIPAL = true; // future security flag can disable principal display
    const HOST_KEY = "chaxHost";
    let lastReportDir = "";
    let lastReportPath = "";
    const PORT_KEY = "chaxPort";
    let rootInstanceId = "";
    let mdTemplatesRoot = "";
    let mdWorkspaceItems = [];
    let scriptsRoot = "";
    let scriptsManifestPath = "";
    let scriptsSourceName = "";
    let scriptsPack = "";
    let scriptsChecklist = "";
    let scriptItems = [];
    const scriptPidByKey = new Map();

    let allChecklists = [];
    let instanceOptions = [];
    let loadedChecklist = "";
    let loadedInstance = "";
    const instanceMeta = new Map(); // instance_id -> { principal }
    let tableData = [];
    const templateRelationshipsBySlug = new Map();
    let templateRelationshipsLoadedChecklist = "";
    let templateRelationshipsLoadingChecklist = "";
    let templateRelationshipsLoadingPromise = null;
    let sectionCounter = 0;
    let draftCounter = 1;
    let instantiating = false;
    let activeEntity = { id: "", principal: "" };
    let entities = [];
    let creatingInstanceFromFilter = false;
    let creatingChecklistFromFilter = false;
    let lastChecklistCreateDeniedAt = 0;
    let lastInstanceCreateAttemptAt = 0;
    let lastInstanceCreateAttemptValue = "";
    let lastInstanceNoTemplateAt = 0;
    let listSlugsInFlight = false;
    let autoRefreshTimer = null;
    let autoRefreshInFlight = false;
    let showNaRows = false;
    let wrapRowText = false;
    let activeView = "checklist";
    let flowGraph = null;
    let flowMode = "swimlanes";
    let flowSelectedMeta = null;
    let flowFocus = null;
    let flowDrilldown = null;
    let flowPreviousMode = null;
    let flowRenderedProjection = null;

    const state = {
      mode: config?.defaultMode || "user",
      editEnabled: config?.defaultEdit !== false,
    };
    const initialSelectionFromUrl = (() => {
      const params = new URLSearchParams(window.location.search || "");
      return {
        checklist: (params.get("checklist_id") || params.get("checklist") || "").trim(),
        instance: (params.get("instance_id") || params.get("instance") || "").trim(),
        tour: (params.get("tour") || "").trim(),
      };
    })();
    const autoRefreshMs = (() => {
      const raw = CONFIG.autoRefreshMs ?? config?.autoRefreshMs;
      const parsed = Number(raw);
      return Number.isFinite(parsed) && parsed > 0 ? parsed : 0;
    })();
    const verifyAutoRefreshMs = (() => {
      const raw = CONFIG.verifyAutoRefreshMs ?? config?.verifyAutoRefreshMs;
      if (raw === undefined || raw === null || raw === "") return 15000;
      const parsed = Number(raw);
      return Number.isFinite(parsed) && parsed >= 0 ? parsed : 15000;
    })();
    const autoRefreshPauseAfterInteractionMs = (() => {
      const raw = CONFIG.autoRefreshPauseAfterInteractionMs ?? config?.autoRefreshPauseAfterInteractionMs;
      if (raw === undefined || raw === null || raw === "") return 1200;
      const parsed = Number(raw);
      return Number.isFinite(parsed) && parsed >= 0 ? parsed : 1200;
    })();
    const largeChecklistWarnThreshold = (() => {
      const raw = CONFIG.largeChecklistWarnThreshold ?? config?.largeChecklistWarnThreshold;
      if (raw === undefined || raw === null || raw === "") return 800;
      const parsed = Number(raw);
      return Number.isFinite(parsed) && parsed >= 0 ? parsed : 800;
    })();
    const deferVerifyRenderThreshold = (() => {
      const raw = CONFIG.deferVerifyRenderThreshold ?? config?.deferVerifyRenderThreshold;
      if (raw === undefined || raw === null || raw === "") return 500;
      const parsed = Number(raw);
      return Number.isFinite(parsed) && parsed >= 0 ? parsed : 500;
    })();
    const instanceSeedConcurrency = (() => {
      const raw = CONFIG.instanceSeedConcurrency ?? config?.instanceSeedConcurrency;
      if (raw === undefined || raw === null || raw === "") return 8;
      const parsed = Number(raw);
      if (!Number.isFinite(parsed) || parsed < 1) return 8;
      return Math.min(16, Math.max(1, Math.floor(parsed)));
    })();
    let lastUserInteractionAt = 0;
    let lastAutoVerifyRefreshAt = 0;
    let nextAutoRefreshAt = 0;
    if (window.localStorage?.getItem(SHOW_NA_ROWS_KEY) === "1") {
      showNaRows = true;
    }
    if (window.localStorage?.getItem(WRAP_ROW_TEXT_KEY) === "1") {
      wrapRowText = true;
    }

    if (variantTag && config?.variantLabel) {
      variantTag.textContent = config.variantLabel;
    }
    if (showNaRowsToggle) {
      showNaRowsToggle.checked = showNaRows;
    }
    if (wrapRowTextToggle) {
      wrapRowTextToggle.checked = wrapRowText;
    }

    instanceOptions = [getRootInstance()];
    loadedInstance = getRootInstance();

    if (hostInput && !hostInput.value) hostInput.value = window.localStorage?.getItem(HOST_KEY) || "127.0.0.1";
    if (portInput && !portInput.value) portInput.value = window.localStorage?.getItem(PORT_KEY) || "8080";

      async function loadInstanceCatalog() {
        try {
          const data = await fetchJson("/api/v1/instances");
          const items = data.items || [];
          items.forEach((inst) => {
            const id = inst.instance_id || inst.id || "";
            const principal = inst.principal || inst.instance_principal || "";
            if (id) recordPrincipal(id, principal);
            if (principal && principal.toLowerCase() === ROOT_PRINCIPAL_LOWER) {
              rootInstanceId = id;
            }
          });
        } catch {
          // Ignore catalog failures; fallback logic handles missing roots.
        }
      }

      async function loadInstancesForChecklist(checklistName) {
        instanceOptions = [];
        const selectedChecklist = (checklistName || "").trim();
        if (!selectedChecklist) {
          instanceOptions = [getRootInstance()];
          renderInstanceOptions(instanceFilter?.value || "");
          updateInstanceInputs(getRootInstance());
          return;
        }
        try {
          // Use the checklist endpoint (unpaged) so instance options are complete.
          const query = new URLSearchParams();
          addWorkspaceQueryContext(query, selectedChecklist);
          const suffix = query.toString() ? `?${query.toString()}` : "";
          const data = await fetchJson(`/api/v1/checklists/${encodeURIComponent(selectedChecklist)}${suffix}`);
          const items = data.items || [];
          const set = new Set();
          items.forEach((slug) => {
            const id = slug.instance_id || "";
            const principal = slug.instance_principal || "";
            if (id) set.add(id);
            if (id && principal) recordPrincipal(id, principal);
            if (!rootInstanceId && principal && principal.toLowerCase() === ROOT_PRINCIPAL_LOWER) {
              rootInstanceId = id;
            }
          });
          if (set.size === 0) {
            set.add(getRootInstance());
          }
          instanceOptions = Array.from(set);
        } catch (err) {
          instanceOptions = [getRootInstance()];
        }
        renderInstanceOptions(instanceFilter?.value || "");
        const desired =
          loadedInstance && instanceOptions.includes(loadedInstance) ? loadedInstance : getRootInstance();
        updateInstanceInputs(desired);
      }

    modeSelect.value = state.mode;
    if (editToggle) {
      editToggle.checked = state.editEnabled;
      editToggle.disabled = state.mode !== "service";
    }
    if (instanceSelect) instanceSelect.value = getRootInstance();
    renderInstanceOptions(instanceFilter?.value || "");
    updateInstanceInputs(getRootInstance());
    setActiveEntity(activeEntity.id, activeEntity.principal);

    function normalizeHostPort(rawHost, rawPort) {
      let host = (rawHost || "").trim();
      host = host.replace(/^https?:\/\//i, "");
      host = host.replace(/\/+$/, "");
      let port = (rawPort || "").trim();
      port = port.replace(/[^0-9]/g, "");
      return { host: host || "127.0.0.1", port: port || "8080" };
    }

    function baseUrl() {
      if (window.location.protocol.startsWith("http")) {
        return window.location.origin.replace(/\/+$/, "");
      }
      if (CONFIG.baseUrl) {
        return CONFIG.baseUrl.replace(/\/+$/, "");
      }
      const { host, port } = normalizeHostPort(
        hostInput?.value || window.localStorage?.getItem(HOST_KEY) || "127.0.0.1",
        portInput?.value || window.localStorage?.getItem(PORT_KEY) || "8080"
      );
      return `http://${host}${port ? `:${port}` : ""}`;
    }

    function isTokenValid() {
      if (!authState || !authState.access_token) return false;
      if (!authState.expires_in) return true;
      const expiresAt = (authState.obtained_at || 0) + authState.expires_in * 1000;
      return Date.now() + 5000 < expiresAt;
    }

    function setAuthState(newState) {
      authState = newState;
      saveAuthState(newState);
      updateSessionPanel();
    }

    async function fetchJson(path, options = {}) {
      const target = `${baseUrl()}${path}`;
      const headers = new Headers(options.headers || {});
      if (isTokenValid()) {
        headers.set("Authorization", `Bearer ${authState.access_token}`);
      }
      const resp = await fetch(target, { ...options, headers });
      const text = await resp.text();
      let parsed = {};
      if (text) {
        try {
          parsed = JSON.parse(text);
        } catch (err) {
          throw new Error(`Bad JSON from ${target}: ${err}`);
        }
      }
      if (resp.status === 401) {
        tokenStatusSpan && (tokenStatusSpan.textContent = "unauthenticated");
        throw new Error("Unauthorized");
      }
      if (!resp.ok || (parsed.ok === false)) {
        const errMsg = parsed.error ? `${parsed.error.code}: ${parsed.error.message}` : text || resp.status;
        throw new Error(errMsg);
      }
      return parsed.data !== undefined ? parsed.data : parsed;
    }

    async function fetchJsonWithWarnings(path, options = {}) {
      const target = `${baseUrl()}${path}`;
      const headers = new Headers(options.headers || {});
      if (isTokenValid()) {
        headers.set("Authorization", `Bearer ${authState.access_token}`);
      }
      const resp = await fetch(target, { ...options, headers });
      const text = await resp.text();
      let parsed = {};
      if (text) {
        try {
          parsed = JSON.parse(text);
        } catch (err) {
          throw new Error(`Bad JSON from ${target}: ${err}`);
        }
      }
      if (resp.status === 401) {
        tokenStatusSpan && (tokenStatusSpan.textContent = "unauthenticated");
        throw new Error("Unauthorized");
      }
      if (!resp.ok || (parsed.ok === false)) {
        const errMsg = parsed.error ? `${parsed.error.code}: ${parsed.error.message}` : text || resp.status;
        throw new Error(errMsg);
      }
      return {
        data: parsed.data !== undefined ? parsed.data : parsed,
        warnings: Array.isArray(parsed.warnings) ? parsed.warnings : [],
      };
    }

    let predicateCatalog = new Map(); // exact, case-sensitive token -> record
    let predicateCatalogLower = new Map(); // lower(token) -> record (for case-mismatch hints)
    const knownAddressIds = new Set();
    const relDrafts = new Map();
    const relValidationSeq = new Map();
    const addressExistCache = new Map();
    const verifyIndicatorByAddress = new Map();

    const PREDICATE_TOKEN_RE = /^[A-Za-z][A-Za-z0-9_]{0,127}$/;
    const CANONICAL_PREDICATE_RE =
      /^(Bool|pass|fail|other|na|Pass|Fail|Other|Na|NA|Result|Status|Comment|Section|Action|Spec|Procedure|Instructions|Timestamp)(Propagate|Sync|Verify|Search)(Validated|Implied|Assumed|AndGate|OrGate|Prefill)(Pass|Fail|Other|Na|NA|Result|Status|Comment|Section|Action|Spec|Procedure|Instructions|Timestamp)$/;
    const SLOT_PREDICATE_RE =
      /^(pass|fail|other|na|Pass|Fail|Other|Na|NA|Result|Status|Comment|Section|Action|Spec|Procedure|Instructions|Timestamp)(Search|Propagate)(Prefill|Validated)(Result|Status|Comment|Section|Action|Spec|Procedure|Instructions|Timestamp)$/;

    const SLOT_FIELDS = [
      { value: "Result", label: "Result" },
      { value: "Status", label: "Status" },
      { value: "Comment", label: "Comment" },
      { value: "Section", label: "Section" },
      { value: "Action", label: "Action" },
      { value: "Spec", label: "Spec" },
      { value: "Procedure", label: "Procedure" },
      { value: "Instructions", label: "Instructions" },
      { value: "Timestamp", label: "Timestamp" },
    ];
    const CANON_SUBJECT_STATES = [
      { value: "Bool", label: "Bool" },
      { value: "pass", label: "Pass" },
      { value: "fail", label: "Fail" },
      { value: "other", label: "Other" },
      { value: "na", label: "NA" },
    ].concat(SLOT_FIELDS);
    const CANON_RELATIONS = [
      { value: "Propagate", label: "Propagate" },
      { value: "Sync", label: "Sync" },
      { value: "Verify", label: "Verify" },
      { value: "Search", label: "Search" },
    ];
    const CANON_TYPES = [
      { value: "Validated", label: "Validated" },
      { value: "Implied", label: "Implied" },
      { value: "Assumed", label: "Assumed" },
      { value: "AndGate", label: "AndGate" },
      { value: "OrGate", label: "OrGate" },
      { value: "Prefill", label: "Prefill" },
    ];
    const CANON_OBJECT_STATES = [
      { value: "Pass", label: "Pass" },
      { value: "Fail", label: "Fail" },
      { value: "Other", label: "Other" },
      { value: "Na", label: "NA" },
    ].concat(SLOT_FIELDS);

    function buildCanonicalPredicateToken(subjectState, relation, type, objectState) {
      if (!subjectState || !relation || !type || !objectState) return "";
      return `${subjectState}${relation}${type}${objectState}`;
    }

    function parseCanonicalPredicateToken(token) {
      const raw = (token || "").trim();
      const match = raw.match(CANONICAL_PREDICATE_RE);
      if (!match) return null;
      const lowered = String(match[1] || "").toLowerCase();
      const normalizedSubject =
        lowered === "pass" || lowered === "fail" || lowered === "other" || lowered === "na"
          ? lowered
          : match[1];
      const normalizedObject = match[4] === "NA" ? "Na" : match[4];
      return {
        subjectState: normalizedSubject,
        relation: match[2],
        type: match[3],
        objectState: normalizedObject,
      };
    }
    function parseSlotPredicateToken(token) {
      const raw = (token || "").trim();
      const match = raw.match(SLOT_PREDICATE_RE);
      if (!match) return null;
      const relation = match[2];
      const type = match[3];
      const isValidPair =
        (relation === "Search" && type === "Prefill") ||
        (relation === "Propagate" && type === "Validated");
      if (!isValidPair) return null;
      let subjectState = match[1];
      const lowered = subjectState.toLowerCase();
      if (lowered === "pass" || lowered === "fail" || lowered === "other" || lowered === "na") {
        subjectState = lowered;
      }
      return {
        subjectState,
        relation,
        type,
        objectState: match[4],
      };
    }

    function isValidPredicateToken(token) {
      const raw = (token || "").trim();
      return PREDICATE_TOKEN_RE.test(raw);
    }

    function ensureDatalist(id) {
      let el = document.getElementById(id);
      if (!el) {
        el = document.createElement("datalist");
        el.id = id;
        document.body.appendChild(el);
      }
      return el;
    }

    function setTinyIndicator(el, variant, tooltip) {
      if (!el) return;
      el.className = `tiny-indicator ${variant || ""}`.trim();
      el.title = tooltip || "";
    }

    function defaultVerifyIndicator(tooltip) {
      return { variant: "ind-muted", tooltip: tooltip || "Verify diagnostics unavailable." };
    }

    function summarizeSelfVerifyIndicator(node) {
      const addressId = String(node?.address_id || "");
      const verify = Array.isArray(node?.verify) ? node.verify : [];
      const selfBoolVerify = verify.filter((item) => {
        const target = String(item?.target_address_id || "");
        const predicate = String(item?.predicate || "");
        return target === addressId && predicate.includes("BoolVerify");
      });
      if (!selfBoolVerify.length) {
        return defaultVerifyIndicator("No BoolVerify self-check diagnostics for this row.");
      }
      const indeterminate = selfBoolVerify.find(
        (item) => String(item?.predicate_bool || "").toLowerCase() === "indeterminate"
      );
      if (indeterminate) {
        const reason =
          String(indeterminate.reason || "").trim() ||
          String(indeterminate.reason_code || "").trim() ||
          "Spec/result comparison is indeterminate.";
        return {
          variant: "ind-warn",
          tooltip: `Verify warning: ${reason}`,
        };
      }
      const states = Array.from(
        new Set(
          selfBoolVerify
            .map((item) => String(item?.predicate_bool || "").toLowerCase())
            .filter((value) => value)
        )
      );
      const stateSummary = states.length ? states.join(", ") : "determinate";
      return {
        variant: "ind-good",
        tooltip: `Verify determinate (${stateSummary}).`,
      };
    }

    function setResultVerifyIndicatorCell(rowKey, indicator) {
      const el = document.querySelector(`.result-verify-ind[data-id='${rowKey}']`);
      if (!el) return;
      const resolved = indicator || defaultVerifyIndicator();
      setTinyIndicator(el, resolved.variant, resolved.tooltip);
    }

    async function refreshVerifyIndicatorsForAddresses(addressIds) {
      const targets = Array.from(
        new Set((addressIds || []).map((id) => String(id || "").trim()).filter((id) => id))
      );
      if (!targets.length) return;

      let nodeMap = new Map();
      try {
        const payload = await fetchJson("/api/v1/evaluate/graph", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ root_address_ids: targets }),
        });
        nodeMap = new Map((payload.nodes || []).map((node) => [String(node.address_id || ""), node]));
      } catch {
        // Keep muted indicators when diagnostics are unavailable.
      }

      targets.forEach((addressId) => {
        const node = nodeMap.get(addressId);
        const indicator = node ? summarizeSelfVerifyIndicator(node) : defaultVerifyIndicator();
        verifyIndicatorByAddress.set(addressId, indicator);
        const row = findRowByAddress(tableData, addressId);
        if (row) {
          row.verifyIndicator = indicator;
        }
        setResultVerifyIndicatorCell(addressId, indicator);
      });
    }

    async function refreshPredicateCatalog() {
      try {
        const items = [];
        let cursor = null;
        for (let i = 0; i < 20; i++) {
          const params = new URLSearchParams();
          params.set("limit", "250");
          if (cursor !== null && cursor !== undefined) {
            params.set("cursor", String(cursor));
          }
          const page = await fetchJson(`/api/v1/predicates?${params.toString()}`);
          (page.items || []).forEach((p) => items.push(p));
          cursor = page.next_cursor;
          if (cursor === null || cursor === undefined) break;
        }
        const byName = new Map();
        const byLower = new Map();
        items.forEach((p) => {
          const name = String(p.name || "").trim();
          if (!name) return;
          byName.set(name, p);
          byLower.set(name.toLowerCase(), p);
        });
        predicateCatalog = byName;
        predicateCatalogLower = byLower;
        updatePredicateDatalist();
      } catch {
        // ignore: predicates are advisory; relationship creation still works.
      }
    }

    function updatePredicateDatalist() {
      const el = ensureDatalist("predicateOptions");
      const canonical = [];
      CANON_SUBJECT_STATES.forEach((s) => {
        CANON_RELATIONS.forEach((r) => {
          CANON_TYPES.forEach((tp) => {
            CANON_OBJECT_STATES.forEach((o) => {
              canonical.push(buildCanonicalPredicateToken(s.value, r.value, tp.value, o.value));
            });
          });
        });
      });
      el.innerHTML = canonical.map((t) => `<option value="${escapeHtml(t)}"></option>`).join("");
    }

    updatePredicateDatalist();

    function rebuildAddressDatalist() {
      knownAddressIds.clear();
      const rows = [];
      tableData.forEach((section) => {
        section.rows.forEach((row) => {
          if (row.addressId) {
            knownAddressIds.add(row.addressId);
            const label = `${row.procedure || ""}${row.action ? ` — ${row.action}` : ""}`.trim();
            rows.push({ id: row.addressId, label });
          }
        });
      });
      const el = ensureDatalist("addressOptions");
      rows.sort((a, b) => a.id.localeCompare(b.id));
      el.innerHTML = rows
        .map((r) => `<option value="${escapeHtml(r.id)}">${escapeHtml(r.label || r.id)}</option>`)
        .join("");
    }

    async function checkAddressExists(addressId) {
      const normalized = normalizeBase32Id(addressId);
      const cached = addressExistCache.get(normalized);
      if (cached && Date.now() - cached.checkedAt < 30000) return cached.exists;
      try {
        await fetchJson(`/api/v1/slugs/${normalized}`);
        addressExistCache.set(normalized, { exists: true, checkedAt: Date.now() });
        return true;
      } catch (err) {
        const msg = (err && err.message) || "";
        if (msg.startsWith("NOT_FOUND:")) {
          addressExistCache.set(normalized, { exists: false, checkedAt: Date.now() });
          return false;
        }
        throw err;
      }
    }

    function getRelDraft(rowId) {
      if (!relDrafts.has(rowId)) {
        relDrafts.set(rowId, {
          predicate: "passPropagateValidatedPass",
          target: "",
          lastEdit: "slots",
          subjectState: "pass",
          relation: "Propagate",
          type: "Validated",
          objectState: "Pass",
        });
      }
      return relDrafts.get(rowId);
    }

    function predicateIndicator(predicate) {
      const token = String(predicate || "").trim();
      if (!token) return { variant: "ind-muted", tooltip: "Predicate token required." };
      if (!isValidPredicateToken(token)) {
        return {
          variant: "ind-bad",
          tooltip: "Invalid predicate token. Expected [A-Za-z][A-Za-z0-9_]{0,127} (ASCII, case-sensitive).",
        };
      }

      const canonical = parseCanonicalPredicateToken(token);
      const slot = canonical ? null : parseSlotPredicateToken(token);
      const exact = predicateCatalog.get(token);
      const status = String(exact?.status || "");
      const lowerMatch = predicateCatalogLower.get(token.toLowerCase());
      const caseMismatch = !exact && lowerMatch && String(lowerMatch.name || "") !== token;

      const parts = [];
      if (canonical) {
        const subjLabel = CANON_SUBJECT_STATES.find((s) => s.value === canonical.subjectState)?.label || canonical.subjectState;
        const objLabel = CANON_OBJECT_STATES.find((o) => o.value === canonical.objectState)?.label || canonical.objectState;
        parts.push(`Canonical: ${subjLabel} / ${canonical.relation} / ${canonical.type} / ${objLabel}`);
      } else if (slot) {
        const subjLabel = CANON_SUBJECT_STATES.find((s) => s.value === slot.subjectState)?.label || slot.subjectState;
        const objLabel = CANON_OBJECT_STATES.find((o) => o.value === slot.objectState)?.label || slot.objectState;
        parts.push(`Slot: ${subjLabel} / ${slot.relation} / ${slot.type} / ${objLabel}`);
      } else {
        parts.push("Non-canonical token (allowed).");
      }

      if (caseMismatch) {
        parts.push(`Case mismatch vs catalog: "${String(lowerMatch.name || "")}".`);
      } else if (exact) {
        if (status && status.toLowerCase() === "active") {
          parts.push("Catalog: registered (active).");
        } else if (status) {
          parts.push(`Catalog: status=${status}.`);
        } else {
          parts.push("Catalog: registered (missing status).");
        }
      } else {
        parts.push("Catalog: not registered (allowed).");
      }

      if (slot && slot.relation === "Propagate" && slot.type === "Validated") {
        const validTargets = new Set(["Result", "Comment", "Status"]);
        if (!validTargets.has(slot.objectState)) {
          parts.push("Invalid target: Propagate/Validated supports Result, Comment, or Status.");
          return { variant: "ind-warn", tooltip: parts.join(" ") };
        }
        if (slot.subjectState === "Instructions") {
          parts.push("Instructions source is ignored for Propagate/Validated.");
          return { variant: "ind-warn", tooltip: parts.join(" ") };
        }
      }

      if (exact && status.toLowerCase() === "active" && (canonical || slot)) {
        return { variant: "ind-good", tooltip: parts.join(" ") };
      }
      if (canonical || slot) {
        return { variant: "ind-info", tooltip: parts.join(" ") };
      }
      return { variant: "ind-warn", tooltip: parts.join(" ") };
    }

    function targetIndicator(rawTarget, row) {
      const raw = normalizeBase32Id(rawTarget);
      if (!raw) return { variant: "ind-muted", tooltip: "Target required.", resolved: "" };
      if (isValidBase32Id(raw, 32)) {
        return {
          variant: "ind-info",
          tooltip: "Address ID (instance-level relationship).",
          resolved: raw,
          kind: "address",
        };
      }
      if (isValidBase32Id(raw, 16)) {
        if (row?.instanceId && isValidBase32Id(row.instanceId, 16)) {
          return {
            variant: "ind-good",
            tooltip: "Slug ID shortcut; resolves to an Address ID for this instance.",
            resolved: raw + row.instanceId,
            kind: "slug",
          };
        }
        return { variant: "ind-bad", tooltip: "Slug ID entered, but this row has no valid instance_id.", resolved: "" };
      }
      return { variant: "ind-bad", tooltip: "Enter a 16-char Slug ID or 32-char Address ID (Base32).", resolved: "" };
    }

    function renderRelationshipsPanel(row, stateAllowed) {
      const rowId = row.uid;
      const draft = getRelDraft(rowId);
      const locked = !stateAllowed || !row.addressId;
      const help = !row.addressId
        ? "Save this row first to create relationships."
        : "Build a canonical predicate with the four dropdowns (or type a token). Target accepts a 32-char Address ID, or a 16-char Slug ID (auto-expands to same-instance address).";
      const incomingList = renderIncomingRelationships(
        row.incomingRelationships || [],
        "No incoming relationships."
      );
      const templateList = renderRelationships(
        row.templateRelationships || [],
        "No template relationships."
      );

      const renderOptions = (options, selected) =>
        options
          .map(
            (o) =>
              `<option value="${escapeHtml(o.value)}"${o.value === selected ? " selected" : ""}>${escapeHtml(o.label)}</option>`
          )
          .join("");
      return `
        <div class="relationships-list" data-row="${escapeHtml(rowId)}">${renderRelationships(
          row.relationships || [],
          null,
          row.prefillDataset
        )}</div>
        <div class="note note-soft"><span class="note-key">Incoming relationships</span> are view-only; author them on source rows as outgoing links. Not exported from this checklist file.</div>
        <div class="relationships-list incoming-relationships" data-row="${escapeHtml(rowId)}">${incomingList}</div>
        <div class="note note-soft"><span class="note-key">Template relationships</span> (slug-level).</div>
        <div class="relationships-list template-relationships" data-row="${escapeHtml(rowId)}">${templateList}</div>
        <div class="rel-editor edit-only" data-row="${escapeHtml(rowId)}">
          <div class="rel-editor-row">
            <span class="tiny-indicator ind-muted rel-predicate-ind" data-row="${escapeHtml(rowId)}" title=""></span>
            <select class="rel-slot rel-subject-state" data-row="${escapeHtml(rowId)}" ${locked ? "disabled" : ""}>
              <option value="">Subject</option>
              ${renderOptions(CANON_SUBJECT_STATES, draft.subjectState || "")}
            </select>
            <select class="rel-slot rel-relation" data-row="${escapeHtml(rowId)}" ${locked ? "disabled" : ""}>
              <option value="">Relation</option>
              ${renderOptions(CANON_RELATIONS, draft.relation || "")}
            </select>
            <select class="rel-slot rel-type" data-row="${escapeHtml(rowId)}" ${locked ? "disabled" : ""}>
              <option value="">Type</option>
              ${renderOptions(CANON_TYPES, draft.type || "")}
            </select>
            <select class="rel-slot rel-object-state" data-row="${escapeHtml(rowId)}" ${locked ? "disabled" : ""}>
              <option value="">Object</option>
              ${renderOptions(CANON_OBJECT_STATES, draft.objectState || "")}
            </select>
            <input class="rel-predicate" data-row="${escapeHtml(rowId)}" list="predicateOptions" placeholder="predicate_token" value="${escapeHtml(draft.predicate || "")}" ${locked ? "disabled" : ""}>
            <span class="tiny-indicator ind-muted rel-target-ind" data-row="${escapeHtml(rowId)}" title=""></span>
            <input class="rel-target" data-row="${escapeHtml(rowId)}" list="addressOptions" placeholder="target address_id or slug_id" value="${escapeHtml(draft.target || "")}" ${locked ? "disabled" : ""}>
            <button class="secondary rel-add" type="button" data-row="${escapeHtml(rowId)}" ${locked ? "disabled" : ""}>Add</button>
          </div>
          <div class="note rel-hint" data-row="${escapeHtml(rowId)}">${escapeHtml(help)}</div>
        </div>`;
    }

    async function loadTemplateRelationshipsForChecklist(checklistName) {
      templateRelationshipsBySlug.clear();
      const name = (checklistName || "").trim();
      if (!name) return;
      try {
        const params = new URLSearchParams();
        params.set("checklist", name);
        const payload = await fetchJson(`/api/v1/relationships/template?${params.toString()}`);
        const items = payload.items || [];
        items.forEach((rel) => {
          const subject = rel.subject_slug_id || "";
          if (!subject) return;
          if (!templateRelationshipsBySlug.has(subject)) {
            templateRelationshipsBySlug.set(subject, []);
          }
          templateRelationshipsBySlug.get(subject).push({
            predicate: rel.predicate || "",
            target: rel.target_slug_id || "",
          });
        });
      } catch {
        templateRelationshipsBySlug.clear();
      }
    }

    async function ensureTemplateRelationshipsLoaded(checklistName) {
      const name = (checklistName || "").trim();
      if (!name) return;
      if (templateRelationshipsLoadedChecklist === name) return;
      if (templateRelationshipsLoadingPromise && templateRelationshipsLoadingChecklist === name) {
        await templateRelationshipsLoadingPromise;
        return;
      }
      templateRelationshipsLoadingChecklist = name;
      templateRelationshipsLoadingPromise = (async () => {
        await loadTemplateRelationshipsForChecklist(name);
        templateRelationshipsLoadedChecklist = name;
      })().finally(() => {
        if (templateRelationshipsLoadingChecklist === name) {
          templateRelationshipsLoadingChecklist = "";
        }
        templateRelationshipsLoadingPromise = null;
      });
      await templateRelationshipsLoadingPromise;
    }

    async function fetchEntities() {
      try {
        const data = await fetchJson("/api/v1/entities");
        entities = data.items || data.entities || [];
      } catch {
        entities = [];
      }
      renderEntityOptions();
      if (!activeEntity.id && entities.length > 0) {
        const first = entities[0];
        setActiveEntity(first.entity_id || first.id || "", first.principal || "");
      }
    }

    function setMdWorkspaceStatus(state, tooltip) {
      if (!mdWorkspaceStatus) return;
      const colors = { good: "#7bd58c", idle: "#f6d36b", warn: "#f2b35a", bad: "#e57f7f" };
      mdWorkspaceStatus.textContent = "\u25CF";
      mdWorkspaceStatus.style.color = colors[state] || colors.idle;
      mdWorkspaceStatus.title = tooltip || "";
    }

    function setJsonlImportStatus(state, tooltip) {
      if (!jsonlImportStatus) return;
      const colors = { good: "#7bd58c", idle: "#f6d36b", warn: "#f2b35a", bad: "#e57f7f" };
      jsonlImportStatus.textContent = "\u25CF";
      jsonlImportStatus.style.color = colors[state] || colors.idle;
      jsonlImportStatus.title = tooltip || "";
    }

    function setScriptsStatus(state, tooltip) {
      if (!scriptsStatus) return;
      const colors = { good: "#7bd58c", idle: "#f6d36b", warn: "#f2b35a", bad: "#e57f7f" };
      scriptsStatus.textContent = "\u25CF";
      scriptsStatus.style.color = colors[state] || colors.idle;
      scriptsStatus.title = tooltip || "";
    }

    function renderScriptsOptions(items) {
      if (!scriptsSelect) return;
      scriptsSelect.innerHTML = "";
      const sorted = (items || []).slice().sort((a, b) =>
        String(a.label || a.id || "").localeCompare(String(b.label || b.id || ""))
      );
      sorted.forEach((item) => {
        const opt = document.createElement("option");
        opt.value = item.id || "";
        const status = item.valid === false ? "INVALID" : item.enabled === false ? "DISABLED" : "READY";
        opt.textContent = `${item.label || item.id || "(unnamed)"} (${status})`;
        scriptsSelect.appendChild(opt);
      });
      if (sorted.length === 0) {
        const opt = document.createElement("option");
        opt.value = "";
        opt.textContent = "(no scripts found)";
        scriptsSelect.appendChild(opt);
      }
    }

    function currentScriptEntry() {
      const selectedId = (scriptsSelect?.value || "").trim();
      if (!selectedId) return null;
      return (scriptItems || []).find((item) => (item.id || "") === selectedId) || null;
    }

    function scriptEntryKey(entry) {
      if (!entry) return "";
      const checklist = scriptsChecklist || checklistSelect?.value || loadedChecklist || "";
      return `${scriptsSourceName || ""}::${scriptsPack || ""}::${checklist}::${entry.id || ""}`;
    }

    function resolveScriptsContext(checklistName) {
      const name = (checklistName || "").trim();
      if (!name) return { sourceName: "", pack: "", checklistDir: "" };
      const current = currentMdWorkspaceItem();
      const currentName = current ? (current.parsed_checklist || current.checklist || "") : "";
      if (current && currentName === name && current.pack) {
        return { sourceName: current.source_name || "", pack: current.pack || "", checklistDir: current.checklist || "" };
      }
      const byParsed = (mdWorkspaceItems || []).filter(
        (item) => (item.parsed_checklist || item.checklist || "") === name
      );
      if (byParsed.length === 1) {
        return { sourceName: byParsed[0].source_name || "", pack: byParsed[0].pack || "", checklistDir: byParsed[0].checklist || "" };
      }
      const byFolder = (mdWorkspaceItems || []).filter((item) => (item.checklist || "") === name);
      if (byFolder.length === 1) {
        return { sourceName: byFolder[0].source_name || "", pack: byFolder[0].pack || "", checklistDir: byFolder[0].checklist || "" };
      }
      return { sourceName: "", pack: "", checklistDir: "" };
    }

    function addWorkspaceQueryContext(params, checklistName) {
      const ctx = resolveScriptsContext(checklistName);
      if (ctx.sourceName) params.set("source_name", ctx.sourceName);
      if (ctx.pack) params.set("pack", ctx.pack);
      if (ctx.checklistDir) params.set("checklist_dir", ctx.checklistDir);
      return ctx;
    }

    function updateScriptsInfo() {
      if (!scriptsInfo) return;
      const parts = [];
      parts.push(`Checklist: ${scriptsChecklist || (checklistSelect?.value || loadedChecklist || "")}`);
      parts.push(`Source: ${scriptsSourceName || "(auto)"}`);
      parts.push(`Pack: ${scriptsPack || "(auto)"}`);
      parts.push(`Scripts folder: ${scriptsRoot || "(not loaded)"}`);
      if (scriptsManifestPath) {
        parts.push(`Manifest: ${scriptsManifestPath}`);
      }
      const selected = currentScriptEntry();
      if (selected) {
        parts.push("---");
        parts.push(`Selected: ${selected.label || selected.id || ""}`);
        parts.push(`ID: ${selected.id || ""}`);
        if (selected.rel_path) parts.push(`File: ${selected.rel_path}`);
        if (selected.command) parts.push(`Command override: ${selected.command}`);
        if (Array.isArray(selected.args) && selected.args.length > 0) {
          parts.push(`Manifest args: ${selected.args.join(" ")}`);
        }
        if (Array.isArray(selected.start_args) && selected.start_args.length > 0) {
          parts.push(`Start args: ${selected.start_args.join(" ")}`);
        }
        if (Array.isArray(selected.stop_args) && selected.stop_args.length > 0) {
          parts.push(`Stop args: ${selected.stop_args.join(" ")}`);
        }
        parts.push(`Enabled: ${selected.enabled === false ? "false" : "true"}`);
        parts.push(`Valid: ${selected.valid === false ? "false" : "true"}`);
        if (selected.error) parts.push(`Error: ${selected.error}`);
        if (selected.description) parts.push(`Description: ${selected.description}`);
        const pid = scriptPidByKey.get(scriptEntryKey(selected));
        if (pid) parts.push(`Last PID: ${pid}`);
      }
      scriptsInfo.textContent = parts.join("\n");
      if (stopScriptBtn) {
        stopScriptBtn.disabled = !selected || !Array.isArray(selected.stop_args) || selected.stop_args.length === 0;
      }
    }

    async function refreshScripts() {
      const checklist = (checklistSelect?.value || loadedChecklist || "").trim();
      if (!checklist) {
        scriptsRoot = "";
        scriptsManifestPath = "";
        scriptsSourceName = "";
        scriptsPack = "";
        scriptsChecklist = "";
        scriptItems = [];
        renderScriptsOptions([]);
        setScriptsStatus("idle", "Select a checklist to inspect scripts.");
        if (scriptsInfo) scriptsInfo.textContent = "";
        if (stopScriptBtn) stopScriptBtn.disabled = true;
        return;
      }

      const scriptsContext = resolveScriptsContext(checklist);
      const params = new URLSearchParams();
      params.set("checklist", checklist);
      if (scriptsContext.sourceName) {
        params.set("source_name", scriptsContext.sourceName);
      }
      if (scriptsContext.pack) {
        params.set("pack", scriptsContext.pack);
      }
      try {
        const data = await fetchJson(`/api/v1/workspace/scripts?${params.toString()}`);
        scriptsChecklist = data.checklist || checklist;
        scriptsSourceName = data.source_name || scriptsContext.sourceName || "";
        scriptsPack = data.pack || scriptsContext.pack || "";
        scriptsRoot = data.scripts_root || "";
        scriptsManifestPath = data.manifest_path || "";
        scriptItems = Array.isArray(data.items) ? data.items : [];
        renderScriptsOptions(scriptItems);
        updateScriptsInfo();

        const invalidCount = scriptItems.filter((item) => item && item.valid === false).length;
        if (data.valid === false) {
          setScriptsStatus("warn", data.error || "Scripts folder is not ready.");
        } else if (!scriptItems.length) {
          setScriptsStatus("idle", "No scripts found in checklist scripts folder.");
        } else if (invalidCount > 0) {
          setScriptsStatus("warn", `${invalidCount} script entries are invalid.`);
        } else {
          setScriptsStatus("good", `${scriptItems.length} script(s) ready.`);
        }
      } catch (err) {
        scriptsRoot = "";
        scriptsManifestPath = "";
        scriptsSourceName = "";
        scriptItems = [];
        renderScriptsOptions([]);
        setScriptsStatus("bad", `Failed to load scripts: ${err.message}`);
        if (scriptsInfo) scriptsInfo.textContent = `Scripts load failed: ${err.message}`;
        if (stopScriptBtn) stopScriptBtn.disabled = true;
      }
    }

    async function runSelectedScript() {
      const script = currentScriptEntry();
      if (!script) {
        alert("Select a script first.");
        return;
      }
      const checklist = (scriptsChecklist || checklistSelect?.value || loadedChecklist || "").trim();
      if (!checklist) {
        alert("Select a checklist first.");
        return;
      }
      const payload = {
        checklist,
        source_name: scriptsSourceName || "",
        pack: scriptsPack || "",
        script_id: script.id || "",
      };
      if (Array.isArray(script.start_args) && script.start_args.length > 0) {
        payload.args = script.start_args.slice();
      }
      const instanceId = (instanceSelect?.value || loadedInstance || "").trim();
      if (instanceId) {
        payload.instance_id = instanceId;
        const principal = resolveInstancePrincipal(instanceId);
        if (principal) {
          payload.instance_principal = principal;
        }
      }
      try {
        const result = await fetchJson("/api/v1/workspace/scripts/run", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
        const lines = [];
        lines.push(`Script: ${result.script_id || script.id || ""}`);
        lines.push(`Status: ${result.status || (result.dry_run ? "dry-run" : "requested")}`);
        if (result.pid) lines.push(`PID: ${result.pid}`);
        if (result.executable) lines.push(`Executable: ${result.executable}`);
        if (Array.isArray(result.args)) lines.push(`Args: ${result.args.join(" ")}`);
        if (result.stdout_log) lines.push(`stdout: ${result.stdout_log}`);
        if (result.stderr_log) lines.push(`stderr: ${result.stderr_log}`);
        if (result.pid) {
          scriptPidByKey.set(scriptEntryKey(script), result.pid);
        }
        if (scriptsRunInfo) scriptsRunInfo.textContent = lines.join("\n");
        setScriptsStatus("good", `Launched ${script.id || script.label || "script"}.`);
        updateScriptsInfo();
      } catch (err) {
        if (scriptsRunInfo) scriptsRunInfo.textContent = `Run failed: ${err.message}`;
        setScriptsStatus("bad", `Script run failed: ${err.message}`);
        alert(`Script run failed: ${err.message}`);
      }
    }

    async function stopSelectedScript() {
      const script = currentScriptEntry();
      if (!script) {
        alert("Select a script first.");
        return;
      }
      if (!Array.isArray(script.stop_args) || script.stop_args.length === 0) {
        alert("Selected script does not define stop_args in scripts.json.");
        return;
      }
      const checklist = (scriptsChecklist || checklistSelect?.value || loadedChecklist || "").trim();
      if (!checklist) {
        alert("Select a checklist first.");
        return;
      }
      const payload = {
        checklist,
        source_name: scriptsSourceName || "",
        pack: scriptsPack || "",
        script_id: script.id || "",
        args: script.stop_args.slice(),
      };
      const instanceId = (instanceSelect?.value || loadedInstance || "").trim();
      if (instanceId) {
        payload.instance_id = instanceId;
        const principal = resolveInstancePrincipal(instanceId);
        if (principal) {
          payload.instance_principal = principal;
        }
      }
      try {
        const result = await fetchJson("/api/v1/workspace/scripts/run", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
        const lines = [];
        lines.push(`Script: ${result.script_id || script.id || ""}`);
        lines.push("Status: stop requested");
        if (result.pid) lines.push(`PID: ${result.pid}`);
        if (Array.isArray(result.args)) lines.push(`Args: ${result.args.join(" ")}`);
        if (result.stdout_log) lines.push(`stdout: ${result.stdout_log}`);
        if (result.stderr_log) lines.push(`stderr: ${result.stderr_log}`);
        if (scriptsRunInfo) scriptsRunInfo.textContent = lines.join("\n");
        setScriptsStatus("warn", `Stop requested for ${script.id || script.label || "script"}.`);
      } catch (err) {
        if (scriptsRunInfo) scriptsRunInfo.textContent = `Stop failed: ${err.message}`;
        setScriptsStatus("bad", `Script stop failed: ${err.message}`);
        alert(`Script stop failed: ${err.message}`);
      }
    }

    function openScriptsDir() {
      if (!scriptsRoot) {
        alert("Scripts folder not loaded yet. Click Refresh first.");
        return;
      }
      const normalized = scriptsRoot.replace(/\\/g, "/");
      const url = "file:///" + encodeURI(normalized);
      window.open(url, "_blank");
    }

    function mdWorkspaceKey(item) {
      const source = item?.source_name || "";
      const pack = item?.pack || "";
      const checklist = item?.checklist || "";
      return `${source}::${pack}::${checklist}`;
    }

    function parseMdWorkspaceKey(value) {
      const parts = String(value || "").split("::");
      if (parts.length === 3) {
        return { sourceName: parts[0], pack: parts[1], checklist: parts[2] };
      }
      if (parts.length === 2) {
        return { sourceName: "", pack: parts[0], checklist: parts[1] };
      }
      return { sourceName: "", pack: "", checklist: value };
    }

    function renderMdWorkspaceOptions(items) {
      if (!mdWorkspaceSelect) return;
      mdWorkspaceSelect.innerHTML = "";
      const sorted = (items || []).slice().sort((a, b) =>
        mdWorkspaceKey(a).localeCompare(mdWorkspaceKey(b))
      );
      sorted.forEach((item) => {
        const opt = document.createElement("option");
        opt.value = mdWorkspaceKey(item);
        const status = item.valid ? "OK" : "INVALID";
        const source = item.source_name || "public";
        const label = item.pack ? `${source}/${item.pack}/${item.checklist}` : `${source}/${item.checklist || ""}`;
        if (item.duplicate_checklist_id) {
          opt.title = "Duplicate checklist id across configured sources; pick source and pack deliberately.";
        }
        opt.textContent = `${label} (${status})`;
        mdWorkspaceSelect.appendChild(opt);
      });
      if (sorted.length === 0) {
        const opt = document.createElement("option");
        opt.value = "";
        opt.textContent = "(no markdown templates found)";
        mdWorkspaceSelect.appendChild(opt);
      }
    }

    function currentMdWorkspaceItem() {
      const value = (mdWorkspaceSelect?.value || "").trim();
      if (!value) return null;
      const parsed = parseMdWorkspaceKey(value);
      return (
        (mdWorkspaceItems || []).find(
          (i) =>
            (i.source_name || "") === parsed.sourceName &&
            i.pack === parsed.pack &&
            i.checklist === parsed.checklist
        ) || null
      );
    }

    function buildLibraryBasePath(item) {
      if (item?.source_primary === false) return "";
      const pack = item?.pack || "";
      const checklist = item?.checklist || "";
      if (!pack || !checklist) return "";
      return `/checklists/${encodeURIComponent(pack)}/${encodeURIComponent(checklist)}/`;
    }

    function resolveLibraryAssetBase(checklistName) {
      const name = (checklistName || "").trim();
      if (!name) return "";
      const current = currentMdWorkspaceItem();
      const currentName = current ? (current.parsed_checklist || current.checklist || "") : "";
      if (current && currentName === name) {
        return buildLibraryBasePath(current);
      }
      const matches = (mdWorkspaceItems || []).filter(
        (item) => (item.parsed_checklist || item.checklist || "") === name
      );
      if (matches.length === 1) return buildLibraryBasePath(matches[0]);
      const folderMatches = (mdWorkspaceItems || []).filter((item) => (item.checklist || "") === name);
      if (folderMatches.length === 1) return buildLibraryBasePath(folderMatches[0]);
      return "";
    }

    function updateMdWorkspaceInfo() {
      const item = currentMdWorkspaceItem();
      if (!item) {
        setMdWorkspaceStatus("idle", "Select a markdown template file.");
        if (mdWorkspaceInfo) mdWorkspaceInfo.textContent = "";
        return;
      }
      const parts = [];
      parts.push(`Source: ${item.source_name || "public"}`);
      if (item.source_path) parts.push(`Source path: ${item.source_path}`);
      parts.push(`Pack: ${item.pack || ""}`);
      parts.push(`Checklist folder: ${item.checklist || ""}`);
      if (item.parsed_checklist) parts.push(`Parsed checklist: ${item.parsed_checklist}`);
      if (item.rel_path) parts.push(`File: ${item.rel_path}`);
      parts.push(`Bytes: ${item.bytes || 0}`);
      if (item.valid) {
        parts.push(`Valid: true`);
        parts.push(`Checklist: ${item.parsed_checklist || item.checklist || ""}`);
        parts.push(`Rows: ${item.slugs || 0}`);
        parts.push(`Template relationships: ${item.template_relationships || 0}`);
        const warnings = Array.isArray(item.warnings) ? item.warnings : [];
        if (warnings.length > 0) {
          const codes = warnings.map((w) => w.code || "warning").join(", ");
          parts.push(`Warnings: ${warnings.length} (${codes})`);
          setMdWorkspaceStatus("warn", warnings[0]?.message || "Parsed with warnings.");
        } else {
          setMdWorkspaceStatus("good", "Parsed successfully.");
        }
      } else {
        parts.push(`Valid: false`);
        parts.push(`Error: ${item.error || "(unknown error)"}`);
        setMdWorkspaceStatus("bad", item.error || "Invalid markdown template.");
      }
      if (mdWorkspaceInfo) mdWorkspaceInfo.textContent = parts.join("\n");
    }

    async function refreshMdWorkspace() {
      try {
        const data = await fetchJson("/api/v1/workspace/markdown/templates");
        mdTemplatesRoot = data.templates_root || "";
        mdWorkspaceItems = data.items || [];
        renderMdWorkspaceOptions(mdWorkspaceItems);
        updateMdWorkspaceInfo();
        syncMarkdownFields();
        if (mdWorkspaceSelect && mdWorkspaceSelect.options.length > 0) {
          mdWorkspaceSelect.disabled = false;
        }
        setMdWorkspaceStatus("idle", mdTemplatesRoot ? `Templates root: ${mdTemplatesRoot}` : "Templates root not reported.");
        refreshScripts();
      } catch (err) {
        mdTemplatesRoot = "";
        mdWorkspaceItems = [];
        renderMdWorkspaceOptions([]);
        setMdWorkspaceStatus("bad", `Failed to load workspace templates: ${err.message}`);
        if (mdWorkspaceInfo) mdWorkspaceInfo.textContent = "";
        refreshScripts();
      }
    }

    async function bootstrapInitialSetupChecklist() {
      if (initialSelectionFromUrl.checklist !== "initial_setup") return;
      if ((allChecklists || []).includes("initial_setup")) return;
      try {
        await fetchJson("/api/v1/workspace/markdown/import", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            source_name: "public",
            pack: "chax",
            checklist_dir: "initial_setup",
            checklist: "initial_setup",
            instance_principal: ROOT_PRINCIPAL,
            replace_instance: true,
          }),
        });
        await loadChecklists();
      } catch (err) {
        console.warn("initial setup import failed", err);
        setChecklistStatus("error", `Initial setup template could not be imported: ${err.message}`);
      }
    }

    function showInitialSetupTour() {
      if (initialSelectionFromUrl.tour !== "initial_setup") return;
      if (!instanceFilter) return;
      if (document.querySelector(".tour-bubble")) return;
      const bubble = document.createElement("div");
      bubble.className = "tour-bubble";
      bubble.innerHTML =
        'Type <code>startup</code> here. If no matching instance exists, Checklist Assistant will create it and use this checklist to store your first local settings.';
      document.body.appendChild(bubble);
      instanceFilter.classList.add("tour-target");
      const place = () => {
        const rect = instanceFilter.getBoundingClientRect();
        const left = Math.min(Math.max(12, rect.left), Math.max(12, window.innerWidth - 340));
        bubble.style.left = `${left}px`;
        bubble.style.top = `${Math.min(rect.bottom + 14, window.innerHeight - bubble.offsetHeight - 12)}px`;
      };
      const dismiss = () => {
        instanceFilter.classList.remove("tour-target");
        bubble.remove();
        window.removeEventListener("resize", place);
      };
      place();
      window.addEventListener("resize", place);
      bubble.addEventListener("click", dismiss);
      instanceFilter.addEventListener("input", () => {
        if ((instanceFilter.value || "").trim().toLowerCase() === "startup") {
          window.setTimeout(dismiss, 900);
        }
      });
      instanceFilter.focus();
    }

    function openMdWorkspaceDir() {
      if (!mdTemplatesRoot) {
        alert("Templates folder not loaded yet. Click Refresh first.");
        return;
      }
      const normalized = mdTemplatesRoot.replace(/\\/g, "/");
      const url = "file:///" + encodeURI(normalized);
      window.open(url, "_blank");
    }

    async function importMdWorkspaceSelected() {
      const item = currentMdWorkspaceItem();
      if (!item) {
        alert("Select a markdown template file first.");
        return;
      }
      if (!item.valid) {
        const ok = confirm(`"${item.filename}" did not validate. Import anyway?\n\n${item.error || ""}`);
        if (!ok) return;
      }
      const selectedInstanceId =
        (instanceSelect?.value || loadedInstance || getRootInstance()).trim() || getRootInstance();
      const templatePrincipal = ROOT_PRINCIPAL;
      const applyData = !!mdWorkspaceApplyData?.checked;
      const replaceInstance = mdWorkspaceReplaceInstance ? !!mdWorkspaceReplaceInstance.checked : true;
      try {
        const payload = {
          pack: item.pack || "",
          source_name: item.source_name || "",
          checklist_dir: item.checklist || "",
          checklist: item.checklist || "",
          instance_principal: templatePrincipal,
          apply_data: applyData,
          replace_instance: replaceInstance,
        };
        if (applyData) {
          const applyPrincipal = resolveInstancePrincipal(selectedInstanceId);
          if (!applyPrincipal) {
            alert(`Instance principal unavailable for ${selectedInstanceId}.`);
            return;
          }
          payload.apply_instance_principal = applyPrincipal;
        }
        const result = await fetchJson("/api/v1/workspace/markdown/import", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
        await loadChecklists();
        if (checklistSelect && result.checklist) checklistSelect.value = result.checklist;
        await loadInstancesForChecklist(result.checklist || checklistSelect.value);
        const targetInstance = applyData ? (result.apply_instance_id || selectedInstanceId) : result.instance_id;
        if (targetInstance) updateInstanceInputs(targetInstance);
        await listSlugs();
        await refreshScripts();
        alert(`Imported "${item.checklist}" from ${item.source_name || "public"}/${item.pack}.`);
      } catch (err) {
        alert(`Workspace import failed: ${err.message}`);
      }
    }

    async function exportMdWorkspaceCurrent() {
      const checklist = (checklistSelect?.value || loadedChecklist || "").trim();
      const instance = (instanceSelect?.value || loadedInstance || "").trim();
      if (!checklist) {
        alert("Select a checklist first.");
        return;
      }
      const includeData = !!mdWorkspaceExportIncludeData?.checked;
      let pack = (mdWorkspaceExportPack?.value || "").trim();
      const item = currentMdWorkspaceItem();
      if (!pack) {
        pack = item?.pack || "";
      }
      const workspaceContext = resolveScriptsContext(checklist);
      try {
        const payload = {
          checklist,
          source_name: item?.source_name || workspaceContext.sourceName || "",
          pack,
          checklist_dir: item?.checklist || workspaceContext.checklistDir || "",
          include_data: includeData,
          instance_id: instance || "",
        };
        const result = await fetchJson("/api/v1/workspace/markdown/export", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
        await refreshMdWorkspace();
        if (mdWorkspaceInfo) {
          mdWorkspaceInfo.textContent =
            `Exported checklist "${result.checklist}" to ${result.path}\nSource: ${result.source_name || ""}\nBytes: ${result.bytes || 0}`;
        }
        alert("Export complete.");
      } catch (err) {
        alert(`Workspace export failed: ${err.message}`);
      }
    }

    async function generateReport(checklistName, instanceId, format = "tex") {
      const checklist = (checklistName || checklistSelect.value || "").trim();
      const instance = (instanceId || instanceSelect?.value || loadedInstance || "").trim();
      if (!checklist) {
        alert("Checklist name is required.");
        return;
      }
      if (!instance) {
        alert("Instance id is required.");
        return;
      }
        try {
          const jsonlMode = reportJsonlMode ? reportJsonlMode.value : "report";
          const includePrincipals =
            reportJsonlIncludePrincipals ? reportJsonlIncludePrincipals.checked : false;
          const slugOnly = reportJsonlSlugOnly ? reportJsonlSlugOnly.checked : false;
          const workspaceContext = resolveScriptsContext(checklist);
          const payload = {
            checklist,
            source_name: workspaceContext.sourceName || "",
            pack: workspaceContext.pack || "",
            checklist_dir: workspaceContext.checklistDir || "",
            instance_id: instance,
            format,
            jsonl_mode: jsonlMode,
            jsonl_include_principals: includePrincipals,
            jsonl_slug_only: slugOnly,
          };
          const result = await fetchJsonWithWarnings("/api/v1/export/report", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
        const data = result.data || {};
        lastReportPath = data.path || "";
        lastReportDir = data.directory || "";
        const generatedFormat = String(data.format || format || "tex").toUpperCase();
        if (reportPath) reportPath.textContent = `File (${generatedFormat}): ${lastReportPath}`;
        if (reportDir) reportDir.textContent = `Folder: ${lastReportDir}`;
        const warnings = result.warnings || [];
        if (warnings.length === 0) {
          alert(`${generatedFormat} report generated.`);
          return;
        }
        const legacy = warnings.filter((w) => w && w.code === "LEGACY_TEMPLATE_SLUG");
        if (legacy.length > 0) {
          alert(
            "Warning: legacy template slug(s) were mapped to the latest slug.\n" +
              `${generatedFormat} report generated. Update the report template if this is undesired.\n` +
              "Auto-mapping can be disabled via use_latest_slug_lineage=false."
          );
          return;
        }
        const summary = warnings
          .slice(0, 5)
          .map((w) => `${w.code || "warning"}: ${w.message || ""}`.trim())
          .join("\n");
        alert(`${generatedFormat} report generated with warnings:\n${summary}`);
      } catch (err) {
        const requestedFormat = String(format || "tex").toUpperCase();
        alert(`${requestedFormat} report generation failed: ${err.message}`);
      }
    }

    function openReportDir() {
      if (!lastReportDir) {
        alert("No report folder available yet. Generate a report first.");
        return;
      }
      const normalized = lastReportDir.replace(/\\/g, "/");
      const url = "file:///" + encodeURI(normalized);
      window.open(url, "_blank");
    }

    function getRootInstance() {
      return rootInstanceId || ROOT_PRINCIPAL;
    }

    function addTemplateInstanceQuery(params) {
      if (rootInstanceId) {
        params.set("instance_id", rootInstanceId);
        return;
      }
      params.set("instance_principal", ROOT_PRINCIPAL);
    }

    function isRootInstance(value) {
      const val = value || "";
      const rootVal = getRootInstance();
      return val === rootVal || (!rootInstanceId && val === ROOT_PRINCIPAL);
    }

    function setChecklistStatus(state, tooltip) {
      if (!checklistStatus) return;
      const colors = { loaded: "#7bd58c", idle: "#f6d36b", error: "#e57f7f" };
      checklistStatus.textContent = "●";
      checklistStatus.style.color = colors[state] || colors.idle;
      checklistStatus.title = tooltip || "";
    }

    function setInstanceStatus(state, tooltip) {
      if (!instanceStatus) return;
      const colors = { loaded: "#7bd58c", idle: "#f6d36b", warn: "#f2b35a", error: "#e57f7f" };
      instanceStatus.textContent = state === "warn" ? "⚠" : "●";
      instanceStatus.style.color = colors[state] || colors.idle;
      instanceStatus.title = tooltip || "";
    }

    function setLoadedInstanceStatusWithSize(instanceLabel, rowCount, createdCount = 0) {
      const countText = `${rowCount} row${rowCount === 1 ? "" : "s"}`;
      const seedText =
        createdCount > 0
          ? ` Seeded ${createdCount} missing row${createdCount === 1 ? "" : "s"} from template.`
          : "";
      if (rowCount >= largeChecklistWarnThreshold) {
        setInstanceStatus(
          "warn",
          `${instanceLabel} loaded (${countText}).${seedText} Large checklist; this may behave slower. It may help to split into multiple smaller checklists.`
        );
        return;
      }
      setInstanceStatus("loaded", `${instanceLabel} loaded (${countText}).${seedText}`);
    }

    function updateLoadButton(message) {
      const selected = checklistSelect?.value || "";
      const isLoaded = !!selected && selected === loadedChecklist;
      if (listBtn) {
        listBtn.textContent = isLoaded ? "Checklist Loaded" : "Load Checklist";
        listBtn.disabled = isLoaded;
      }
      if (checklistCopyBtn) checklistCopyBtn.disabled = !selected;
      if (checklistDeleteBtn) checklistDeleteBtn.disabled = !selected;
      if (instanceCopyBtn) instanceCopyBtn.disabled = !(instanceSelect?.value);
      setChecklistStatus(
        isLoaded ? "loaded" : "idle",
        message || (isLoaded ? `Loaded checklist "${selected}"` : "Select a checklist")
      );
    }

    function syncSelectionToUrl(checklistValue, instanceValue) {
      const checklist = (checklistValue || "").trim();
      const instance = (instanceValue || "").trim();
      const queryParts = [];
      if (checklist) {
        queryParts.push(`checklist_id=${encodeURIComponent(checklist)}`);
      }
      if (instance) {
        queryParts.push(`instance_id=${encodeURIComponent(instance)}`);
      }
      const hash = window.location.hash || "";
      const nextUrl = `${window.location.pathname}${queryParts.length > 0 ? `?${queryParts.join("&")}` : ""}${hash}`;
      const currentUrl = `${window.location.pathname}${window.location.search}${hash}`;
      if (nextUrl === currentUrl) {
        return;
      }
      window.history.replaceState({ checklist_id: checklist, instance_id: instance }, "", nextUrl);
    }

    function resolveInstancePrincipal(instanceId) {
      const val = instanceId || getRootInstance();
      return instanceMeta.get(val)?.principal || (isRootInstance(val) ? ROOT_PRINCIPAL : "");
    }

    function updateInstanceInputs(value) {
      const val = value || getRootInstance();
      loadedInstance = val;
      if (instanceSelect && instanceSelect.value !== val) instanceSelect.value = val;
      if (instanceCopyBtn) instanceCopyBtn.disabled = !val;
      if (instanceDeleteBtn) instanceDeleteBtn.disabled = !val || isRootInstance(val);
      setInstanceStatus(val ? "idle" : "idle", val ? `Selected ${val}` : "Select an instance");
      syncMarkdownFields();
    }

    function renderEntityOptions() {
      if (!entitySelect) return;
      entitySelect.innerHTML = "";
      entities.forEach((e) => {
        const opt = document.createElement("option");
        const principal = e.principal || "";
        opt.value = e.entity_id || e.id || "";
        opt.textContent = `${e.entity_id || e.id || ""}${principal ? ` (${principal})` : ""}`;
        opt.dataset.principal = principal;
        entitySelect.appendChild(opt);
      });
    }

    function setActiveEntity(id, principal) {
      activeEntity = { id: id || "", principal: principal || "" };
      if (activeEntityIdSpan) activeEntityIdSpan.textContent = activeEntity.id || "(unset)";
      if (activeEntityPrincipalSpan)
        activeEntityPrincipalSpan.textContent =
          SHOW_PRINCIPAL && activeEntity.principal ? activeEntity.principal : "(hidden)";
    }

    function updateSessionPanel() {
      if (tokenStatusSpan) {
        tokenStatusSpan.textContent = isTokenValid() ? "ready" : "not set";
      }
      if (tokenScopesSpan) {
        tokenScopesSpan.textContent = authState?.scope || "";
      }
      if (devEntityPanel) {
        devEntityPanel.style.display = DEV_MODE ? "block" : "none";
      }
    }

    async function fetchMe() {
      if (!isTokenValid()) return;
      try {
        const data = await fetchJson("/api/v1/me");
        const entity = data.entity || {};
        const auth = data.auth || {};
        setActiveEntity(entity.entity_id || "", entity.entity_principal || "");
        if (tokenScopesSpan) tokenScopesSpan.textContent = (auth.scopes || []).join(" ");
      } catch (err) {
        tokenStatusSpan && (tokenStatusSpan.textContent = "unauthenticated");
      }
    }

    function startOAuthLogin() {
      if (!window.location.protocol.startsWith("http")) {
        alert("Please run the UI via http://localhost (file:// is not supported).");
        return;
      }
      try {
        const verifierBytes = new Uint8Array(32);
        crypto.getRandomValues(verifierBytes);
        const verifier = btoa(String.fromCharCode(...verifierBytes))
          .replace(/\+/g, "-")
          .replace(/\//g, "_")
          .replace(/=+/g, "");
        const encoder = new TextEncoder();
        const hashed = crypto.subtle.digest("SHA-256", encoder.encode(verifier));
        hashed.then((buf) => {
          const bytes = Array.from(new Uint8Array(buf));
          const challenge = btoa(String.fromCharCode(...bytes))
            .replace(/\+/g, "-")
            .replace(/\//g, "_")
            .replace(/=+/g, "");
          const state = Math.random().toString(36).slice(2);
          const originUrl = baseUrl();
          if (!originUrl) {
            alert("Base URL is empty. Please set host/port.");
            return;
          }
          let redirectUri = "";
          if (window.location.protocol.startsWith("http")) {
            redirectUri = new URL("oauth_callback.html", window.location.href).toString();
          } else {
            redirectUri = `${originUrl}/CHAX-CLIENT/web/oauth_callback.html`;
          }
          const pkce = {
            verifier,
            state,
            clientId: PUBLIC_CLIENT_ID,
            clientSecret: PUBLIC_CLIENT_SECRET,
            redirectUri,
            baseUrl: originUrl,
            returnTo: window.location.pathname,
          };
          sessionStorage.setItem(PKCE_STORE_KEY, JSON.stringify(pkce));
          const params = new URLSearchParams({
            response_type: "code",
            client_id: PUBLIC_CLIENT_ID,
            redirect_uri: redirectUri,
            scope: "checklist:read checklist:write",
            state,
            code_challenge: challenge,
            code_challenge_method: "S256",
          });
          tokenStatusSpan && (tokenStatusSpan.textContent = "redirecting...");
          window.location.href = `${originUrl}/oauth/authorize?${params.toString()}`;
        });
      } catch (err) {
        console.error("OAuth login failed to start", err);
        alert("OAuth login could not start: " + err.message);
      }
    }

    function logout() {
      setAuthState(null);
      setActiveEntity("", "");
      clearPkceState();
    }

    function findInstanceMatches(filterValue) {
      const filter = (filterValue || "").toLowerCase();
      if (!filter) return instanceOptions.slice();
      return instanceOptions.filter((id) => {
        if (id.toLowerCase().includes(filter)) return true;
        const principal = instanceMeta.get(id)?.principal || "";
        return principal.toLowerCase().includes(filter);
      });
    }

    function renderInstanceOptions(filterValue = "") {
      if (!instanceSelect) return;
      instanceSelect.innerHTML = "";
      const filtered = findInstanceMatches(filterValue);
      const preferred = loadedInstance || instanceSelect.value || getRootInstance();
      filtered.forEach((val) => {
        const principal = instanceMeta.get(val)?.principal;
        const label =
          SHOW_INSTANCE_PRINCIPAL && principal ? `${val} (${principal})` : val;
        const opt = document.createElement("option");
        opt.value = val;
        opt.textContent = label;
        instanceSelect.appendChild(opt);
      });
      if (filtered.length === 0) {
        const opt = document.createElement("option");
        const fallback = getRootInstance();
        const principal = instanceMeta.get(fallback)?.principal || (isRootInstance(fallback) ? ROOT_PRINCIPAL : "");
        opt.value = fallback;
        opt.textContent = SHOW_INSTANCE_PRINCIPAL && principal ? `${fallback} (${principal})` : fallback;
        instanceSelect.appendChild(opt);
      }
      const selected = filtered.includes(preferred) ? preferred : (filtered[0] || getRootInstance());
      if (instanceSelect.value !== selected) {
        instanceSelect.value = selected;
      }
      updateInstanceInputs(selected);
      if (instanceCopyBtn) instanceCopyBtn.disabled = !(instanceSelect?.value);
      if (instanceDeleteBtn) instanceDeleteBtn.disabled = !instanceSelect?.value || isRootInstance(instanceSelect.value);
      setInstanceStatus(instanceSelect?.value ? "idle" : "idle", instanceSelect?.value ? `Selected ${instanceSelect.value}` : "Select an instance");
    }

    async function tryCreateInstanceFromFilter() {
      if (!instanceFilter) return;
      if (creatingInstanceFromFilter) return;
      const value = (instanceFilter.value || "").trim();
      if (!value) return;
      const now = Date.now();
      if (value === lastInstanceCreateAttemptValue && now - lastInstanceCreateAttemptAt < 1500) {
        return;
      }
      lastInstanceCreateAttemptAt = now;
      lastInstanceCreateAttemptValue = value;
      const exactId = instanceOptions.find((id) => id === value);
      const exactPrincipal = instanceOptions.find(
        (id) => (instanceMeta.get(id)?.principal || "") === value
      );
      if (exactId || exactPrincipal) {
        updateInstanceInputs(exactId || exactPrincipal);
        renderInstanceOptions("");
        instanceFilter.value = "";
        return;
      }
      const matches = findInstanceMatches(value);
      if (matches.length > 0) return;
      const checklistName = (checklistSelect?.value || loadedChecklist || "").trim();
      if (!checklistName) {
        alert("Select a checklist first.");
        return;
      }
      creatingInstanceFromFilter = true;
      try {
        const ok = confirm(
          `Create new instance for "${checklistName}" using principal:\n${value}\n\nProceed?`
        );
        if (!ok) return;
        const newId = await instantiateFromTemplate(checklistName, value);
        if (newId) {
          recordPrincipal(newId, value);
          if (!instanceOptions.includes(newId)) {
            instanceOptions.push(newId);
          }
          renderInstanceOptions("");
          updateInstanceInputs(newId);
          await listSlugs();
          instanceFilter.value = "";
        }
      } catch (err) {
        alert(`Instance create failed: ${err.message}`);
      } finally {
        creatingInstanceFromFilter = false;
      }
    }

    function syncMarkdownFields() {
      if (mdWorkspaceExportPack && !mdWorkspaceExportPack.value) {
        const item = currentMdWorkspaceItem();
        if (item?.pack) {
          mdWorkspaceExportPack.value = item.pack;
        }
      }
    }

    function canTemplateEdit() {
      if (state.mode !== "service") return false;
      if (!state.editEnabled) return false;
      return true;
    }

    function canStateEdit() {
      if (state.mode === "guest") return false;
      if (isRootInstance(loadedInstance) && state.mode !== "service") return false;
      if (state.mode === "service") return state.editEnabled;
      return true;
    }

    function updateModeHint() {
      if (!modeHint) return;
      const viewOnly = !canStateEdit();
      document.body.dataset.viewOnly = viewOnly ? "1" : "0";
      if (editToggle) editToggle.disabled = state.mode === "guest";
      if (editToggle) editToggle.style.display = state.mode === "service" ? "inline-flex" : "none";
      if (newChecklistBtn) newChecklistBtn.disabled = !canTemplateEdit();
      if (state.mode === "guest") {
        modeHint.textContent = "Guest mode: view only. Editing and draft creation are hidden.";
      } else if (isRootInstance(loadedInstance) && state.mode !== "service") {
        modeHint.textContent = "Template (root) view: normal users are read-only; switch to Service to edit template.";
      } else if (state.mode === "service" && !state.editEnabled) {
        modeHint.textContent = "Service mode: edits disabled. Toggle \"Allow edits\" to author templates or update results.";
      } else if (state.mode === "service" && state.editEnabled) {
        modeHint.textContent = "Service mode: editing enabled. New rows/sections require Save before IDs are assigned.";
      } else if (!canTemplateEdit()) {
        modeHint.textContent = "User mode: you can update results/status/comments. Template edits are disabled.";
      } else {
        modeHint.textContent = "User mode: update mutable fields for this instance.";
      }
    }

    function setSaveState(rowKey, stateStr, tooltip = "") {
      const cells = document.querySelectorAll(`td.state-cell[data-id='${rowKey}']`);
      cells.forEach((cell) => {
        const draftNote = cell.querySelector(".draft-note");
        const hasDraftBtn = !!cell.querySelector(".save-draft");
        cell.dataset.state = stateStr;
        cell.title = tooltip;
        if (hasDraftBtn && stateStr !== "saved") {
          if (draftNote) draftNote.textContent = tooltip || "Unsaved draft";
          cell.className = "state-cell state-unsaved";
          return;
        }
        if (stateStr === "pending") {
          cell.innerHTML = SAVE_ICONS.pending;
          cell.className = "state-cell state-pending";
        } else if (stateStr === "unsaved") {
          cell.innerHTML = SAVE_ICONS.unsaved;
          cell.className = "state-cell state-unsaved";
        } else {
          cell.innerHTML = SAVE_ICONS.saved;
          cell.className = "state-cell state-saved";
        }
      });
    }

    function setIndicatorCell(rowKey, status, comment) {
      const cell = document.querySelector(`td.ind-cell[data-id='${rowKey}']`);
      if (!cell) return;
      const ind = indicatorFor(status, comment);
      cell.innerHTML = ind.symbol;
      cell.title = ind.tooltip;
      cell.className = `ind-cell ${ind.cls}`;
    }

    async function checkHealth() {
      if (!healthStatus) return;
      healthStatus.textContent = "Checking...";
      healthStatus.style.color = "";
      try {
        const data = await fetchJson("/api/v1/health");
        healthStatus.textContent = `OK (${data.status || "ok"}, uptime ${data.uptime_ms} ms)`;
        healthStatus.style.color = "#7bd58c";
      } catch (err) {
        healthStatus.textContent = `Error: ${err.message}`;
        healthStatus.style.color = "#ff8080";
      }
    }

    function collectPayload(addressId) {
      const row = findRowByAddress(tableData, addressId);
      const normalizedStatus = normalizeMutableStatus(row?.status);
      const payload = {
        result: row?.result || "",
        comment: row?.comment || "",
        entity_id: activeEntity.id || "",
      };
      if (normalizedStatus) {
        payload.status = normalizedStatus;
      }
      return payload;
    }

    const daemonRefreshSeq = new Map(); // subject_address_id -> sequence number

    function applyServerSlugToLoadedRow(addressId, serverSlug) {
      const row = findRowByAddress(tableData, addressId);
      if (!row) return false;

      const nextResult = serverSlug.result ?? row.result ?? "";
      const nextStatus = normalizeMutableStatus(serverSlug.status ?? row.status ?? "");
      const nextComment = serverSlug.comment ?? row.comment ?? "";
      const nextEntityId = serverSlug.entity_id ?? row.entityId ?? "";
      const nextTimestamp = serverSlug.timestamp ?? row.timestamp ?? "";
      const changed =
        row.result !== nextResult ||
        row.status !== nextStatus ||
        row.comment !== nextComment ||
        row.entityId !== nextEntityId ||
        row.timestamp !== nextTimestamp;

      row.result = nextResult;
      row.status = nextStatus;
      row.comment = nextComment;
      row.entityId = nextEntityId;
      row.timestamp = nextTimestamp;

      if (!changed) {
        return false;
      }

      const rowId = row.uid;
      const resultInput = slugTableBody.querySelector(`.row-result[data-row='${rowId}']`);
      if (resultInput && document.activeElement !== resultInput) {
        resultInput.value = row.result || "";
      }

      const commentInput = slugTableBody.querySelector(`.row-comment[data-row='${rowId}']`);
      if (commentInput && document.activeElement !== commentInput) {
        commentInput.value = row.comment || "";
      }

      const radios = slugTableBody.querySelectorAll(`input[type='radio'][name='status-${rowId}']`);
      radios.forEach((r) => {
        r.checked = r.value === row.status;
      });

      setIndicatorCell(row.addressId || row.uid, row.status, row.comment);
      setResultVerifyIndicatorCell(row.addressId || row.uid, row.verifyIndicator || defaultVerifyIndicator());
      setSaveState(row.addressId || row.uid, "saved", "Saved");
      return true;
    }

    async function refreshRelatedRowsAfterSave(subjectAddressId) {
      const row = findRowByAddress(tableData, subjectAddressId);
      if (!row || !row.addressId) return;

      const nextSeq = (daemonRefreshSeq.get(subjectAddressId) || 0) + 1;
      daemonRefreshSeq.set(subjectAddressId, nextSeq);

      await new Promise((resolve) => setTimeout(resolve, 250));
      if ((daemonRefreshSeq.get(subjectAddressId) || 0) !== nextSeq) return;

      const payload = await fetchJson(`/api/v1/relationships/address/${encodeURIComponent(subjectAddressId)}`);
      const outgoing = payload.outgoing || [];
      const incoming = payload.incoming || [];
      row.relationships = outgoing.map((e) => ({ predicate: e.predicate || "", target: e.target || "" }));
      row.incomingRelationships = incoming.map((e) => ({
        predicate: e.predicate || "",
        source: e.source || e.target || "",
      }));
      row.prefillDataset = payload.prefill_dataset || null;

      const listEl = slugTableBody.querySelector(`.relationships-list[data-row='${row.uid}']`);
      if (listEl) {
        listEl.innerHTML = renderRelationships(row.relationships || [], null, row.prefillDataset);
      }
      const incomingListEl = slugTableBody.querySelector(`.relationships-list.incoming-relationships[data-row='${row.uid}']`);
      if (incomingListEl) {
        incomingListEl.innerHTML = renderIncomingRelationships(row.incomingRelationships || [], null);
      }

      const targetIds = Array.from(new Set(outgoing.map((e) => e.target).filter(Boolean)));
      await Promise.all(
        targetIds.map(async (targetId) => {
          const targetRow = findRowByAddress(tableData, targetId);
          if (!targetRow) return;
          const targetSlug = await fetchJson(`/api/v1/slugs/${encodeURIComponent(targetId)}`);
          applyServerSlugToLoadedRow(targetId, targetSlug);
        })
      );
      await refreshVerifyIndicatorsForAddresses([subjectAddressId, ...targetIds]);
    }

    async function saveRow(addressId, payload) {
      const body = payload || collectPayload(addressId);
      const payloadStr = JSON.stringify(body);
      await fetchJson(`/api/v1/slugs/${encodeURIComponent(addressId)}`, {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: payloadStr,
      });
      refreshRelatedRowsAfterSave(addressId).catch(() => {});
    }

    async function saveStatusNow(row, rerenderOnSuccess) {
      const key = row.addressId || row.uid;
      if (!row.addressId) {
        setSaveState(key, "unsaved", "Unsaved draft");
        if (rerenderOnSuccess) {
          renderTable();
        }
        return;
      }
      setSaveState(key, "pending", "Saving status...");
      try {
        await saveRow(row.addressId, collectPayload(row.addressId));
        setSaveState(key, "saved", "Saved");
        if (rerenderOnSuccess) {
          renderTable();
        }
      } catch (err) {
        setSaveState(key, "unsaved", err?.message || "Save failed");
      }
    }

    function queueSave(row) {
      const key = row.addressId || row.uid;
      if (!saver || !row.addressId) {
        setSaveState(key, "unsaved", "Unsaved draft");
        return;
      }
      saver.queue(row.addressId, () => collectPayload(row.addressId));
    }

    function addDraftRow(sectionId, afterRowId) {
      if (!canTemplateEdit()) return;
      const section = tableData.find((s) => s.uid === sectionId);
      if (!section) return;
      const newRow = createDraftRow(loadedChecklist, section.name, ++draftCounter);
      const idx = afterRowId ? section.rows.findIndex((r) => r.uid === afterRowId) : section.rows.length - 1;
      if (idx === -1) {
        section.rows.push(newRow);
      } else {
        section.rows.splice(idx + 1, 0, newRow);
      }
      renderTable();
    }

    function addNewSection() {
      if (!canTemplateEdit()) return;
      const uid = `section-${++sectionCounter}`;
      const section = {
        uid,
        name: "",
        isNew: true,
        rows: [createDraftRow(loadedChecklist, "", ++draftCounter)],
      };
      tableData.push(section);
      renderTable();
    }

    function startNewChecklist(nameInput) {
      if (!canTemplateEdit()) return;
      const name = (nameInput || "").trim() || prompt("Name for the new checklist?");
      if (!name) return;
      loadedChecklist = name;
      rootInstanceId = "";
      instanceMeta.clear();
      instanceOptions = [getRootInstance()];
      updateInstanceInputs(getRootInstance());
      renderInstanceOptions(instanceFilter?.value || "");
      if (!allChecklists.includes(name)) {
        allChecklists.push(name);
      }
      checklistSelect.innerHTML = `<option value="${escapeHtml(name)}">${escapeHtml(name)}</option>`;
      checklistSelect.value = name;
      tableData = [];
      addNewSection();
      updateLoadButton();
    }

    function findChecklistMatches(filterValue) {
      const filter = (filterValue || "").toLowerCase();
      if (!filter) return allChecklists.slice();
      return allChecklists.filter((c) => c.toLowerCase().includes(filter));
    }

    function updateChecklistOptions(filterValue = "") {
      checklistSelect.innerHTML = "";
      const filtered = findChecklistMatches(filterValue);
      filtered.forEach((name) => {
        const opt = document.createElement("option");
        opt.value = name;
        opt.textContent = name;
        checklistSelect.appendChild(opt);
      });
      if (checklistSelect.options.length > 0 && !checklistSelect.value) {
        checklistSelect.value = checklistSelect.options[0].value;
      }
      updateLoadButton();
    }

    async function tryCreateChecklistFromFilter() {
      if (!checklistFilter) return;
      if (creatingChecklistFromFilter) return;
      const value = (checklistFilter.value || "").trim();
      if (!value) return;
      if (allChecklists.includes(value)) {
        checklistSelect.value = value;
        updateLoadButton();
        return;
      }
      const matches = findChecklistMatches(value);
      if (matches.length > 0) return;
      if (!canTemplateEdit()) {
        const now = Date.now();
        if (now - lastChecklistCreateDeniedAt > 3000) {
          lastChecklistCreateDeniedAt = now;
          alert("Checklist creation requires Service mode with edits enabled.");
        }
        return;
      }
      creatingChecklistFromFilter = true;
      try {
        const ok = confirm(`Create new checklist:\n${value}\n\nProceed?`);
        if (!ok) return;
        startNewChecklist(value);
        checklistFilter.value = "";
      } finally {
        creatingChecklistFromFilter = false;
      }
    }

    async function loadChecklists() {
      await loadInstanceCatalog();
      try {
        const data = await fetchJson("/api/v1/checklists");
        allChecklists = data.checklists || data.items || [];
        updateChecklistOptions(checklistFilter.value || "");
      } catch (err) {
        allChecklists = ["chax-demo"];
        updateChecklistOptions(checklistFilter.value || "");
      }
      if (checklistSelect.value) {
        await loadInstancesForChecklist(checklistSelect.value);
      }
      if (checklistCopyBtn) checklistCopyBtn.disabled = !(checklistSelect?.value);
      syncMarkdownFields();
    }

    function buildTableFromSlugs(slugs) {
      const map = new Map();
      slugs.forEach((slug) => {
        const sectionName = slug.section || "(no section)";
        if (!map.has(sectionName)) {
          map.set(sectionName, {
            uid: `section-${++sectionCounter}`,
            name: sectionName,
            isNew: false,
            rows: [],
          });
        }
        map.get(sectionName).rows.push({
          uid: slug.address_id || `row-${++draftCounter}`,
          addressId: slug.address_id || "",
          slugId: slug.slug_id || "",
          checklist: slug.checklist || loadedChecklist,
          section: sectionName,
          action: slug.action || "",
          spec: slug.spec || "",
          result: slug.result || "",
          status: normalizeMutableStatus(slug.status),
          comment: slug.comment || "",
          instructions: slug.instructions || "",
          procedure: slug.procedure || "",
          entityId: slug.entity_id || "",
          timestamp: slug.timestamp || "",
          instanceId: slug.instance_id || "",
          prefillDataset: slug.prefill_dataset || null,
          relationships: slug.relationships || [],
          incomingRelationships: [],
          templateRelationships: templateRelationshipsBySlug.get(slug.slug_id) || [],
          verifyIndicator: verifyIndicatorByAddress.get(slug.address_id) || defaultVerifyIndicator(),
          isNew: false,
        });
      });
      tableData = Array.from(map.values());
    }

    function loadedRowCount() {
      return tableData.reduce((sum, section) => sum + (section.rows?.length || 0), 0);
    }

    function effectiveAutoRefreshMs() {
      if (!autoRefreshMs) return 0;
      const rows = loadedRowCount();
      if (rows >= 1200) return Math.max(autoRefreshMs, 6000);
      if (rows >= 600) return Math.max(autoRefreshMs, 3000);
      if (rows >= 250) return Math.max(autoRefreshMs, 1800);
      return autoRefreshMs;
    }

    function shouldRefreshVerifyOnAuto(changedAddressIds) {
      const changed = Array.isArray(changedAddressIds) ? changedAddressIds.length : 0;
      if (!changed) return false;
      if (verifyAutoRefreshMs <= 0) return false;
      const now = Date.now();
      if (now - lastAutoVerifyRefreshAt < verifyAutoRefreshMs) {
        return false;
      }
      lastAutoVerifyRefreshAt = now;
      return true;
    }

    function markUserInteraction() {
      lastUserInteractionAt = Date.now();
    }

    async function mapWithConcurrency(items, concurrency, worker) {
      const list = Array.isArray(items) ? items : [];
      if (!list.length) return [];
      const limit = Math.max(1, Math.min(concurrency || 1, list.length));
      const results = new Array(list.length);
      let nextIndex = 0;
      const runners = new Array(limit).fill(0).map(async () => {
        while (true) {
          const current = nextIndex++;
          if (current >= list.length) {
            return;
          }
          results[current] = await worker(list[current], current);
        }
      });
      await Promise.all(runners);
      return results;
    }

    function hasDraftRows() {
      return tableData.some((section) => section.rows.some((row) => row.isNew));
    }

    function hasPendingState() {
      if (!slugTableBody) return false;
      return !!slugTableBody.querySelector(".state-cell.state-pending, .state-cell.state-unsaved");
    }

    function isEditingTable() {
      if (!slugTableBody) return false;
      const active = document.activeElement;
      if (!active || !slugTableBody.contains(active)) return false;
      if (active.isContentEditable) return true;
      return active.tagName === "INPUT" || active.tagName === "TEXTAREA" || active.tagName === "SELECT";
    }

    async function refreshLoadedSlugs() {
      const checklistName = (checklistSelect?.value || loadedChecklist || "").trim();
      if (!checklistName) return;
      const instanceVal = (instanceSelect?.value || loadedInstance || getRootInstance()).trim() || getRootInstance();
      const isRoot = isRootInstance(instanceVal);
      let items = [];

      try {
        const params = new URLSearchParams();
        params.set("checklist", checklistName);
        addWorkspaceQueryContext(params, checklistName);
        if (isRoot) {
          addTemplateInstanceQuery(params);
        } else {
          params.set("instance_id", instanceVal);
        }
        const data = await fetchJson(`/api/v1/slugs?${params.toString()}`);
        items = data.items || [];
      } catch (err) {
        setInstanceStatus("error", `Refresh failed: ${err.message}`);
        return;
      }

      if (!items.length && isRootInstance(instanceVal)) {
        try {
          const fallbackParams = new URLSearchParams({ checklist: checklistName });
          addWorkspaceQueryContext(fallbackParams, checklistName);
          const all = await fetchJson(
            `/api/v1/slugs?${fallbackParams.toString()}`
          );
          const rootLower = ROOT_PRINCIPAL_LOWER;
          items = (all.items || []).filter((s) => {
            if (rootInstanceId && s.instance_id === rootInstanceId) return true;
            return (s.instance_principal || "").toLowerCase() === rootLower;
          });
          const firstRoot = (all.items || []).find(
            (s) => (s.instance_principal || "").toLowerCase() === rootLower
          );
          if (firstRoot?.instance_id && !rootInstanceId) {
            rootInstanceId = firstRoot.instance_id;
          }
        } catch (err) {
          setInstanceStatus("error", `Refresh failed: ${err.message}`);
          return;
        }
      }

      if (!items.length) return;
      const expected = loadedRowCount();
      if (expected === 0 || items.length !== expected) {
        await listSlugs({ silent: true });
        return;
      }
      for (const slug of items) {
        if (!findRowByAddress(tableData, slug.address_id)) {
          await listSlugs({ silent: true });
          return;
        }
      }
      const changedAddressIds = [];
      items.forEach((slug) => {
        if (applyServerSlugToLoadedRow(slug.address_id, slug)) {
          changedAddressIds.push(slug.address_id);
        }
      });
      if (shouldRefreshVerifyOnAuto(changedAddressIds)) {
        await refreshVerifyIndicatorsForAddresses(changedAddressIds);
      }
    }

    function shouldAutoRefresh() {
      if (!autoRefreshMs) return false;
      if (document.hidden) return false;
      if (!loadedChecklist || !loadedInstance) return false;
      if (listSlugsInFlight || instantiating) return false;
      if (creatingChecklistFromFilter || creatingInstanceFromFilter) return false;
      if (hasDraftRows() || hasPendingState()) return false;
      if (isEditingTable()) return false;
      if (autoRefreshPauseAfterInteractionMs > 0 && Date.now() - lastUserInteractionAt < autoRefreshPauseAfterInteractionMs) {
        return false;
      }
      return true;
    }

    function startAutoRefresh() {
      if (!autoRefreshMs) return;
      if (autoRefreshTimer) clearInterval(autoRefreshTimer);
      nextAutoRefreshAt = Date.now() + effectiveAutoRefreshMs();
      autoRefreshTimer = setInterval(() => {
        if (!shouldAutoRefresh() || autoRefreshInFlight) return;
        const now = Date.now();
        if (nextAutoRefreshAt && now < nextAutoRefreshAt) return;
        const interval = effectiveAutoRefreshMs();
        nextAutoRefreshAt = now + (interval > 0 ? interval : autoRefreshMs);
        autoRefreshInFlight = true;
        refreshLoadedSlugs()
          .catch(() => {})
          .finally(() => {
            autoRefreshInFlight = false;
          });
      }, autoRefreshMs);
    }

    function recordPrincipal(instanceId, principal) {
      if (!instanceId) return;
      if (!principal && instanceMeta.has(instanceId)) return;
      instanceMeta.set(instanceId, { principal: principal || "" });
    }

    async function instantiateFromTemplate(checklistName, principalInput, templateItemsOpt) {
      const principal = (principalInput || "").trim();
      if (!principal) {
        alert("Instance principal is required to create a new instance.");
        return null;
      }
      let templateItems = templateItemsOpt || [];
      if (!templateItems.length) {
        const templateParams = new URLSearchParams();
        templateParams.set("checklist", checklistName);
        addWorkspaceQueryContext(templateParams, checklistName);
        addTemplateInstanceQuery(templateParams);
        try {
          const templateData = await fetchJson(`/api/v1/slugs?${templateParams.toString()}`);
          templateItems = templateData.items || [];
        } catch {
          templateItems = [];
        }
        if (!templateItems.length) {
          try {
            const fallbackParams = new URLSearchParams({ checklist: checklistName });
            addWorkspaceQueryContext(fallbackParams, checklistName);
            const templateData = await fetchJson(`/api/v1/slugs?${fallbackParams.toString()}`);
            const allItems = templateData.items || [];
            templateItems = allItems.filter((s) => {
              const id = s.instance_id || "";
              const principal = (s.instance_principal || "").toLowerCase();
              if (rootInstanceId && id === rootInstanceId) return true;
              return principal === ROOT_PRINCIPAL_LOWER;
            });
            allItems.forEach((s) => {
              if (s.instance_id && s.instance_principal) recordPrincipal(s.instance_id, s.instance_principal);
              if (!rootInstanceId && s.instance_principal && s.instance_principal.toLowerCase() === ROOT_PRINCIPAL_LOWER) {
                rootInstanceId = s.instance_id || rootInstanceId;
              }
            });
          } catch {
            templateItems = [];
          }
        }
      }
      if (!templateItems.length) {
        const now = Date.now();
        if (now - lastInstanceNoTemplateAt > 3000) {
          lastInstanceNoTemplateAt = now;
          alert("No template rows found to instantiate this checklist.");
        }
        return false;
      }
      const ok = confirm(`Create ${templateItems.length} rows for instance "${principal}" from checklist "${checklistName}"?`);
      if (!ok) return false;
      instantiating = true;
      let createdInstanceId = "";
      const workspaceContext = resolveScriptsContext(checklistName);
      for (const t of templateItems) {
        const body = {
          checklist: checklistName,
          section: t.section || "",
          procedure: t.procedure || "",
          action: t.action || "",
          spec: t.spec || "",
          instructions: t.instructions || "",
          slug_id: t.slug_id || "",
          instance_principal: principal,
          status: t.status || "",
          result: t.result || "",
          comment: t.comment || "",
        };
        if (workspaceContext.pack) {
          body.source_name = workspaceContext.sourceName || "";
          body.pack = workspaceContext.pack;
          body.checklist_dir = workspaceContext.checklistDir || checklistName;
        }
        const resp = await fetchJson("/api/v1/slugs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
        });
        if (!createdInstanceId) {
          createdInstanceId = resp.instance_id || resp.data?.instance_id || (resp.address_id || resp.data?.address_id || "").split("||")[1] || "";
        }
      }
      instantiating = false;
      if (createdInstanceId) {
        recordPrincipal(createdInstanceId, principal);
        if (!instanceOptions.includes(createdInstanceId)) {
          instanceOptions.push(createdInstanceId);
          renderInstanceOptions(instanceFilter?.value || "");
        }
      }
      return createdInstanceId || true;
    }

    function summarizeJsonlContent(content) {
      const lines = (content || "").split(/\r?\n/);
      let rows = 0;
      let invalid = 0;
      let notObject = 0;
      let missingId = 0;
      let firstError = null;
      for (let i = 0; i < lines.length; i++) {
        const raw = lines[i];
        const trimmed = raw.trim();
        if (!trimmed) continue;
        rows += 1;
        let parsed = null;
        try {
          parsed = JSON.parse(raw);
        } catch (err) {
          invalid += 1;
          if (!firstError) {
            firstError = { line: i + 1, message: err?.message || "Invalid JSON." };
          }
          continue;
        }
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
          notObject += 1;
          if (!firstError) {
            firstError = { line: i + 1, message: "Expected a JSON object per line." };
          }
          continue;
        }
        const hasId = !!parsed.slug_id || !!parsed.address_id;
        if (!hasId) {
          missingId += 1;
        }
      }
      return { rows, invalid, notObject, missingId, firstError };
    }

    async function refreshJsonlImportInfo() {
      if (!jsonlImportFile || !jsonlImportInfo) return;
      const file =
        jsonlImportFile && jsonlImportFile.files && jsonlImportFile.files.length > 0
          ? jsonlImportFile.files[0]
          : null;
      if (!file) {
        jsonlImportInfo.textContent = "";
        setJsonlImportStatus("idle", "Select a JSONL file.");
        return;
      }
      const content = await file.text();
      const summary = summarizeJsonlContent(content);
      const parts = [];
      parts.push(`File: ${file.name}`);
      parts.push(`Bytes: ${file.size}`);
      parts.push(`Rows: ${summary.rows}`);
      if (summary.invalid > 0) {
        parts.push(`Invalid JSON lines: ${summary.invalid}`);
      }
      if (summary.notObject > 0) {
        parts.push(`Non-object lines: ${summary.notObject}`);
      }
      if (summary.missingId > 0) {
        parts.push(`Missing slug_id/address_id: ${summary.missingId}`);
      }
      if (summary.firstError) {
        parts.push(`First error: line ${summary.firstError.line} (${summary.firstError.message})`);
      }
      jsonlImportInfo.textContent = parts.join("\n");
      if (summary.rows == 0) {
        setJsonlImportStatus("warn", "JSONL file is empty.");
      } else if (summary.invalid > 0 || summary.notObject > 0) {
        setJsonlImportStatus("bad", "JSONL parse errors detected.");
      } else if (summary.missingId > 0) {
        setJsonlImportStatus("warn", "Some rows are missing slug_id/address_id.");
      } else {
        setJsonlImportStatus("good", "JSONL ready to import.");
      }
    }

    async function postJsonlImport(content, allowNew) {
      const checklist = (checklistSelect?.value || loadedChecklist || "").trim();
      const instance = (instanceSelect?.value || loadedInstance || "").trim();
      if (!checklist) {
        alert("Select a checklist first.");
        return null;
      }
      if (!instance) {
        alert("Select an instance first.");
        return null;
      }
      const params = new URLSearchParams();
      params.set("checklist", checklist);
      params.set("instance_id", instance);
      if (allowNew) {
        params.set("allow_new", "1");
      }
      const headers = new Headers({ "Content-Type": "application/x-ndjson" });
      if (isTokenValid()) {
        headers.set("Authorization", `Bearer ${authState.access_token}`);
      }
      const resp = await fetch(`${baseUrl()}/api/v1/import/jsonl?${params.toString()}`, {
        method: "POST",
        headers,
        body: content,
      });
      const text = await resp.text();
      let parsed = {};
      if (text) {
        try {
          parsed = JSON.parse(text);
        } catch {
          parsed = {};
        }
      }
      if (!resp.ok || parsed.ok === false) {
        return {
          ok: false,
          status: resp.status,
          error: parsed.error || { message: text || resp.status },
        };
      }
      return { ok: true, data: parsed.data || {}, warnings: parsed.warnings || [] };
    }

    async function importJsonlFile() {
      const file =
        jsonlImportFile && jsonlImportFile.files && jsonlImportFile.files.length > 0
          ? jsonlImportFile.files[0]
          : null;
      if (!file) {
        alert("Select a JSONL file first.");
        return;
      }
      const content = await file.text();
      let result = await postJsonlImport(content, false);
      if (!result) return;
      if (!result.ok) {
        if (result.error?.code === "MISSING_SLUGS") {
          const count = result.error?.details?.count || 0;
          const ok = confirm(
            `JSONL includes ${count} rows not present in this instance.\n` +
              "Import anyway and insert them as new rows?"
          );
          if (!ok) return;
          result = await postJsonlImport(content, true);
        } else {
          alert(`JSONL import failed: ${result.error?.message || "Unknown error."}`);
          return;
        }
      }
      if (!result || !result.ok) {
        alert(`JSONL import failed: ${result?.error?.message || "Unknown error."}`);
        return;
      }
      const warnings = Array.isArray(result.warnings) ? result.warnings : [];
      if (warnings.length > 0) {
        const summary = warnings
          .slice(0, 5)
          .map((w) => `${w.code || "warning"}: ${w.message || ""}`.trim())
          .join("\n");
        alert(`JSONL import completed with warnings:\n${summary}`);
      } else {
        alert("JSONL import complete.");
      }
      await listSlugs();
    }

    async function ensureInstanceCoverage(checklistName, instanceId, templateItems, instanceItems) {
      const templateBySlug = new Map(templateItems.map((t) => [t.slug_id, t]));
      const have = new Set(instanceItems.map((i) => i.slug_id));
      const missing = [];
      templateBySlug.forEach((tpl, slugId) => {
        if (!have.has(slugId)) missing.push(tpl);
      });
      if (!missing.length) {
        return { createdCount: 0, createdItems: [] };
      }
      const principal = resolveInstancePrincipal(instanceId);
      if (!principal) {
        throw new Error(`Instance principal unavailable for ${instanceId}.`);
      }
      setInstanceStatus(
        "idle",
        `Instance "${instanceId}" missing ${missing.length} row${missing.length === 1 ? "" : "s"}; seeding from template...`
      );
      const workspaceContext = resolveScriptsContext(checklistName);
      const createdItems = await mapWithConcurrency(missing, instanceSeedConcurrency, async (t) => {
        const body = {
          slug_id: t.slug_id || "",
          instance_id: instanceId,
          checklist: checklistName,
          section: t.section || "",
          procedure: t.procedure || "",
          action: t.action || "",
          spec: t.spec || "",
          instructions: t.instructions || "",
          instance_principal: principal,
          status: "",
          result: "",
          comment: "",
        };
        if (workspaceContext.pack) {
          body.source_name = workspaceContext.sourceName || "";
          body.pack = workspaceContext.pack;
          body.checklist_dir = workspaceContext.checklistDir || checklistName;
        }
        const created = await fetchJson("/api/v1/slugs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
        });
        return {
          address_id: created.address_id || "",
          slug_id: created.slug_id || t.slug_id || "",
          checklist: checklistName,
          section: t.section || "",
          procedure: t.procedure || "",
          action: t.action || "",
          spec: t.spec || "",
          instructions: t.instructions || "",
          result: created.result || "",
          status: normalizeMutableStatus(created.status || ""),
          comment: created.comment || "",
          entity_id: created.entity_id || "",
          timestamp: created.timestamp || "",
          instance_id: created.instance_id || instanceId,
          prefill_dataset: created.prefill_dataset || null,
          relationships: created.relationships || [],
        };
      });
      return { createdCount: createdItems.length, createdItems };
    }

    async function listSlugs(options = {}) {
      if (listSlugsInFlight) return;
      listSlugsInFlight = true;
      const silent = !!options.silent;
      try {
        const params = new URLSearchParams();
        const checklistName = checklistSelect.value || "chax-demo";
        const instanceVal = (instanceSelect?.value || loadedInstance || getRootInstance()).trim() || getRootInstance();
        updateInstanceInputs(instanceVal);
        syncSelectionToUrl(checklistName, instanceVal);
        if (templateRelationshipsLoadedChecklist !== checklistName) {
          templateRelationshipsBySlug.clear();
          templateRelationshipsLoadedChecklist = "";
        }
        loadedChecklist = checklistName;

        // Fetch template (root) rows
        const tplParams = new URLSearchParams();
        tplParams.set("checklist", checklistName);
        addWorkspaceQueryContext(tplParams, checklistName);
        addTemplateInstanceQuery(tplParams);
        let templateItems = [];
        try {
          const tpl = await fetchJson(`/api/v1/slugs?${tplParams.toString()}`);
          templateItems = tpl.items || [];
        } catch {
          templateItems = [];
        }
        if (!templateItems.length) {
          // fallback: anything without instance filter, filtered client-side to root id
          try {
            const fallbackParams = new URLSearchParams({ checklist: checklistName });
            addWorkspaceQueryContext(fallbackParams, checklistName);
            const all = await fetchJson(`/api/v1/slugs?${fallbackParams.toString()}`);
            const rootLower = ROOT_PRINCIPAL_LOWER;
            templateItems = (all.items || []).filter((s) => {
              const id = s.instance_id || "";
              const principal = (s.instance_principal || "").toLowerCase();
              if (rootInstanceId && id === rootInstanceId) return true;
              return principal === rootLower;
            });
            (all.items || []).forEach((s) => {
              if (s.instance_id && s.instance_principal) recordPrincipal(s.instance_id, s.instance_principal);
              if (!rootInstanceId && s.instance_principal && s.instance_principal.toLowerCase() === ROOT_PRINCIPAL_LOWER) {
                rootInstanceId = s.instance_id || rootInstanceId;
              }
            });
          } catch {
            templateItems = [];
          }
        }
        templateItems.forEach((t) => {
          if (t.instance_id && t.instance_principal) recordPrincipal(t.instance_id, t.instance_principal);
          if (!rootInstanceId && t.instance_principal && t.instance_principal.toLowerCase() === ROOT_PRINCIPAL_LOWER) {
            rootInstanceId = t.instance_id || rootInstanceId;
          }
        });
        if (!rootInstanceId && templateItems.length > 0) {
          rootInstanceId = templateItems[0].instance_id || rootInstanceId;
        }

        ensureTemplateRelationshipsLoaded(checklistName).catch(() => {});

        if (isRootInstance(instanceVal)) {
          buildTableFromSlugs(templateItems);
          const templateAddressIds = templateItems.map((item) => item.address_id).filter(Boolean);
          const deferVerify = templateAddressIds.length >= deferVerifyRenderThreshold;
          if (!deferVerify) {
            await refreshVerifyIndicatorsForAddresses(templateAddressIds);
          }
          rebuildAddressDatalist();
          refreshPredicateCatalog();
          renderTable();
          if (deferVerify) {
            refreshVerifyIndicatorsForAddresses(templateAddressIds).catch(() => {});
          }
          nextAutoRefreshAt = Date.now() + effectiveAutoRefreshMs();
          updateLoadButton();
          setLoadedInstanceStatusWithSize(`Template instance (${getRootInstance()})`, templateAddressIds.length);
          return;
        }

        // Fetch chosen instance rows
        const instParams = new URLSearchParams();
        instParams.set("checklist", checklistName);
        addWorkspaceQueryContext(instParams, checklistName);
        instParams.set("instance_id", instanceVal);
        let instanceItems = [];
        try {
          const inst = await fetchJson(`/api/v1/slugs?${instParams.toString()}`);
          instanceItems = inst.items || [];
        } catch {
          instanceItems = [];
        }
        instanceItems.forEach((s) => {
          if (s.instance_id && s.instance_principal) recordPrincipal(s.instance_id, s.instance_principal);
        });

        // Ensure coverage for missing template rows
        const coverage = await ensureInstanceCoverage(checklistName, instanceVal, templateItems, instanceItems);
        if (coverage.createdCount > 0) {
          const seenSlugs = new Set(instanceItems.map((item) => item.slug_id));
          coverage.createdItems.forEach((item) => {
            if (!item || !item.slug_id || seenSlugs.has(item.slug_id)) {
              return;
            }
            seenSlugs.add(item.slug_id);
            instanceItems.push(item);
          });
        }

        // Strict client-side filter to guard against API over-fetch
        instanceItems = instanceItems.filter((s) => (s.instance_id || "") === instanceVal);

        buildTableFromSlugs(instanceItems);
        const instanceAddressIds = instanceItems.map((item) => item.address_id).filter(Boolean);
        const deferVerify = instanceAddressIds.length >= deferVerifyRenderThreshold;
        if (!deferVerify) {
          await refreshVerifyIndicatorsForAddresses(instanceAddressIds);
        }
        rebuildAddressDatalist();
        refreshPredicateCatalog();
        renderTable();
        if (deferVerify) {
          refreshVerifyIndicatorsForAddresses(instanceAddressIds).catch(() => {});
        }
        nextAutoRefreshAt = Date.now() + effectiveAutoRefreshMs();
        updateLoadButton();
        setLoadedInstanceStatusWithSize(`Instance "${instanceVal}"`, instanceAddressIds.length, coverage.createdCount);
      } catch (err) {
        setChecklistStatus("error", `List failed: ${err.message}`);
        setInstanceStatus("error", `List failed: ${err.message}`);
        if (!silent) {
          alert(`List failed: ${err.message}`);
        }
      } finally {
        listSlugsInFlight = false;
      }
    }

    function setActiveView(view) {
      activeView = view === "flow" ? "flow" : "checklist";
      checklistView?.classList.toggle("view-hidden", activeView !== "checklist");
      flowView?.classList.toggle("view-hidden", activeView !== "flow");
      checklistViewTab?.classList.toggle("active", activeView === "checklist");
      flowViewTab?.classList.toggle("active", activeView === "flow");
      checklistViewTab?.setAttribute("aria-selected", activeView === "checklist" ? "true" : "false");
      flowViewTab?.setAttribute("aria-selected", activeView === "flow" ? "true" : "false");
      if (activeView === "flow") {
        loadFlowGraph();
      }
    }

    function flowRelationshipEdges(graph) {
      return (Array.isArray(graph?.edges) ? graph.edges : []).filter((edge) => edge.kind === "relationship");
    }

    function flowSwimlaneEdges(graph) {
      return (Array.isArray(graph?.edges) ? graph.edges : []).filter((edge) => (
        edge.kind === "checklistOrder" ||
        (
          edge.kind === "relationship" &&
          !edge.is_lineage &&
          !edge.is_external &&
          edge.source_address_id &&
          edge.target_address_id
        )
      ));
    }

    function sortedFlowNodes(nodes) {
      return [...nodes].sort((left, right) => {
        const leftOrder = Number(left.address_order || 0);
        const rightOrder = Number(right.address_order || 0);
        if (leftOrder !== rightOrder) return leftOrder - rightOrder;
        return String(left.address_id || "").localeCompare(String(right.address_id || ""));
      });
    }

    function flowEdgeKey(edge) {
      return [edge?.source_address_id || "", edge?.predicate || "", edge?.target_address_id || ""].join("\u001f");
    }

    function flowPredicateColor(predicate) {
      const colors = {
        ResultSearchPrefillResult: "#96e6a1",
        ResultSearchPrefillStatus: "#76e4f7",
        ResultSearchPrefillComment: "#f4a6d7",
        PassSearchPrefillResult: "#a8d17c",
        BoolVerifyValidatedStatus: "#ffcb66",
        ResultPropagateValidatedResult: "#64d2a6",
        passPropagateValidatedPass: "#7bcf8a",
        failPropagateValidatedFail: "#ff8d8d",
        naPropagateValidatedNa: "#b9c2ce",
        otherPropagateValidatedOther: "#c59bff",
      };
      return colors[predicate] || "var(--accent-cool-strong)";
    }

    function flowNodeName(node, fallback) {
      return node?.procedure || fallback || "(unknown)";
    }

    function flowNodeCard(node, orderColumn) {
      const shape = ["action", "decision", "terminal", "metric"].includes(node.visual_shape)
        ? node.visual_shape
        : "action";
      const status = node.status ? `Status: ${node.status}` : "Status: not set";
      const gridColumn = Number.isInteger(orderColumn) && orderColumn > 0 ? ` style="grid-column: ${orderColumn};"` : "";
      return `
        <article class="flow-card ${shape}" data-flow-node="${escapeHtmlAttr(node.address_id || "")}" tabindex="0" role="button" title="${escapeHtmlAttr(node.address_id || "")}"${gridColumn}>
          <h3>${escapeHtml(flowNodeName(node, "(unnamed procedure)"))}</h3>
          <p>${escapeHtml(node.action || "")}</p>
          <p>${escapeHtml(node.spec || "")}</p>
          <p>${escapeHtml(status)}</p>
        </article>`;
    }

    function renderFlowEdgeList(edges, nodeByAddress) {
      flowEdges.innerHTML = edges
        .map((edge) => {
          const source = flowNodeName(nodeByAddress.get(edge.source_address_id), edge.source_address_id);
          const target = flowNodeName(nodeByAddress.get(edge.target_address_id), edge.target_address_id || "(external)");
          const external = edge.is_external ? "external" : "";
          return `<li class="${external}" data-flow-edge-key="${escapeHtmlAttr(flowEdgeKey(edge))}" tabindex="0" role="button">${escapeHtml(source)} &rarr; <strong>${escapeHtml(
            edge.predicate || ""
          )}</strong> &rarr; ${escapeHtml(target)}</li>`;
        })
        .join("");
      if (!edges.length) {
        flowEdges.innerHTML = '<li>No relationship edges.</li>';
      }
    }

    function renderFlowSwimlanes(nodes) {
      const orderedNodes = sortedFlowNodes(nodes);
      const sections = new Map();
      const orderColumns = new Map();
      orderedNodes.forEach((node, index) => {
        orderColumns.set(node.address_id, index + 1);
        const key = node.section || "(no section)";
        if (!sections.has(key)) sections.set(key, []);
        sections.get(key).push(node);
      });
      flowCanvas.innerHTML = `<div class="flow-lanes-visual" style="--flow-cols: ${Math.max(1, orderedNodes.length)}">${Array.from(sections.entries())
        .map(([section, rows]) => `
          <section class="flow-lane">
            <div class="flow-lane-title">${escapeHtml(section)}</div>
            <div class="flow-cards">${rows.map((node) => flowNodeCard(node, orderColumns.get(node.address_id))).join("")}</div>
          </section>`)
        .join("")}</div>`;
    }

    function renderFlowHierarchy(edges, nodeByAddress) {
      flowCanvas.innerHTML = `<div class="flow-hierarchy">${edges
        .map((edge) => {
          const source = nodeByAddress.get(edge.source_address_id);
          const target = nodeByAddress.get(edge.target_address_id);
          const targetLabel = target
            ? `${target.section || ""}: ${flowNodeName(target)}`
            : `${edge.external_category === "legacy" ? "Legacy" : "External"}: ${edge.target_address_id}`;
          return `
            <div class="flow-hierarchy-row" data-flow-edge-key="${escapeHtmlAttr(flowEdgeKey(edge))}" tabindex="0" role="button">
              <div class="flow-hierarchy-cell">${escapeHtml(
                `${source?.section || ""}: ${flowNodeName(source, edge.source_address_id)}`
              )}</div>
              <div class="flow-hierarchy-cell flow-hierarchy-predicate">${escapeHtml(edge.predicate || "")}</div>
              <div class="flow-hierarchy-cell">${escapeHtml(targetLabel)}</div>
            </div>`;
        })
        .join("")}</div>`;
    }

    function renderFlowGraphMap(nodes, edges, nodeByAddress) {
      const maxNodes = 250;
      const trace = flowTraceState();
      const visibleNodes = [...nodes]
        .sort((left, right) => Number(trace.hitNodes.has(right.address_id)) - Number(trace.hitNodes.has(left.address_id)))
        .slice(0, maxNodes);
      const sections = new Map();
      visibleNodes.forEach((node) => {
        const key = node.section || "(no section)";
        if (!sections.has(key)) sections.set(key, []);
        sections.get(key).push(node);
      });
      const positions = new Map();
      const laneHeight = new Map();
      let y = 8;
      for (const [section, rows] of sections.entries()) {
        const height = 48 + Math.ceil(rows.length / 5) * 82;
        laneHeight.set(section, { y, height });
        rows.forEach((node, index) => {
          positions.set(node.address_id, {
            x: 120 + (index % 5) * 180,
            y: y + 32 + Math.floor(index / 5) * 82,
          });
        });
        y += height + 12;
      }
      const graphEdges = edges.filter(
        (edge) => positions.has(edge.source_address_id) && positions.has(edge.target_address_id)
      );
      const svgLanes = Array.from(laneHeight.entries())
        .map(([section, lane]) => `
          <rect class="lane" x="8" y="${lane.y}" width="1030" height="${lane.height}" rx="6"></rect>
          <text class="lane-label" x="20" y="${lane.y + 22}">${escapeHtml(section)}</text>`)
        .join("");
      const svgEdges = graphEdges
        .map((edge) => {
          const source = positions.get(edge.source_address_id);
          const target = positions.get(edge.target_address_id);
          return `<path class="edge" data-flow-edge-key="${escapeHtmlAttr(flowEdgeKey(edge))}" d="M ${source.x + 160} ${source.y + 22} C ${source.x + 174} ${source.y + 22}, ${target.x - 14} ${target.y + 22}, ${target.x} ${target.y + 22}"></path>`;
        })
        .join("");
      const svgNodes = visibleNodes
        .map((node) => {
          const position = positions.get(node.address_id);
          const shape = ["action", "decision", "terminal", "metric"].includes(node.visual_shape)
            ? node.visual_shape
            : "action";
          const label = flowNodeName(node, "(unnamed)").slice(0, 26);
          let geometry = `<rect class="node ${shape}" x="${position.x}" y="${position.y}" width="160" height="46" rx="6"></rect>`;
          if (shape === "decision") {
            geometry = `<polygon class="node ${shape}" points="${position.x + 80},${position.y} ${position.x + 160},${position.y + 23} ${position.x + 80},${position.y + 46} ${position.x},${position.y + 23}"></polygon>`;
          } else if (shape === "terminal") {
            geometry = `<ellipse class="node ${shape}" cx="${position.x + 80}" cy="${position.y + 23}" rx="80" ry="23"></ellipse>`;
          } else if (shape === "metric") {
            geometry = `<path class="node ${shape}" d="M ${position.x} ${position.y} H ${position.x + 148} L ${position.x + 160} ${position.y + 12} V ${position.y + 46} H ${position.x} Z"></path>`;
          }
          return `
            <g data-flow-node="${escapeHtmlAttr(node.address_id || "")}" tabindex="0" role="button" title="${escapeHtmlAttr(node.address_id || "")}">
              ${geometry}
              <text x="${position.x + 8}" y="${position.y + 20}">${escapeHtml(label)}</text>
              <text x="${position.x + 8}" y="${position.y + 35}">${escapeHtml((node.status || "Not set").slice(0, 22))}</text>
            </g>`;
        })
        .join("");
      const truncation = nodes.length > maxNodes
        ? `<div class="note">Showing the first ${maxNodes} rows. Use Filter to focus the concentrated graph.</div>`
        : "";
      flowCanvas.innerHTML = `${truncation}<div class="flow-graph-map"><svg viewBox="0 0 1050 ${Math.max(y, 120)}" role="img" aria-label="Full concentrated relationship graph">${svgLanes}${svgEdges}${svgNodes}</svg></div>`;
    }

    function renderFlowOverview(title, groups) {
      flowCanvas.innerHTML = `<div class="flow-overview-grid">${groups
        .map((group) => `
          <article class="flow-overview-card">
            <h3>${escapeHtml(group.title || title)}</h3>
            <div class="flow-metrics">${group.metrics
              .map((metric) => `<div class="flow-metric"><strong>${escapeHtml(String(metric.value))}</strong>${escapeHtml(metric.label)}</div>`)
              .join("")}</div>
          </article>`)
        .join("")}</div>`;
    }

    function renderFlowExternal(edges, nodeByAddress) {
      flowCanvas.innerHTML = `<div class="flow-hierarchy">${edges
        .filter((edge) => edge.is_external)
        .map((edge) => `
          <div class="flow-hierarchy-row" data-flow-edge-key="${escapeHtmlAttr(flowEdgeKey(edge))}" tabindex="0" role="button">
            <div class="flow-hierarchy-cell">${escapeHtml(
              flowNodeName(nodeByAddress.get(edge.source_address_id), edge.source_address_id)
            )}</div>
            <div class="flow-hierarchy-cell flow-hierarchy-predicate">${escapeHtml(edge.predicate || "")}</div>
            <div class="flow-hierarchy-cell">${escapeHtml(
              `${edge.external_category === "legacy" ? "Legacy" : "External"}: ${edge.target_address_id}`
            )}</div>
          </div>`)
        .join("")}</div>`;
    }

    function renderFlowLint(nodes, edges, nodeByAddress, warnings) {
      const findings = [];
      const external = edges.filter((edge) => edge.is_external && !edge.is_lineage);
      const lineage = edges.filter((edge) => edge.is_lineage);
      const selfReferences = edges.filter((edge) => edge.source_address_id === edge.target_address_id);
      if (external.length) {
        findings.push({ tone: "", text: `${external.length} operational relationship${external.length === 1 ? "" : "s"} target an external row.` });
      }
      if (lineage.length) {
        findings.push({ tone: "info", text: `${lineage.length} lineage relationship${lineage.length === 1 ? "" : "s"} is displayed as metadata, not workflow routing.` });
      }
      if (selfReferences.length) {
        findings.push({ tone: "", text: `${selfReferences.length} self-reference${selfReferences.length === 1 ? "" : "s"} found.` });
      }
      const disconnectedCount = nodes.filter(
        (node) => !node.incoming_relationship_count && !node.outgoing_relationship_count
      ).length;
      if (disconnectedCount) {
        findings.push({
          tone: "info",
          text: `${disconnectedCount} row${disconnectedCount === 1 ? "" : "s"} has no relationship edges.`,
        });
      }
      (warnings || []).forEach((warning) => findings.push({ tone: "", text: String(warning) }));
      if (!findings.length) {
        findings.push({ tone: "info", text: "No relationship lint findings." });
      }
      flowCanvas.innerHTML = `<div class="flow-lint">${findings
        .map((finding) => `<div class="flow-lint-item ${finding.tone}">${escapeHtml(finding.text)}</div>`)
        .join("")}</div>`;
      flowEdges.innerHTML = "";
    }

    function renderRelationshipWorkbench(workbench) {
      if (!flowSummary || !flowCanvas || !flowEdges) return;
      const nodes = Array.isArray(workbench?.nodes) ? workbench.nodes : [];
      const edges = Array.isArray(workbench?.edges) ? workbench.edges : [];
      const findings = Array.isArray(workbench?.findings) ? workbench.findings : [];
      const summary = workbench?.summary || {};
      const nodeById = new Map(nodes.map((node) => [node.id, node]));
      const kinds = ["checklist_row", "dataset", "dataset_column", "mutation_source", "terminal", "external"];
      const labels = {
        checklist_row: "Checklist rows",
        dataset: "Datasets",
        dataset_column: "Dataset columns",
        mutation_source: "Mutation sources",
        terminal: "Declared outcomes",
        external: "External / unresolved",
      };
      const maxPerKind = 60;
      const grouped = new Map(kinds.map((kind) => [kind, []]));
      nodes.forEach((node) => {
        const kind = grouped.has(node.kind) ? node.kind : "external";
        grouped.get(kind).push(node);
      });
      grouped.forEach((items) => items.sort((left, right) =>
        String(left.title || left.id || "").localeCompare(String(right.title || right.id || ""))));

      const visible = new Map();
      const omitted = [];
      kinds.forEach((kind) => {
        const items = grouped.get(kind) || [];
        items.slice(0, maxPerKind).forEach((node) => visible.set(node.id, node));
        if (items.length > maxPerKind) omitted.push(`${items.length - maxPerKind} ${labels[kind].toLowerCase()}`);
      });
      const positions = new Map();
      const laneWidth = 176;
      const nodeWidth = 160;
      const nodeHeight = 46;
      const rowGap = 64;
      let tallest = 0;
      kinds.forEach((kind, laneIndex) => {
        const items = (grouped.get(kind) || []).slice(0, maxPerKind);
        tallest = Math.max(tallest, items.length);
        items.forEach((node, rowIndex) => {
          positions.set(node.id, { x: 18 + laneIndex * laneWidth, y: 48 + rowIndex * rowGap });
        });
      });
      const width = 18 + kinds.length * laneWidth;
      const height = Math.max(170, 62 + tallest * rowGap);
      const visibleEdges = edges.filter((edge) => visible.has(edge.source_id) && visible.has(edge.target_id));
      const label = (value, limit) => {
        const text = String(value || "");
        return text.length > limit ? `${text.slice(0, Math.max(1, limit - 3))}...` : text;
      };
      const svgEdges = visibleEdges.map((edge) => {
        const source = positions.get(edge.source_id);
        const target = positions.get(edge.target_id);
        if (!source || !target) return "";
        const startX = source.x + nodeWidth;
        const startY = source.y + nodeHeight / 2;
        const endX = target.x;
        const endY = target.y + nodeHeight / 2;
        const midpoint = (startX + endX) / 2;
        return `<path class="workbench-edge ${escapeHtmlAttr(String(edge.class || "relationship"))}" d="M ${startX} ${startY} C ${midpoint} ${startY}, ${midpoint} ${endY}, ${endX} ${endY}" marker-end="url(#workbenchArrow)"><title>${escapeHtml(edge.class || "relationship")}: ${escapeHtml(edge.label || "")}</title></path>`;
      }).join("");
      const svgLanes = kinds.map((kind, laneIndex) =>
        `<text class="workbench-lane" x="${18 + laneIndex * laneWidth}" y="22">${escapeHtml(labels[kind])}</text>`).join("");
      const svgNodes = Array.from(visible.values()).map((node) => {
        const position = positions.get(node.id);
        if (!position) return "";
        return `<g class="workbench-node ${escapeHtmlAttr(String(node.kind || "external"))}">
          <rect x="${position.x}" y="${position.y}" width="${nodeWidth}" height="${nodeHeight}" rx="7"></rect>
          <text x="${position.x + 8}" y="${position.y + 18}">${escapeHtml(label(node.title || node.id, 25))}</text>
          <text class="workbench-subtext" x="${position.x + 8}" y="${position.y + 34}">${escapeHtml(label(node.subtitle || node.kind || "", 31))}</text>
          <title>${escapeHtml(node.title || node.id)}${node.subtitle ? ` — ${escapeHtml(node.subtitle)}` : ""}</title>
        </g>`;
      }).join("");
      const truncation = omitted.length
        ? `<div class="note">Overview shows the first ${maxPerKind} nodes in each lane; ${escapeHtml(omitted.join(", "))} are represented in the exported JSON/DOT but omitted here for legibility.</div>`
        : "";
      const findingPriority = (finding) => {
        if (finding.code === "ORPHAN_ROW") return 0;
        if (finding.code === "COLUMN_BINDING_TARGET_MISSING") return 1;
        if (finding.severity === "warning" || finding.severity === "error") return 2;
        return 3;
      };
      const orderedFindings = [...findings].sort((left, right) =>
        findingPriority(left) - findingPriority(right) || String(left.code || "").localeCompare(String(right.code || "")));
      const findingInfo = (finding) => {
        const details = finding?.details && typeof finding.details === "object" ? finding.details : {};
        const target = nodeById.get(finding.node_id);
        const context = target?.kind === "checklist_row"
          ? `${target.title || "Checklist row"}${target.subtitle ? ` — ${target.subtitle}` : ""}`
          : (details.header || details.path || details.slug_id || "");
        const reasons = [];
        if (finding.code === "ORPHAN_ROW") {
          reasons.push("Checklist display order is intentionally not counted as a relationship.");
          const unresolved = Array.isArray(details.unresolved_markdown_relationships)
            ? details.unresolved_markdown_relationships : [];
          if (unresolved.length) {
            reasons.push(`Unresolved Markdown targets: ${unresolved.map((item) => `${item.predicate || "predicate"} → ${item.target_slug_id || "unknown"}`).join(", ")}.`);
          }
        } else if (finding.code === "CONSTANT_COLUMN") {
          reasons.push(`All ${details.records ?? "?"} records contain one non-empty value (${details.characters ?? "?"} stored characters).`);
        } else if (finding.code === "REPEATED_LITERAL") {
          reasons.push(`${details.nonempty ?? "?"} of ${details.records ?? "?"} records contain one non-empty value; ${details.blank ?? "?"} are blank.`);
        } else if (finding.code === "HIGH_FAN_OUT_LOOKUP_KEY") {
          const coverage = Number(details.bound_row_coverage);
          const percent = Number.isFinite(coverage) ? `${Math.round(coverage * 100)}%` : "a substantial share";
          reasons.push(`${details.bound_columns ?? "?"} fields bind to ${details.bound_rows ?? "?"} of ${details.checklist_rows ?? "?"} rows (${percent}).`);
        }
        if (typeof details.recommendation === "string" && details.recommendation.trim()) {
          reasons.push(`Suggested next step: ${details.recommendation.trim()}`);
        }
        return { context, reasons };
      };
      const findingCards = orderedFindings.slice(0, 18).map((finding) => {
        const info = findingInfo(finding);
        const reasonHtml = info.reasons.length
          ? `<details><summary>Why this appears</summary><div class="finding-context">${escapeHtml(info.reasons.join(" "))}</div></details>`
          : "";
        return `<article class="relationship-workbench-finding ${escapeHtmlAttr(String(finding.severity || "info"))}">
          <strong class="finding-code">${escapeHtml(finding.code || "FINDING")}</strong>
          <div>${escapeHtml(finding.message || "")}</div>
          ${info.context ? `<div class="finding-context">${escapeHtml(info.context)}</div>` : ""}
          ${reasonHtml}
        </article>`;
      }).join("");
      const moreFindings = orderedFindings.length > 18
        ? `<div class="note">${escapeHtml(String(orderedFindings.length - 18))} additional findings are listed below and in relationship-workbench.json.</div>`
        : "";
      flowSummary.innerHTML = [
        `<span class="chip">${escapeHtml(String(summary.rows ?? 0))} rows</span>`,
        `<span class="chip">${escapeHtml(String(summary.predicate_edges ?? 0))} predicate edges</span>`,
        `<span class="chip">${escapeHtml(String(summary.binding_edges ?? 0))} binding edges</span>`,
        `<span class="chip">${escapeHtml(String(summary.datasets ?? 0))} datasets</span>`,
        `<span class="chip">${escapeHtml(String(summary.orphan_rows ?? 0))} orphan rows</span>`,
      ].join("");
      flowCanvas.innerHTML = `${truncation}<section class="relationship-workbench-findings"><h3>Findings</h3>${findingCards || "<div class=\"note\">No relationship findings.</div>"}</section>${moreFindings}<div class="relationship-workbench-map"><svg viewBox="0 0 ${width} ${height}" role="img" aria-label="Relationship Workbench graph"><defs><marker id="workbenchArrow" markerWidth="8" markerHeight="8" refX="7" refY="3" orient="auto"><path d="M0,0 L0,6 L7,3 z"></path></marker></defs>${svgLanes}${svgEdges}${svgNodes}</svg></div>`;
      const visibleFindings = orderedFindings.slice(0, 120);
      flowEdges.innerHTML = visibleFindings.length
        ? visibleFindings.map((finding) => {
          const info = findingInfo(finding);
          return `<li><strong>${escapeHtml(finding.code || "FINDING")}</strong> — ${escapeHtml(finding.message || "")}${info.context ? ` <span class="note">${escapeHtml(info.context)}</span>` : ""}</li>`;
        }).join("")
        : "<li>No relationship findings.</li>";
      if (findings.length > visibleFindings.length) {
        flowEdges.insertAdjacentHTML("beforeend", `<li class="note">${escapeHtml(String(findings.length - visibleFindings.length))} additional findings are available in the exported relationship-workbench.json.</li>`);
      }
      flowRenderedProjection = null;
    }

    function flowNodeMeta(addressId) {
      const node = (flowGraph?.nodes || []).find((item) => item.address_id === addressId);
      return node ? { type: "node", key: addressId, raw: node } : null;
    }

    function flowEdgeMeta(key) {
      const edge = (flowGraph?.edges || []).find((item) => flowEdgeKey(item) === key);
      if (edge) return { type: "edge", key, raw: edge };
      const aggregate = flowRenderedProjection?.groups?.find((item) => item.key === key);
      return aggregate ? { type: "aggregate", key, raw: aggregate } : null;
    }

    function flowHubMeta(predicate, kind) {
      return predicate ? { type: "hub", key: `${kind || "predicate"}:${predicate}`, raw: { predicate, kind: kind || "predicate" } } : null;
    }

    function flowExternalMeta(target) {
      return target ? { type: "external", key: target, raw: { target_address_id: target } } : null;
    }

    function flowMetaFromElement(element) {
      const nodeElement = element.closest("[data-flow-node]");
      if (nodeElement) return flowNodeMeta(nodeElement.dataset.flowNode || "");
      const hubElement = element.closest("[data-flow-hub]");
      if (hubElement) return flowHubMeta(hubElement.dataset.flowHub || "", hubElement.dataset.flowHubKind || "");
      const externalElement = element.closest("[data-flow-external]");
      if (externalElement) return flowExternalMeta(externalElement.dataset.flowExternal || "");
      const edgeElement = element.closest("[data-flow-edge-key]");
      if (edgeElement) return flowEdgeMeta(edgeElement.dataset.flowEdgeKey || "");
      return null;
    }

    function flowMetaText(meta) {
      if (!meta) return "";
      if (meta.type === "node") return JSON.stringify(meta.raw, null, 2);
      if (meta.type === "hub" || meta.type === "external" || meta.type === "aggregate") return JSON.stringify(meta.raw, null, 2);
      const nodes = new Map((flowGraph?.nodes || []).map((node) => [node.address_id, node]));
      const edge = meta.raw;
      return JSON.stringify({
        source: flowNodeName(nodes.get(edge.source_address_id), edge.source_address_id),
        predicate: edge.predicate,
        target: flowNodeName(nodes.get(edge.target_address_id), edge.target_address_id),
        kind: edge.kind,
        is_lineage: edge.is_lineage,
        is_external: edge.is_external,
        external_category: edge.external_category,
      }, null, 2);
    }

    function inspectFlowMeta(meta) {
      flowSelectedMeta = meta;
      if (!flowInspector) return;
      if (!meta) {
        flowInspector.innerHTML = '<h3>Inspect</h3><div class="note">Select or right-click a node or relationship.</div>';
        return;
      }
      if (meta.type === "node") {
        const node = meta.raw;
        flowInspector.innerHTML = [
          '<h3>Inspect Node</h3>',
          `<div class="meta-line"><strong>Procedure</strong><span>${escapeHtml(flowNodeName(node))}</span></div>`,
          `<div class="meta-line"><strong>Slug</strong><span>${escapeHtml(node.slug_id || "")}</span></div>`,
          `<div class="meta-line"><strong>Section</strong><span>${escapeHtml(node.section || "")}</span></div>`,
          `<div class="meta-line"><strong>Action</strong><span>${escapeHtml(node.action || "")}</span></div>`,
          `<div class="meta-line"><strong>Spec</strong><span>${escapeHtml(node.spec || "")}</span></div>`,
          `<div class="meta-line"><strong>Incoming</strong><span>${escapeHtml(String(node.incoming_relationship_count || 0))}</span></div>`,
          `<div class="meta-line"><strong>Outgoing</strong><span>${escapeHtml(String(node.outgoing_relationship_count || 0))}</span></div>`,
          node.instructions ? `<pre>${escapeHtml(node.instructions)}</pre>` : "",
        ].join("");
        return;
      }
      if (meta.type === "hub") {
        flowInspector.innerHTML = [
          '<h3>Inspect Predicate</h3>',
          `<div class="meta-line"><strong>Predicate</strong><span>${escapeHtml(meta.raw.predicate || "")}</span></div>`,
          `<div class="meta-line"><strong>Context</strong><span>${escapeHtml(meta.raw.kind || "relationship")}</span></div>`,
          '<div class="note">Drill down renders the connected rows using this predicate.</div>',
        ].join("");
        return;
      }
      if (meta.type === "external") {
        flowInspector.innerHTML = [
          '<h3>Inspect External Target</h3>',
          `<div class="meta-line"><strong>Target</strong><span>${escapeHtml(meta.raw.target_address_id || "")}</span></div>`,
        ].join("");
        return;
      }
      if (meta.type === "aggregate") {
        flowInspector.innerHTML = [
          '<h3>Inspect Aggregate</h3>',
          `<div class="meta-line"><strong>Source</strong><span>${escapeHtml(meta.raw.sourceLabel || "")}</span></div>`,
          `<div class="meta-line"><strong>Predicate</strong><span>${escapeHtml(meta.raw.predicate || "")}</span></div>`,
          `<div class="meta-line"><strong>Target</strong><span>${escapeHtml(meta.raw.targetLabel || "")}</span></div>`,
          `<div class="meta-line"><strong>Count</strong><span>${escapeHtml(String(meta.raw.edges?.length || 0))}</span></div>`,
        ].join("");
        return;
      }
      const edge = meta.raw;
      const nodeByAddress = new Map((flowGraph?.nodes || []).map((node) => [node.address_id, node]));
      flowInspector.innerHTML = [
        '<h3>Inspect Relationship</h3>',
        `<div class="meta-line"><strong>Source</strong><span>${escapeHtml(flowNodeName(nodeByAddress.get(edge.source_address_id), edge.source_address_id))}</span></div>`,
        `<div class="meta-line"><strong>Predicate</strong><span>${escapeHtml(edge.predicate || "")}</span></div>`,
        `<div class="meta-line"><strong>Target</strong><span>${escapeHtml(flowNodeName(nodeByAddress.get(edge.target_address_id), edge.target_address_id))}</span></div>`,
        `<div class="meta-line"><strong>Category</strong><span>${escapeHtml(edge.external_category || "local")}</span></div>`,
        `<pre>${escapeHtml(flowMetaText(meta))}</pre>`,
      ].join("");
    }

    function showFlowContextMenu(x, y) {
      if (!flowContextMenu) return;
      flowContextMenu.style.left = `${x}px`;
      flowContextMenu.style.top = `${y}px`;
      flowContextMenu.style.display = "block";
    }

    function hideFlowContextMenu() {
      if (flowContextMenu) flowContextMenu.style.display = "none";
    }

    function focusForFlowMeta(meta) {
      if (!meta) return null;
      if (meta.type === "node") return { type: "node", address_id: meta.raw.address_id };
      if (meta.type === "hub" || meta.type === "aggregate") return { type: "predicate", predicate: meta.raw.predicate };
      return { type: "predicate", predicate: meta.raw.predicate };
    }

    function flowTraceState() {
      const nodes = Array.isArray(flowGraph?.nodes) ? flowGraph.nodes : [];
      const edges = flowRelationshipEdges(flowGraph);
      const query = (flowFilterInput?.value || "").trim().toLowerCase();
      const hitNodes = new Set();
      const hitEdges = new Set();
      const nodeMatches = (node) => [
        node.address_id,
        node.slug_id,
        node.section,
        node.procedure,
        node.action,
        node.spec,
        node.status,
      ].join(" ").toLowerCase().includes(query);
      if (query) {
        nodes.filter(nodeMatches).forEach((node) => hitNodes.add(node.address_id));
        edges.forEach((edge) => {
          const predicateMatches = String(edge.predicate || "").toLowerCase().includes(query);
          if (predicateMatches || hitNodes.has(edge.source_address_id) || hitNodes.has(edge.target_address_id)) {
            hitEdges.add(flowEdgeKey(edge));
            hitNodes.add(edge.source_address_id);
            hitNodes.add(edge.target_address_id);
          }
        });
      }
      if (flowFocus?.type === "node") {
        edges.forEach((edge) => {
          if (edge.source_address_id === flowFocus.address_id || edge.target_address_id === flowFocus.address_id) {
            hitEdges.add(flowEdgeKey(edge));
            hitNodes.add(edge.source_address_id);
            hitNodes.add(edge.target_address_id);
          }
        });
        hitNodes.add(flowFocus.address_id);
      } else if (flowFocus?.type === "predicate") {
        edges.forEach((edge) => {
          if (edge.predicate === flowFocus.predicate) {
            hitEdges.add(flowEdgeKey(edge));
            hitNodes.add(edge.source_address_id);
            hitNodes.add(edge.target_address_id);
          }
        });
      }
      return { active: Boolean(query || flowFocus), hitNodes, hitEdges };
    }

    function applyFlowTrace() {
      const trace = flowTraceState();
      const apply = (element, hit) => {
        element.classList.toggle("trace-hit", trace.active && hit);
        element.classList.toggle("trace-dim", trace.active && !hit);
      };
      document.querySelectorAll("[data-flow-node]").forEach((element) => {
        apply(element, trace.hitNodes.has(element.dataset.flowNode || ""));
      });
      document.querySelectorAll("[data-flow-edge-key]").forEach((element) => {
        apply(element, trace.hitEdges.has(element.dataset.flowEdgeKey || ""));
      });
      document.querySelectorAll("[data-flow-hub]").forEach((element) => {
        const predicate = element.dataset.flowHub || "";
        const hit = trace.hitEdges.size
          ? flowRelationshipEdges(flowGraph).some((edge) => edge.predicate === predicate && trace.hitEdges.has(flowEdgeKey(edge)))
          : false;
        apply(element, hit);
      });
    }

    function drawFlowSwimlaneLines() {
      const visual = flowCanvas?.querySelector(".flow-lanes-visual");
      if (!visual || !flowGraph) return;
      let svg = visual.querySelector(".flow-lines");
      if (!svg) {
        svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
        svg.classList.add("flow-lines");
        visual.prepend(svg);
      }
      const rect = visual.getBoundingClientRect();
      svg.setAttribute("viewBox", `0 0 ${visual.scrollWidth} ${visual.scrollHeight}`);
      svg.setAttribute("width", visual.scrollWidth);
      svg.setAttribute("height", visual.scrollHeight);
      svg.innerHTML = "";
      const cards = new Map(Array.from(visual.querySelectorAll("[data-flow-node]")).map((card) => [card.dataset.flowNode, card]));
      flowSwimlaneEdges(flowGraph).forEach((edge) => {
        const source = cards.get(edge.source_address_id);
        const target = cards.get(edge.target_address_id);
        if (!source || !target) return;
        const sourceRect = source.getBoundingClientRect();
        const targetRect = target.getBoundingClientRect();
        const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
        path.dataset.flowEdgeKey = flowEdgeKey(edge);
        if (source === target) {
          const x = sourceRect.right - rect.left - 12;
          const y = sourceRect.top - rect.top + 10;
          path.setAttribute("d", `M ${x} ${y} C ${x + 28} ${y - 24}, ${x + 60} ${y - 24}, ${x + 60} ${y + 8}`);
        } else {
          const sourceCenter = sourceRect.left + sourceRect.width / 2;
          const targetCenter = targetRect.left + targetRect.width / 2;
          const forward = sourceCenter <= targetCenter;
          const start = {
            x: (forward ? sourceRect.right : sourceRect.left) - rect.left,
            y: sourceRect.top + sourceRect.height / 2 - rect.top,
          };
          const end = {
            x: (forward ? targetRect.left : targetRect.right) - rect.left,
            y: targetRect.top + targetRect.height / 2 - rect.top,
          };
          const direction = forward ? 1 : -1;
          path.setAttribute(
            "d",
            `M ${start.x} ${start.y} C ${start.x + direction * 34} ${start.y}, ${end.x - direction * 34} ${end.y}, ${end.x} ${end.y}`
          );
        }
        path.setAttribute("title", edge.predicate || "relationship");
        if (edge.kind === "checklistOrder") {
          path.classList.add("flow-order");
        } else {
          path.setAttribute("stroke", flowPredicateColor(edge.predicate));
        }
        svg.append(path);
      });
    }

    function wireFlowInteractions() {
      (flowCanvas?.querySelectorAll("button[data-flow-zoom]") || []).forEach((button) => {
        button.addEventListener("click", (event) => {
          event.preventDefault();
          event.stopPropagation();
          window.flowGraphEngine?.applyZoom(flowCanvas, button.dataset.flowZoom || "fit");
        });
      });
      const interactive = [
        ...(flowCanvas?.querySelectorAll("[data-flow-node], [data-flow-edge-key], [data-flow-hub], [data-flow-external]") || []),
        ...(flowEdges?.querySelectorAll("[data-flow-edge-key]") || []),
      ];
      interactive.forEach((element) => {
        const inspect = () => inspectFlowMeta(flowMetaFromElement(element));
        element.addEventListener("click", inspect);
        element.addEventListener("contextmenu", (event) => {
          event.preventDefault();
          inspect();
          showFlowContextMenu(event.clientX, event.clientY);
        });
        element.addEventListener("keydown", (event) => {
          if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            inspect();
          }
        });
      });
    }

    function renderFlowGraph(graph) {
      if (!flowSummary || !flowCanvas || !flowEdges) return;
      if (graph?.schema === "chax-relationship-workbench-v1") {
        renderRelationshipWorkbench(graph);
        return;
      }
      const allNodes = Array.isArray(graph?.nodes) ? graph.nodes : [];
      const allEdges = flowRelationshipEdges(graph);
      const summary = graph?.summary || {};
      const nodes = allNodes;
      const nodeByAddress = new Map(allNodes.map((node) => [node.address_id, node]));
      const edges = allEdges;
      const engine = window.flowGraphEngine;
      let renderDone = Promise.resolve();
      flowSummary.innerHTML = [
        `<span class="chip">${escapeHtml(String(summary.sections ?? new Set(nodes.map((node) => node.section)).size))} sections</span>`,
        `<span class="chip">${escapeHtml(String(nodes.length))} rows</span>`,
        `<span class="chip">${escapeHtml(String(edges.length))} relationships</span>`,
        `<span class="chip">${escapeHtml(String(edges.filter((edge) => edge.is_external).length))} external</span>`,
        flowDrilldown ? '<button class="secondary" type="button" data-flow-back>Back to view</button>' : "",
      ].join("");
      if (!nodes.length) {
        flowCanvas.innerHTML = '<div class="note">No checklist rows are available for this instance.</div>';
        flowEdges.innerHTML = "";
        return;
      }

      if (!engine) {
        flowCanvas.innerHTML = '<div class="note">Flow visualization engine is unavailable. Refresh the page after the client assets are updated.</div>';
        flowEdges.innerHTML = "";
        return;
      }

      const renderProjection = (projection, listEdges) => {
        flowRenderedProjection = projection;
        renderDone = Promise.resolve(engine.renderProjection(flowCanvas, projection, {
          escapeHtml,
          escapeHtmlAttr,
          predicateColor: flowPredicateColor,
          nodeCard: flowNodeCard,
        }));
        flowEdges.innerHTML = "";
      };

      if (flowDrilldown) {
        const projection = engine.buildDrilldownProjection(graph, flowDrilldown);
        renderProjection(projection, projection.edges);
      } else if (flowMode === "hierarchy") {
        const projection = engine.buildHierarchyProjection(graph);
        renderProjection(projection, projection.edges);
      } else if (flowMode === "full") {
        const projection = engine.buildFullConcentratedProjection(graph);
        renderProjection(projection, projection.edges);
      } else if (flowMode === "sections") {
        flowRenderedProjection = null;
        const sectionGroups = new Map();
        nodes.forEach((node) => {
          const key = node.section || "(no section)";
          if (!sectionGroups.has(key)) sectionGroups.set(key, { rows: 0, incoming: 0, outgoing: 0, external: 0 });
          const item = sectionGroups.get(key);
          item.rows += 1;
          item.incoming += Number(node.incoming_relationship_count || 0);
          item.outgoing += Number(node.outgoing_relationship_count || 0);
        });
        edges.filter((edge) => edge.is_external).forEach((edge) => {
          const source = nodeByAddress.get(edge.source_address_id);
          const item = sectionGroups.get(source?.section || "(no section)");
          if (item) item.external += 1;
        });
        renderFlowOverview("Section", Array.from(sectionGroups.entries()).map(([title, item]) => ({
          title,
          metrics: [
            { label: "Rows", value: item.rows },
            { label: "Incoming", value: item.incoming },
            { label: "Outgoing", value: item.outgoing },
            { label: "External", value: item.external },
          ],
        })));
        flowEdges.innerHTML = "";
      } else if (flowMode === "predicates") {
        const projection = engine.buildPredicateOverviewProjection(graph);
        renderProjection(projection, projection.groups.flatMap((group) => group.edges));
      } else if (flowMode === "external") {
        flowRenderedProjection = null;
        renderFlowExternal(edges, nodeByAddress);
        if (!edges.some((edge) => edge.is_external)) {
          flowCanvas.innerHTML = '<div class="note">No external or legacy targets are referenced by this instance.</div>';
        }
        flowEdges.innerHTML = "";
      } else if (flowMode === "lint") {
        flowRenderedProjection = null;
        renderFlowLint(nodes, edges, nodeByAddress, graph?.warnings);
      } else {
        const projection = engine.buildSwimlaneProjection(graph);
        flowRenderedProjection = projection;
        renderDone = Promise.resolve(engine.renderProjection(flowCanvas, projection, {
          escapeHtml,
          escapeHtmlAttr,
          predicateColor: flowPredicateColor,
          nodeCard: flowNodeCard,
        }));
        renderFlowEdgeList([...projection.partition.order, ...projection.partition.local], nodeByAddress);
      }
      renderDone.then(() => requestAnimationFrame(() => {
        if (flowMode === "swimlanes" && !flowDrilldown) drawFlowSwimlaneLines();
        wireFlowInteractions();
        applyFlowTrace();
      })).catch((err) => {
        flowCanvas.innerHTML = `<div class="note">Flow render failed: ${escapeHtml(err.message || String(err))}</div>`;
      });
    }

    async function loadFlowGraph() {
      if (activeView !== "flow") return;
      const checklist = (checklistSelect?.value || loadedChecklist || "").trim();
      const instanceId = (instanceSelect?.value || loadedInstance || getRootInstance()).trim();
      if (!checklist || !instanceId) return;
      try {
        const params = new URLSearchParams({ checklist, instance_id: instanceId });
        addWorkspaceQueryContext(params, checklist);
        const endpoint = flowMode === "workbench" ? "workbench" : "graph";
        const graph = await fetchJson(`/api/v1/visualizations/${endpoint}?${params.toString()}`);
        flowGraph = graph;
        renderFlowGraph(graph);
      } catch (err) {
        flowGraph = null;
        if (flowSummary) flowSummary.innerHTML = "";
        if (flowCanvas) {
          flowCanvas.innerHTML = `<div class="note">Flow load failed: ${escapeHtml(err.message || String(err))}</div>`;
        }
        if (flowEdges) flowEdges.innerHTML = "";
      }
    }

    async function exportFlowGraph() {
      const checklist = (checklistSelect?.value || loadedChecklist || "").trim();
      const instanceId = (instanceSelect?.value || loadedInstance || getRootInstance()).trim();
      if (!checklist || !instanceId) {
        alert("Select a checklist and instance first.");
        return;
      }
      const context = resolveScriptsContext(checklist);
      try {
        const payload = { checklist, instance_id: instanceId };
        if (context.sourceName) payload.source_name = context.sourceName;
        if (context.pack) payload.pack = context.pack;
        if (context.checklistDir) payload.checklist_dir = context.checklistDir;
        const result = await fetchJson("/api/v1/workspace/visualizations/export", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
        await loadFlowGraph();
        alert(`Flow exports (JSON, DOT, Mermaid, DBML, and Relationship Workbench) updated in ${result.directory || "the checklist visualizations folder"}.`);
      } catch (err) {
        alert(`Flow export failed: ${err.message || err}`);
      }
    }

    function validateDraft(row, section) {
      const errors = [];
      if (!section?.name?.trim()) errors.push("Section");
      if (!(row.checklist || loadedChecklist || checklistSelect.value || "").trim()) errors.push("Checklist");
      if (!(row.procedure || "").trim()) errors.push("Procedure");
      if (!(row.action || "").trim()) errors.push("Action");
      if (!(row.spec || "").trim()) errors.push("Spec");
      if (!(row.instructions || "").trim()) errors.push("Instructions");
      if (!resolveInstancePrincipal(loadedInstance)) errors.push("Instance principal");
      return errors;
    }

    async function saveDraft(rowId) {
      if (!canTemplateEdit()) return;
      const ctx = findRow(tableData, rowId);
      if (!ctx) return;
      const { row, section } = ctx;
      const errors = validateDraft(row, section);
      if (errors.length > 0) {
        alert(`Required: ${errors.join(", ")}`);
        return;
      }
      const checklistName = (row.checklist || loadedChecklist || checklistSelect.value || "").trim();
      const workspaceContext = resolveScriptsContext(checklistName);
      const body = {
        checklist: checklistName,
        section: section.name || row.section,
        procedure: row.procedure || "",
        action: row.action || "",
        spec: row.spec || "",
        instructions: row.instructions || "",
        instance_principal: resolveInstancePrincipal(loadedInstance),
        entity_id: activeEntity.id || "",
        status: normalizeMutableStatus(row.status),
        result: row.result || "",
        comment: row.comment || "",
      };
      if (workspaceContext.pack) {
        body.source_name = workspaceContext.sourceName || "";
        body.pack = workspaceContext.pack;
        body.checklist_dir = workspaceContext.checklistDir || checklistName;
      }
      try {
        setSaveState(row.addressId || row.uid, "pending", "Creating slug...");
        const data = await fetchJson("/api/v1/slugs", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
        });
        row.addressId = data.address_id || data.addressId || row.addressId || row.uid;
        row.slugId = data.slug_id || row.slugId || "";
        row.instanceId = data.instance_id || row.instanceId || "";
        row.section = section.name || row.section;
        row.checklist = checklistName;
        row.isNew = false;
        setSaveState(row.addressId, "saved", "Created");
        setIndicatorCell(row.addressId, row.status, row.comment);
        renderTable();
      } catch (err) {
        setSaveState(row.addressId || row.uid, "unsaved", err?.message || "Save failed");
        alert(`Create failed: ${err.message}`);
      }
    }

    function handleFieldChange(rowId, field, value) {
      const ctx = findRow(tableData, rowId);
      if (!ctx) return;
      ctx.row[field] = value;
      if (ctx.row.isNew) {
        setSaveState(ctx.row.uid, "unsaved", "Unsaved draft");
        return;
      }
      const isStateField = field === "status" || field === "result" || field === "comment";
      if (isStateField) {
        setIndicatorCell(ctx.row.addressId || ctx.row.uid, ctx.row.status, ctx.row.comment);
        queueSave(ctx.row);
      }
    }

    function handleStatusChange(rowId, value) {
      const ctx = findRow(tableData, rowId);
      if (!ctx) return;
      const previousStatus = normalizeMutableStatus(ctx.row.status);
      ctx.row.status = value;
      const nextStatus = normalizeMutableStatus(value);
      setIndicatorCell(ctx.row.addressId || ctx.row.uid, ctx.row.status, ctx.row.comment);
      if (ctx.row.isNew) {
        setSaveState(ctx.row.uid, "unsaved", "Unsaved draft");
        return;
      }
      const rerenderOnSuccess =
        !showNaRows && (previousStatus === "NA" || nextStatus === "NA");
      saveStatusNow(ctx.row, rerenderOnSuccess);
    }

    function handleSectionNameChange(sectionId, value) {
      const section = tableData.find((s) => s.uid === sectionId);
      if (!section) return;
      section.name = value;
      section.rows.forEach((row) => (row.section = value));
    }

    function createDetailsRowElement(row, section, stateAllowed, templateAllowed, detailVisible, hideNaRow) {
      const rowId = row.uid;
      const templateLocked = !templateAllowed || !row.isNew;
      row.templateRelationships = templateRelationshipsBySlug.get(row.slugId) || row.templateRelationships || [];
      const detailsRow = document.createElement("tr");
      detailsRow.className = "details-row";
      detailsRow.dataset.rowId = rowId;
      detailsRow.style.display = hideNaRow ? "none" : detailVisible ? "table-row" : "none";
      detailsRow.innerHTML = `
            <td colspan="8">
              <div class="details-box">
                <div><span class="chip">Checklist</span><div class="note">${escapeHtml(loadedChecklist || row.checklist || "")}</div></div>
                <div><span class="chip">Section</span><div class="note">${escapeHtml(section.name || row.section || "")}</div></div>
                <div><span class="chip">Address ID</span><div class="code-block"><code>${escapeHtml(row.addressId || "(unsaved)")}</code></div></div>
                <div><span class="chip">Slug ID</span><div class="code-block"><code>${escapeHtml(row.slugId || "(unsaved)")}</code></div></div>
                <div><span class="chip">Timestamp</span><div class="note">${escapeHtml(row.timestamp || "")}</div></div>
                <div><span class="chip">Procedure</span>${
                  templateLocked
                    ? `<div class="note">${escapeHtml(row.procedure || "")}</div>`
                    : `<input class="procedure-input" data-row="${rowId}" placeholder="Procedure" value="${escapeHtml(
                        row.procedure || ""
                      )}" required>`
                }</div>
                <div><span class="chip">Entity ID</span><div class="code-block"><code>${escapeHtml(row.entityId || "")}</code></div></div>
                <div class="full-span instructions-box"><span class="chip">Instructions</span>${
                  templateLocked
                    ? `<div class="markdown rendered">${renderMarkdownText(
                        row.instructions || "",
                        resolveLibraryAssetBase(row.checklist || loadedChecklist || "")
                      )}</div>`
                    : `<textarea class="instructions-input" data-row="${rowId}" placeholder="Instructions" required>${escapeHtml(
                        row.instructions || ""
                      )}</textarea>`
                }</div>
                <div class="full-span relationships-box"><span class="chip">Relationships</span>${renderRelationshipsPanel(row, stateAllowed)}</div>
              </div>
            </td>
          `;
      return detailsRow;
    }

    function renderTable() {
      slugTableBody.innerHTML = "";
      const templateAllowed = canTemplateEdit();
      const stateAllowed = canStateEdit();
      document.body.dataset.wrapRows = wrapRowText ? "1" : "0";

      tableData.forEach((section) => {
        const sectionRow = document.createElement("tr");
        sectionRow.className = "section-row";
        const sectionCell = document.createElement("td");
        sectionCell.colSpan = 8;
        const sectionLocked = !templateAllowed;
        if (!sectionLocked) {
          sectionCell.innerHTML = `
            <div class="section-title">
              <label>Section</label>
              <input class="section-input" data-section="${section.uid}" placeholder="Section title" value="${escapeHtml(section.name || "")}" ${sectionLocked ? "readonly" : ""} required>
              ${section.isNew ? '<span class="note">New section: first save will record name</span>' : ""}
            </div>`;
        } else {
          sectionCell.textContent = section.name || "(no section)";
        }
        sectionRow.appendChild(sectionCell);
        slugTableBody.appendChild(sectionRow);

        const headerRow = document.createElement("tr");
        headerRow.className = "section-header";
        headerRow.innerHTML = `
          <td style="width:90px;">Details</td>
          <td>Action</td><td>Spec</td><td>Result</td>
          <td>
            Status
            <span class="template-tag" title="Template checklists are the read-only primary copy of a checklist. Create an instance to edit.">ℹ Template</span>
          </td>
          <td>Comment</td><td style="width:70px;">Ind</td><td style="width:90px;">State</td>
        `;
        const statusTag = headerRow.querySelector(".template-tag");
        if (statusTag) {
          statusTag.classList.toggle("hidden", !isRootInstance(loadedInstance));
        }
        slugTableBody.appendChild(headerRow);

        section.rows.forEach((row) => {
          const rowId = row.uid;
          const rowKey = row.addressId || row.uid;
          const hideNaRow = !showNaRows && !row.isNew && normalizeMutableStatus(row.status) === "NA";
          const rowEl = document.createElement("tr");
          rowEl.className = "data-row";
          rowEl.dataset.rowId = rowId;
          rowEl.dataset.sectionId = section.uid;
          const templateLocked = !templateAllowed || !row.isNew;
          const stateLocked = !stateAllowed;
          const lockedAttr = templateLocked ? "readonly" : "";
          const stateReadOnly = stateLocked ? "readonly" : "";
          const stateDisabled = stateLocked ? "disabled" : "";
          const actionField = wrapRowText
            ? `<textarea class="action-input row-cell-wrap" data-row="${rowId}" ${lockedAttr} placeholder="Action" ${!templateLocked ? "required" : ""}>${escapeHtml(row.action || "")}</textarea>`
            : `<input class="action-input row-cell-input" data-row="${rowId}" ${lockedAttr} placeholder="Action" value="${escapeHtml(row.action || "")}" ${!templateLocked ? "required" : ""}>`;
          const specField = wrapRowText
            ? `<textarea class="spec-input row-cell-wrap" data-row="${rowId}" ${lockedAttr} placeholder="Spec" title="${escapeHtmlAttr(SPEC_INPUT_HINT)}" ${!templateLocked ? "required" : ""}>${escapeHtml(row.spec || "")}</textarea>`
            : `<input class="spec-input row-cell-input" data-row="${rowId}" ${lockedAttr} placeholder="Spec" title="${escapeHtmlAttr(SPEC_INPUT_HINT)}" value="${escapeHtml(row.spec || "")}" ${!templateLocked ? "required" : ""}>`;
          const resultField = wrapRowText
            ? `<textarea class="row-result row-cell-wrap" data-row="${rowId}" ${stateReadOnly} placeholder="Result">${escapeHtml(row.result || "")}</textarea>`
            : `<input class="row-result row-cell-input" data-row="${rowId}" ${stateReadOnly} placeholder="Result" value="${escapeHtml(row.result || "")}">`;
          const commentField = wrapRowText
            ? `<textarea class="row-comment row-cell-wrap" data-row="${rowId}" ${stateReadOnly} placeholder="Comment">${escapeHtml(row.comment || "")}</textarea>`
            : `<input class="row-comment row-cell-input" data-row="${rowId}" ${stateReadOnly} placeholder="Comment" value="${escapeHtml(row.comment || "")}">`;
          const verifyIndicator = row.verifyIndicator || defaultVerifyIndicator();
          rowEl.innerHTML = `
            <td><button class="secondary toggle-details" data-row="${rowId}">Details</button></td>
            <td>${actionField}</td>
            <td>${specField}</td>
            <td><div class="result-cell">${resultField}<span class="tiny-indicator result-verify-ind ${verifyIndicator.variant}" data-id="${rowKey}" title="${escapeHtmlAttr(verifyIndicator.tooltip || "")}"></span></div></td>
            <td>
              <div class="radio-group" data-row="${rowId}">
                ${["Pass", "Fail", "NA", "Other"]
                  .map(
                    (s) =>
                      `<label><input type="radio" name="status-${rowId}" value="${s}" data-row="${rowId}" ${row.status === s ? "checked" : ""} ${stateDisabled}>${s}</label>`
                  )
                  .join("")}
              </div>
            </td>
            <td>${commentField}</td>
            <td class="ind-cell" data-id="${rowKey}"></td>
            <td class="state-cell" data-id="${rowKey}" title="${row.isNew ? "Draft" : "Saved"}">${
              row.isNew
                ? templateAllowed
                  ? `<div class="draft-state"><button class="save-draft" data-row="${rowId}">Save</button><div class="note draft-note">Required before saving</div></div>`
                  : `<div class="note">Draft locked</div>`
                : SAVE_ICONS.saved
            }</td>
          `;
          rowEl.style.display = hideNaRow ? "none" : "table-row";
          slugTableBody.appendChild(rowEl);

          if (row.isNew) {
            const detailsRow = createDetailsRowElement(row, section, stateAllowed, templateAllowed, true, hideNaRow);
            slugTableBody.appendChild(detailsRow);
            const toggleBtn = rowEl.querySelector(".toggle-details");
            if (toggleBtn) toggleBtn.textContent = "Hide";
          }

          setIndicatorCell(rowKey, row.status, row.comment);
          setResultVerifyIndicatorCell(rowKey, row.verifyIndicator || defaultVerifyIndicator());
          if (row.isNew) {
            setSaveState(rowKey, "unsaved", "Unsaved draft");
          } else {
            setSaveState(rowKey, "saved", "Saved");
          }

          if (templateAllowed) {
            const addRow = document.createElement("tr");
            addRow.className = "add-row-row";
            addRow.innerHTML = `<td colspan="8"><button class="secondary add-row-after" data-section="${section.uid}" data-after="${rowId}">Add Row Below</button></td>`;
            addRow.style.display = hideNaRow ? "none" : "table-row";
            slugTableBody.appendChild(addRow);
          }
        });
      });

      if (templateAllowed) {
        const sectionFooter = document.createElement("tr");
        sectionFooter.className = "section-footer";
        sectionFooter.innerHTML = `<td colspan="8"><button class="secondary new-section-trigger">New Section</button></td>`;
        slugTableBody.appendChild(sectionFooter);
      }

      slugTableBody.querySelectorAll(".rel-editor").forEach((el) => {
        updateRelationshipEditor(el.dataset.row);
      });
    }

    function updateRelationshipEditor(rowId) {
      if (!rowId) return;
      const ctx = findRow(tableData, rowId);
      if (!ctx) return;
      const { row } = ctx;
      const predicateInput = slugTableBody.querySelector(`input.rel-predicate[data-row='${rowId}']`);
      const subjectStateSel = slugTableBody.querySelector(`select.rel-subject-state[data-row='${rowId}']`);
      const relationSel = slugTableBody.querySelector(`select.rel-relation[data-row='${rowId}']`);
      const typeSel = slugTableBody.querySelector(`select.rel-type[data-row='${rowId}']`);
      const objectStateSel = slugTableBody.querySelector(`select.rel-object-state[data-row='${rowId}']`);
      const targetInput = slugTableBody.querySelector(`input.rel-target[data-row='${rowId}']`);
      const addBtn = slugTableBody.querySelector(`button.rel-add[data-row='${rowId}']`);
      const predInd = slugTableBody.querySelector(`.rel-predicate-ind[data-row='${rowId}']`);
      const targetInd = slugTableBody.querySelector(`.rel-target-ind[data-row='${rowId}']`);
      const hintEl = slugTableBody.querySelector(`.rel-hint[data-row='${rowId}']`);
      const draft = getRelDraft(rowId);

      if (subjectStateSel && subjectStateSel.value !== draft.subjectState) {
        draft.subjectState = subjectStateSel.value;
      }
      if (relationSel && relationSel.value !== draft.relation) {
        draft.relation = relationSel.value;
      }
      if (typeSel && typeSel.value !== draft.type) {
        draft.type = typeSel.value;
      }
      if (objectStateSel && objectStateSel.value !== draft.objectState) {
        draft.objectState = objectStateSel.value;
      }

      if (predicateInput && predicateInput.value !== draft.predicate) {
        draft.predicate = predicateInput.value;
      }
      if (targetInput && targetInput.value !== draft.target) {
        draft.target = targetInput.value;
      }

      if (draft.lastEdit === "slots") {
        const built = buildCanonicalPredicateToken(
          draft.subjectState,
          draft.relation,
          draft.type,
          draft.objectState
        );
        if (built) {
          draft.predicate = built;
          if (predicateInput && predicateInput.value !== built) predicateInput.value = built;
        }
      } else {
        const parsed = parseCanonicalPredicateToken(draft.predicate);
        const slotParsed = parsed ? null : parseSlotPredicateToken(draft.predicate);
        const resolved = parsed || slotParsed;
        if (resolved) {
          draft.subjectState = resolved.subjectState;
          draft.relation = resolved.relation;
          draft.type = resolved.type;
          draft.objectState = resolved.objectState;
          if (subjectStateSel && subjectStateSel.value !== resolved.subjectState) subjectStateSel.value = resolved.subjectState;
          if (relationSel && relationSel.value !== resolved.relation) relationSel.value = resolved.relation;
          if (typeSel && typeSel.value !== resolved.type) typeSel.value = resolved.type;
          if (objectStateSel && objectStateSel.value !== resolved.objectState) objectStateSel.value = resolved.objectState;
        } else if (String(draft.predicate || "").trim()) {
          if (subjectStateSel && subjectStateSel.value) subjectStateSel.value = "";
          if (relationSel && relationSel.value) relationSel.value = "";
          if (typeSel && typeSel.value) typeSel.value = "";
          if (objectStateSel && objectStateSel.value) objectStateSel.value = "";
        }
      }

      const pred = predicateIndicator(draft.predicate);
      setTinyIndicator(predInd, pred.variant, pred.tooltip);

      const target = targetIndicator(draft.target, row);
      const seq = (relValidationSeq.get(rowId) || 0) + 1;
      relValidationSeq.set(rowId, seq);

      if (!target.resolved) {
        setTinyIndicator(targetInd, target.variant, target.tooltip);
        if (hintEl) {
          hintEl.textContent = row.addressId
            ? "Target accepts a 32-char Address ID, or a 16-char Slug ID (auto-expands to same-instance address)."
            : "Save this row first to create relationships.";
        }
        if (addBtn) addBtn.disabled = true;
        return;
      }

      if (hintEl) {
        hintEl.textContent = `${target.kind === "slug" ? "Slug ID" : "Address ID"} resolves to ${target.resolved}`;
      }

      const stateAllowed = canStateEdit();
      if (!stateAllowed || !row.addressId) {
        setTinyIndicator(targetInd, "ind-muted", "Editing disabled.");
        if (addBtn) addBtn.disabled = true;
        return;
      }
      const predicateToken = String(draft.predicate || "").trim();
      if (!predicateToken) {
        setTinyIndicator(targetInd, target.variant, target.tooltip);
        if (addBtn) addBtn.disabled = true;
        return;
      }
      if (pred.variant === "ind-bad") {
        setTinyIndicator(targetInd, target.variant, target.tooltip);
        if (addBtn) addBtn.disabled = true;
        return;
      }

      if (knownAddressIds.has(target.resolved)) {
        setTinyIndicator(targetInd, target.variant, "Target address exists (loaded).");
        if (addBtn) addBtn.disabled = false;
        return;
      }

      setTinyIndicator(targetInd, "ind-muted", "Checking target existence...");
      if (addBtn) addBtn.disabled = true;
      checkAddressExists(target.resolved)
        .then((exists) => {
          if ((relValidationSeq.get(rowId) || 0) !== seq) return;
          if (exists) {
            setTinyIndicator(targetInd, target.variant, "Target address exists.");
            if (addBtn) addBtn.disabled = false;
            return;
          }
          setTinyIndicator(targetInd, "ind-bad", "Target address not found (dead link).");
          if (addBtn) addBtn.disabled = true;
        })
        .catch((err) => {
          if ((relValidationSeq.get(rowId) || 0) !== seq) return;
          setTinyIndicator(targetInd, "ind-bad", `Lookup failed: ${(err && err.message) || err}`);
          if (addBtn) addBtn.disabled = true;
        });
    }

    async function refreshOutgoingRelationships(rowId) {
      const ctx = findRow(tableData, rowId);
      if (!ctx) return;
      const { row } = ctx;
      if (!row.addressId) return;
      const payload = await fetchJson(`/api/v1/relationships/address/${row.addressId}`);
      const outgoing = payload.outgoing || [];
      const incoming = payload.incoming || [];
      row.relationships = outgoing.map((e) => ({ predicate: e.predicate || "", target: e.target || "" }));
      row.incomingRelationships = incoming.map((e) => ({
        predicate: e.predicate || "",
        source: e.source || e.target || "",
      }));
      row.prefillDataset = payload.prefill_dataset || null;
      const listEl = slugTableBody.querySelector(`.relationships-list[data-row='${rowId}']`);
      if (listEl) {
        listEl.innerHTML = renderRelationships(row.relationships || [], null, row.prefillDataset);
      }
      const incomingListEl = slugTableBody.querySelector(`.relationships-list.incoming-relationships[data-row='${rowId}']`);
      if (incomingListEl) {
        incomingListEl.innerHTML = renderIncomingRelationships(row.incomingRelationships || [], null);
      }
    }

    async function addRelationshipFromEditor(rowId) {
      const ctx = findRow(tableData, rowId);
      if (!ctx) return;
      const { row } = ctx;
      if (!row.addressId) {
        alert("Save this row first to create relationships.");
        return;
      }
      if (!canStateEdit()) return;

      const predicateInput = slugTableBody.querySelector(`input.rel-predicate[data-row='${rowId}']`);
      const targetInput = slugTableBody.querySelector(`input.rel-target[data-row='${rowId}']`);
      const hintEl = slugTableBody.querySelector(`.rel-hint[data-row='${rowId}']`);
      const draft = getRelDraft(rowId);
      draft.predicate = (predicateInput?.value || draft.predicate || "").trim();
      draft.target = (targetInput?.value || draft.target || "").trim();
      if (predicateInput) predicateInput.value = draft.predicate;

      const target = targetIndicator(draft.target, row);
      if (!draft.predicate) {
        alert("Predicate is required.");
        updateRelationshipEditor(rowId);
        return;
      }
      if (!isValidPredicateToken(draft.predicate)) {
        alert("Invalid predicate token. Expected [A-Za-z][A-Za-z0-9_]{0,127} (ASCII, case-sensitive).");
        updateRelationshipEditor(rowId);
        return;
      }
      if (!target.resolved) {
        alert("Target must be a 16-char Slug ID or 32-char Address ID (Base32).");
        updateRelationshipEditor(rowId);
        return;
      }
      let exists = knownAddressIds.has(target.resolved);
      if (!exists) {
        exists = await checkAddressExists(target.resolved);
      }
      if (!exists) {
        alert("Target address not found (dead link). Create/instantiate it first.");
        updateRelationshipEditor(rowId);
        return;
      }

      try {
        if (hintEl) hintEl.textContent = "Saving relationship...";
        await fetchJson("/api/v1/relationships/address", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            subject_address_id: row.addressId,
            predicate: draft.predicate,
            target_address_id: target.resolved,
          }),
        });
        draft.target = "";
        if (targetInput) targetInput.value = "";
        await refreshOutgoingRelationships(rowId);
        const targetRow = findRowByAddress(tableData, target.resolved);
        if (targetRow?.uid && targetRow.uid !== rowId) {
          await refreshOutgoingRelationships(targetRow.uid);
        }
        refreshPredicateCatalog();
        if (hintEl) hintEl.textContent = `${draft.predicate} \u2192 ${target.resolved} saved.`;
      } catch (err) {
        if (hintEl) hintEl.textContent = `Save failed: ${(err && err.message) || err}`;
        alert(`Relationship save failed: ${(err && err.message) || err}`);
      } finally {
        updateRelationshipEditor(rowId);
      }
    }

    // Event wiring
    ["wheel", "touchmove", "mousedown", "scroll"].forEach((eventName) => {
      window.addEventListener(eventName, markUserInteraction, { passive: true });
    });
    window.addEventListener("keydown", markUserInteraction);

    slugTableBody.addEventListener("click", (e) => {
      const target = e.target;
      if (target.classList.contains("toggle-details")) {
        const rowId = target.dataset.row;
        let detailRow = slugTableBody.querySelector(`tr.details-row[data-row-id='${rowId}']`);
        if (!detailRow) {
          const ctx = findRow(tableData, rowId);
          if (ctx) {
            const hideNaRow = !showNaRows && !ctx.row.isNew && normalizeMutableStatus(ctx.row.status) === "NA";
            const rowEl = target.closest("tr.data-row");
            detailRow = createDetailsRowElement(
              ctx.row,
              ctx.section,
              canStateEdit(),
              canTemplateEdit(),
              false,
              hideNaRow
            );
            if (rowEl?.parentNode) {
              rowEl.parentNode.insertBefore(detailRow, rowEl.nextSibling);
            }
          }
        }
        if (detailRow) {
          const newDisplay = detailRow.style.display === "none" ? "table-row" : "none";
          detailRow.style.display = newDisplay;
          target.textContent = newDisplay === "table-row" ? "Hide" : "Details";
          if (newDisplay === "table-row") {
            updateRelationshipEditor(rowId);
            refreshOutgoingRelationships(rowId).catch(() => {});
            const ctx = findRow(tableData, rowId);
            const checklistName = (ctx?.row?.checklist || loadedChecklist || "").trim();
            ensureTemplateRelationshipsLoaded(checklistName)
              .then(() => {
                const nextCtx = findRow(tableData, rowId);
                if (!nextCtx) return;
                nextCtx.row.templateRelationships =
                  templateRelationshipsBySlug.get(nextCtx.row.slugId) || [];
                const templateListEl = slugTableBody.querySelector(
                  `.relationships-list.template-relationships[data-row='${rowId}']`
                );
                if (templateListEl) {
                  templateListEl.innerHTML = renderRelationships(
                    nextCtx.row.templateRelationships || [],
                    "No template relationships."
                  );
                }
              })
              .catch(() => {});
          }
        }
      } else if (target.classList.contains("add-row-after")) {
        const sectionId = target.dataset.section;
        const afterRow = target.dataset.after;
        addDraftRow(sectionId, afterRow);
      } else if (target.classList.contains("save-draft")) {
        const rowId = target.dataset.row;
        saveDraft(rowId);
      } else if (target.classList.contains("rel-add")) {
        addRelationshipFromEditor(target.dataset.row);
      } else if (target.classList.contains("new-section-trigger")) {
        addNewSection();
      }
    });

    slugTableBody.addEventListener("input", (e) => {
      const target = e.target;
      const rowId = target.dataset.row;
      if (target.classList.contains("action-input")) handleFieldChange(rowId, "action", target.value);
      else if (target.classList.contains("spec-input")) handleFieldChange(rowId, "spec", target.value);
      else if (target.classList.contains("row-result")) handleFieldChange(rowId, "result", target.value);
      else if (target.classList.contains("row-comment")) handleFieldChange(rowId, "comment", target.value);
      else if (target.classList.contains("procedure-input")) handleFieldChange(rowId, "procedure", target.value);
      else if (target.classList.contains("instructions-input")) handleFieldChange(rowId, "instructions", target.value);
      else if (target.classList.contains("section-input")) handleSectionNameChange(target.dataset.section, target.value);
      else if (target.classList.contains("rel-predicate")) {
        const draft = getRelDraft(rowId);
        draft.lastEdit = "token";
        updateRelationshipEditor(rowId);
      }
      else if (target.classList.contains("rel-target")) updateRelationshipEditor(rowId);
    });

    slugTableBody.addEventListener("change", (e) => {
      const target = e.target;
      if (target.name && target.name.startsWith("status-")) {
        handleStatusChange(target.dataset.row, target.value);
      } else if (target.classList.contains("rel-slot")) {
        const rowId = target.dataset.row;
        const draft = getRelDraft(rowId);
        draft.lastEdit = "slots";
        updateRelationshipEditor(rowId);
      }
    });

    modeSelect.addEventListener("change", () => {
      state.mode = modeSelect.value;
      if (state.mode === "guest") {
        state.editEnabled = false;
      } else if (state.mode !== "service") {
        state.editEnabled = true;
      }
      if (editToggle) {
        editToggle.disabled = state.mode !== "service";
        editToggle.checked = state.editEnabled;
      }
      renderTable();
      updateModeHint();
    });

    editToggle.addEventListener("change", () => {
      state.editEnabled = editToggle.checked;
      renderTable();
      updateModeHint();
    });

    showNaRowsToggle?.addEventListener("change", () => {
      showNaRows = !!showNaRowsToggle.checked;
      if (showNaRows) {
        window.localStorage?.setItem(SHOW_NA_ROWS_KEY, "1");
      } else {
        window.localStorage?.removeItem(SHOW_NA_ROWS_KEY);
      }
      renderTable();
    });

    wrapRowTextToggle?.addEventListener("change", () => {
      wrapRowText = !!wrapRowTextToggle.checked;
      if (wrapRowText) {
        window.localStorage?.setItem(WRAP_ROW_TEXT_KEY, "1");
      } else {
        window.localStorage?.removeItem(WRAP_ROW_TEXT_KEY);
      }
      renderTable();
    });

    healthBtn?.addEventListener("click", checkHealth);
    listBtn?.addEventListener("click", listSlugs);
    checklistFilter.addEventListener("input", (e) => updateChecklistOptions(e.target.value || ""));
    checklistFilter.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        tryCreateChecklistFromFilter();
      }
    });
    checklistFilter.addEventListener("blur", () => {
      tryCreateChecklistFromFilter();
    });
    checklistSelect.addEventListener("change", async () => {
      updateLoadButton();
      await loadInstancesForChecklist(checklistSelect.value || "");
      await listSlugs();
      syncMarkdownFields();
      refreshScripts();
      if (activeView === "flow") loadFlowGraph();
    });
    instanceFilter?.addEventListener("input", (e) => renderInstanceOptions(e.target.value || ""));
    instanceFilter?.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        tryCreateInstanceFromFilter();
      }
    });
    instanceFilter?.addEventListener("blur", () => {
      tryCreateInstanceFromFilter();
    });
    instanceSelect?.addEventListener("change", () => {
      updateInstanceInputs(instanceSelect.value);
      listSlugs();
      if (activeView === "flow") loadFlowGraph();
    });
    checklistViewTab?.addEventListener("click", () => {
      setActiveView("checklist");
    });
    flowViewTab?.addEventListener("click", () => {
      setActiveView("flow");
    });
    refreshFlowBtn?.addEventListener("click", () => {
      loadFlowGraph();
    });
    exportFlowBtn?.addEventListener("click", () => {
      exportFlowGraph();
    });
    flowModeSelect?.addEventListener("change", () => {
      flowMode = flowModeSelect.value || "swimlanes";
      flowDrilldown = null;
      flowPreviousMode = null;
      if (activeView === "flow") loadFlowGraph();
    });
    flowFilterInput?.addEventListener("input", () => {
      flowFocus = null;
      applyFlowTrace();
    });
    window.addEventListener("resize", () => {
      if (activeView === "flow" && flowMode === "swimlanes") {
        requestAnimationFrame(drawFlowSwimlaneLines);
      }
    });
    flowSummary?.addEventListener("click", (event) => {
      if (!event.target.closest("[data-flow-back]")) return;
      flowDrilldown = null;
      flowMode = flowPreviousMode || flowMode;
      flowPreviousMode = null;
      if (flowModeSelect) flowModeSelect.value = flowMode;
      if (flowGraph) renderFlowGraph(flowGraph);
    });
    flowContextMenu?.addEventListener("click", (event) => {
      const button = event.target.closest("button[data-flow-action]");
      if (!button || !flowSelectedMeta) return;
      const action = button.dataset.flowAction;
      hideFlowContextMenu();
      if (action === "drill") {
        if (!flowDrilldown) flowPreviousMode = flowMode;
        flowDrilldown = flowSelectedMeta;
        renderFlowGraph(flowGraph);
      } else if (action === "family") {
        flowFocus = focusForFlowMeta(flowSelectedMeta);
        applyFlowTrace();
      } else if (action === "slug") {
        const node = flowSelectedMeta.type === "node"
          ? flowSelectedMeta.raw
          : (flowGraph?.nodes || []).find((item) => item.address_id === flowSelectedMeta.raw.target_address_id);
        if (flowFilterInput) flowFilterInput.value = node?.slug_id || node?.address_id || "";
        flowFocus = null;
        applyFlowTrace();
      } else if (action === "copy") {
        const text = flowMetaText(flowSelectedMeta);
        if (navigator.clipboard?.writeText) {
          navigator.clipboard.writeText(text).catch(() => {});
        } else {
          const area = document.createElement("textarea");
          area.value = text;
          document.body.appendChild(area);
          area.select();
          document.execCommand("copy");
          area.remove();
        }
      }
    });
    document.addEventListener("click", (event) => {
      if (!flowContextMenu?.contains(event.target)) hideFlowContextMenu();
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") hideFlowContextMenu();
    });
    entityToggle?.addEventListener("click", () => {
      if (!entityPanel) return;
      entityPanel.style.display = entityPanel.style.display === "block" ? "none" : "block";
    });
    if (DEV_MODE) {
      refreshEntitiesBtn?.addEventListener("click", fetchEntities);
      useSelectedEntityBtn?.addEventListener("click", () => {
        if (!entitySelect) return;
        const selected = entitySelect.options[entitySelect.selectedIndex];
        setActiveEntity(selected?.value || "", selected?.dataset.principal || "");
      });
      simulateLoginBtn?.addEventListener("click", async () => {
        const principal = (entityPrincipalInput?.value || "").trim();
        const displayName = (entityDisplayInput?.value || "").trim();
        if (!principal) {
          alert("Entity principal is required.");
          return;
        }
        try {
          const body = { principal, kind: "user", display_name: displayName };
          const data = await fetchJson("/api/v1/entities", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(body),
          });
          const entityId = data.entity_id || data.id || "";
          setActiveEntity(entityId, principal);
          await fetchEntities();
        } catch (err) {
          alert(`Entity create failed: ${err.message}`);
        }
      });
    }
    oauthLoginBtn?.addEventListener("click", startOAuthLogin);
    oauthLogoutBtn?.addEventListener("click", () => {
      logout();
    });
    hostInput?.addEventListener("change", () => {
      const val = (hostInput.value || "").trim();
      if (val && window.localStorage) window.localStorage.setItem(HOST_KEY, val);
    });
    portInput?.addEventListener("change", () => {
      const val = (portInput.value || "").trim();
      if (val && window.localStorage) window.localStorage.setItem(PORT_KEY, val);
    });
    document.getElementById("rootInstanceBtn")?.addEventListener("click", () => {
      if (rootInstanceId && !instanceOptions.includes(rootInstanceId)) {
        instanceOptions.push(rootInstanceId);
      }
      updateInstanceInputs(getRootInstance());
      renderInstanceOptions(instanceFilter?.value || "");
      listSlugs();
    });
    newChecklistBtn?.addEventListener("click", startNewChecklist);
    refreshMdWorkspaceBtn?.addEventListener("click", () => {
      refreshMdWorkspace();
    });
    mdWorkspaceSelect?.addEventListener("change", () => {
      updateMdWorkspaceInfo();
      syncMarkdownFields();
      refreshScripts();
    });
    jsonlImportFile?.addEventListener("change", () => {
      refreshJsonlImportInfo();
    });
    openMdWorkspaceDirBtn?.addEventListener("click", () => {
      openMdWorkspaceDir();
    });
    copyMdWorkspaceDirBtn?.addEventListener("click", () => {
      copyToClipboard(mdTemplatesRoot || "", "Templates folder path");
    });
    refreshScriptsBtn?.addEventListener("click", () => {
      refreshScripts();
    });
    scriptsSelect?.addEventListener("change", () => {
      updateScriptsInfo();
    });
    openScriptsDirBtn?.addEventListener("click", () => {
      openScriptsDir();
    });
    copyScriptsDirBtn?.addEventListener("click", () => {
      copyToClipboard(scriptsRoot || "", "Scripts folder path");
    });
    runScriptBtn?.addEventListener("click", () => {
      runSelectedScript();
    });
    stopScriptBtn?.addEventListener("click", () => {
      stopSelectedScript();
    });
    importMdWorkspaceBtn?.addEventListener("click", () => {
      importMdWorkspaceSelected();
    });
    exportMdWorkspaceBtn?.addEventListener("click", () => {
      exportMdWorkspaceCurrent();
    });
    importJsonlBtn?.addEventListener("click", () => {
      importJsonlFile();
    });
    generateReportBtn?.addEventListener("click", () => {
      generateReport();
    });
    generateHtmlReportBtn?.addEventListener("click", () => {
      generateReport(undefined, undefined, "html");
    });
    openReportDirBtn?.addEventListener("click", () => {
      openReportDir();
    });
    checklistCopyBtn?.addEventListener("click", () => {
      copyToClipboard(checklistSelect?.value || "", "Checklist id");
    });
    checklistDeleteBtn?.addEventListener("click", async () => {
      const name = (checklistSelect?.value || "").trim();
      if (!name) return;
      const ok = confirm(`Delete checklist "${name}" and all its instances? This cannot be undone.`);
      if (!ok) return;
      try {
        await fetchJson(`/api/v1/checklists/${encodeURIComponent(name)}`, { method: "DELETE" });
        await loadChecklists();
        if (checklistSelect?.value) {
          await loadInstancesForChecklist(checklistSelect.value);
          await listSlugs();
        }
        alert(`Checklist "${name}" deleted.`);
      } catch (err) {
        alert(`Delete failed: ${err.message}`);
      }
    });
    instanceCopyBtn?.addEventListener("click", () => {
      copyToClipboard(instanceSelect?.value || "", "Instance id");
    });
    instanceDeleteBtn?.addEventListener("click", async () => {
      const checklistName = (checklistSelect?.value || "").trim();
      const instanceId = (instanceSelect?.value || "").trim();
      if (!checklistName || !instanceId) return;
      if (isRootInstance(instanceId)) {
        alert("Root/template instance cannot be deleted.");
        return;
      }
      const ok = confirm(`Delete instance "${instanceId}" for checklist "${checklistName}"?`);
      if (!ok) return;
      try {
        await fetchJson(
          `/api/v1/checklists/${encodeURIComponent(checklistName)}/instances/${encodeURIComponent(instanceId)}`,
          { method: "DELETE" }
        );
        await loadInstancesForChecklist(checklistName);
        await listSlugs();
        alert(`Instance "${instanceId}" deleted.`);
      } catch (err) {
        alert(`Delete failed: ${err.message}`);
      }
    });

    // Initial load
    updateModeHint();
    updateSessionPanel();
    if (isTokenValid()) {
      fetchMe();
    }
    if (healthStatus) checkHealth();
    refreshMdWorkspace();
    refreshScripts();
    refreshJsonlImportInfo();
    loadChecklists().then(async () => {
      await bootstrapInitialSetupChecklist();
      if (initialSelectionFromUrl.checklist && allChecklists.includes(initialSelectionFromUrl.checklist)) {
        checklistSelect.value = initialSelectionFromUrl.checklist;
        await loadInstancesForChecklist(initialSelectionFromUrl.checklist);
      }
      if (initialSelectionFromUrl.instance) {
        const requestedInstance = isValidBase32Id(initialSelectionFromUrl.instance, 16)
          ? normalizeBase32Id(initialSelectionFromUrl.instance)
          : initialSelectionFromUrl.instance;
        let canApplyRequested = instanceOptions.includes(requestedInstance);
        if (!canApplyRequested && checklistSelect?.value && requestedInstance) {
          try {
            const probeParams = new URLSearchParams();
            probeParams.set("checklist", checklistSelect.value);
            addWorkspaceQueryContext(probeParams, checklistSelect.value);
            probeParams.set("instance_id", requestedInstance);
            const probe = await fetchJson(`/api/v1/slugs?${probeParams.toString()}`);
            const probeItems = probe.items || [];
            const found = probeItems.find((s) => (s.instance_id || "") === requestedInstance);
            if (found) {
              canApplyRequested = true;
              if (!instanceOptions.includes(requestedInstance)) {
                instanceOptions.push(requestedInstance);
              }
              if (found.instance_principal) {
                recordPrincipal(requestedInstance, found.instance_principal);
              }
            }
          } catch {
            // Keep fallback behavior when probe fails.
          }
        }
        if (canApplyRequested) {
          updateInstanceInputs(requestedInstance);
          renderInstanceOptions("");
        }
      }
      updateLoadButton();
      await listSlugs();
      await refreshScripts();
      showInitialSetupTour();
      if (DEV_MODE) {
        fetchEntities();
      }
    });
    startAutoRefresh();
    }

  window.checklistPrototype = {
    init: checklistPrototypeInit,
  };
})();
