// Lightweight import/export module with a single IR and registry-based format mapping.
// Formats supported now: markdown, jsonl. Future formats are stubbed.
(function () {
  const ENABLED_FORMATS = new Set(["markdown", "jsonl"]);
  const STUB_FORMATS = ["json", "xml", "api_text"];
  const DEFAULT_ROOT = "template||default";

  function clone(obj) {
    return JSON.parse(JSON.stringify(obj || {}));
  }

  function emptyTemplate(checklist, principal) {
    return { checklist: checklist || "", instance_principal: principal || DEFAULT_ROOT, sections: [] };
  }

  function emptyRun(checklist, instanceId, principal) {
    return { checklist: checklist || "", instance_id: instanceId || "", instance_principal: principal || "", rows: [] };
  }

  // ---------- IR helpers ----------
  function apiPayloadToIr(templateSlugs, runSlugs, opts = {}) {
    const checklist = opts.checklist || "";
    const rootPrincipal = (opts.rootPrincipal || DEFAULT_ROOT).toLowerCase();
    const templateIR = emptyTemplate(checklist, opts.rootPrincipal || DEFAULT_ROOT);
    const runIR = emptyRun(checklist, opts.instanceId || "", opts.instancePrincipal || "");

    const sectionMap = new Map();
    (templateSlugs || []).forEach((s) => {
      const sec = s.section || "(no section)";
      if (!sectionMap.has(sec)) {
        sectionMap.set(sec, { name: sec, rows: [] });
        templateIR.sections.push(sectionMap.get(sec));
      }
      sectionMap.get(sec).rows.push({
        slug_id: s.slug_id || "",
        procedure: s.procedure || "",
        action: s.action || "",
        spec: s.spec || "",
        instructions: s.instructions || "",
        checklist: s.checklist || checklist,
        section: sec,
        instance_principal: s.instance_principal || templateIR.instance_principal,
      });
      if (s.instance_principal && s.instance_principal.toLowerCase() === rootPrincipal) {
        templateIR.instance_principal = s.instance_principal;
      }
    });

    (runSlugs || []).forEach((s) => {
      runIR.rows.push({
        address_id: s.address_id || "",
        slug_id: s.slug_id || "",
        status: s.status || "",
        result: s.result || "",
        comment: s.comment || "",
        entity_id: s.entity_id || "",
        instance_id: s.instance_id || runIR.instance_id || "",
        instance_principal: s.instance_principal || runIR.instance_principal || "",
        timestamp: s.timestamp || "",
      });
      if (!runIR.instance_id && s.instance_id) runIR.instance_id = s.instance_id;
      if (!runIR.instance_principal && s.instance_principal) runIR.instance_principal = s.instance_principal;
    });

    return { template: templateIR, run: runIR };
  }

  function irToBatchCalls(templateIR, runIR, opts = {}) {
    const rootPrincipal = opts.rootPrincipal || DEFAULT_ROOT;
    const creates = [];
    const updates = [];
    const warnings = [];

    (templateIR?.sections || []).forEach((sec) => {
      (sec.rows || []).forEach((r) => {
        const body = {
          checklist: templateIR.checklist || r.checklist || "",
          section: sec.name || r.section || "",
          procedure: r.procedure || "",
          action: r.action || "",
          spec: r.spec || "",
          instructions: r.instructions || "",
          instance_principal: templateIR.instance_principal || rootPrincipal,
        };
        if (r.slug_id) body.slug_id = r.slug_id;
        creates.push(body);
      });
    });

    (runIR?.rows || []).forEach((r) => {
      if (!r.address_id) {
        warnings.push(`Skipping update with missing address_id (slug_id=${r.slug_id || ""})`);
        return;
      }
      updates.push({
        address_id: r.address_id,
        status: r.status,
        result: r.result,
        comment: r.comment,
        entity_id: r.entity_id,
      });
    });

    return { creates, updates, warnings };
  }

  // ---------- Format translators ----------
  function importMarkdown(text, opts = {}) {
    const lines = (text || "").split(/\r?\n/);
    const warnings = [];
    let checklist = (opts.defaultChecklist || "").trim();
    let currentSection = null;
    let currentRow = null;
    const tpl = emptyTemplate(checklist, opts.defaultInstancePrincipal || DEFAULT_ROOT);
    let headingMismatch = false;

    function commitRow() {
      if (currentRow && currentSection) currentSection.rows.push(currentRow);
      currentRow = null;
    }

    lines.forEach((raw) => {
      const line = raw.trimEnd();
      if (/^#\s*Checklist:/i.test(line)) {
        commitRow();
        const name = line.replace(/^#\s*Checklist:\s*/i, "").trim();
        if (checklist && checklist !== name) {
          headingMismatch = true;
        }
        checklist = name || checklist;
        tpl.checklist = checklist;
      } else if (/^##\s+/.test(line)) {
        commitRow();
        const name = line.replace(/^##\s+/, "").trim();
        currentSection = { name, rows: [] };
        tpl.sections.push(currentSection);
      } else if (/^###\s+/.test(line)) {
        commitRow();
        if (!currentSection) {
          currentSection = { name: "(no section)", rows: [] };
          tpl.sections.push(currentSection);
        }
        const proc = line.replace(/^###\s+/, "").trim();
        currentRow = { procedure: proc, action: "", spec: "", instructions: "", section: currentSection.name, checklist };
      } else if (/^-+\s*Action:/i.test(line)) {
        if (currentRow) currentRow.action = line.replace(/^-+\s*Action:\s*/i, "").trim();
      } else if (/^-+\s*Spec:/i.test(line)) {
        if (currentRow) currentRow.spec = line.replace(/^-+\s*Spec:\s*/i, "").trim();
      } else if (/^-+\s*Instructions?/i.test(line)) {
        // instructions block handled below
      } else if (/^####\s*Instructions/i.test(line)) {
        // start collecting instructions
        let buffer = [];
        // instructions continue until next heading; handled in main loop
        currentRow._collectingInstructions = true;
        currentRow._instructionsBuffer = buffer;
      } else if (currentRow?._collectingInstructions) {
        currentRow._instructionsBuffer.push(line);
      }
    });
    commitRow();

    // finalize instructions buffers
    tpl.sections.forEach((sec) => {
      sec.rows.forEach((r) => {
        if (r._collectingInstructions) {
          r.instructions = (r._instructionsBuffer || []).join("\n").trim();
          delete r._collectingInstructions;
          delete r._instructionsBuffer;
        }
        r.section = sec.name;
        r.checklist = tpl.checklist;
      });
    });

    if (!tpl.checklist) warnings.push("Checklist name missing; provide via UI or '# Checklist: <name>'.");
    if (headingMismatch) warnings.push("Checklist heading differed from UI selection; heading value was used.");
    return { template: tpl, run: emptyRun(tpl.checklist, "", ""), warnings };
  }

  function exportMarkdown(templateIR) {
    const lines = [];
    if (!templateIR) return { text: "", warnings: ["No template data available"] };
    lines.push(`# Checklist: ${templateIR.checklist || ""}`);
    lines.push("");
    (templateIR.sections || []).forEach((sec) => {
      lines.push(`## ${sec.name || ""}`);
      lines.push("");
      (sec.rows || []).forEach((r) => {
        lines.push(`### ${r.procedure || ""}`);
        lines.push(`- Action: ${r.action || ""}`);
        lines.push(`- Spec: ${r.spec || ""}`);
        lines.push(`- Result:`);
        lines.push(`- Status:`);
        lines.push(`- Comment:`);
        lines.push("");
        lines.push("#### Instructions");
        lines.push(r.instructions || "");
        lines.push("");
      });
    });
    return { text: lines.join("\n"), warnings: [] };
  }

  function importJsonl(text, opts = {}) {
    const warnings = [];
    const run = emptyRun(opts.defaultChecklist || "", opts.defaultInstanceId || "", opts.defaultInstancePrincipal || "");
    (text || "")
      .split(/\r?\n/)
      .filter((l) => l.trim().length > 0)
      .forEach((line, idx) => {
        try {
          const obj = JSON.parse(line);
          run.rows.push({
            address_id: obj.address_id || "",
            slug_id: obj.slug_id || "",
            status: obj.status || "",
            result: obj.result || "",
            comment: obj.comment || "",
            entity_id: obj.entity_id || "",
            instance_id: obj.instance_id || run.instance_id,
            instance_principal: obj.instance_principal || run.instance_principal,
          });
        } catch (err) {
          warnings.push(`Line ${idx + 1}: ${err.message}`);
        }
      });
    return { template: emptyTemplate(run.checklist, DEFAULT_ROOT), run, warnings };
  }

  function exportJsonl(runIR) {
    if (!runIR) return { text: "", warnings: ["No run data available"] };
    const lines = (runIR.rows || []).map((r) =>
      JSON.stringify({
        address_id: r.address_id,
        slug_id: r.slug_id,
        status: r.status,
        result: r.result,
        comment: r.comment,
        entity_id: r.entity_id,
        instance_id: r.instance_id,
        instance_principal: r.instance_principal,
      })
    );
    return { text: lines.join("\n"), warnings: [] };
  }

  // ---------- Registry + dispatcher ----------
  const registry = {
    markdown: { import: importMarkdown, export: exportMarkdown },
    jsonl: { import: importJsonl, export: exportJsonl },
  };

  STUB_FORMATS.forEach((fmt) => {
    registry[fmt] = {
      import: () => ({ template: emptyTemplate("", DEFAULT_ROOT), run: emptyRun("", "", ""), warnings: [`${fmt} import not yet supported`] }),
      export: () => ({ text: "", warnings: [`${fmt} export not yet supported`] }),
    };
  });

  // ---------- Batch application ----------
  async function applyBatch(templateIR, runIR, transport) {
    if (!transport || typeof transport.fetch !== "function") {
      throw new Error("applyBatch requires a transport.fetch(path, options) function.");
    }
    const { creates, updates, warnings } = irToBatchCalls(templateIR, runIR);
    const results = { created: 0, updated: 0, warnings: [...warnings] };

    for (const body of creates) {
      await transport.fetch("/api/v1/slugs", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      results.created += 1;
    }

    if (updates.length) {
      await transport.fetch("/api/v1/slugs/bulk-update", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ updates }),
      });
      results.updated += updates.length;
    }

    return results;
  }

  function importContent(format, text, opts = {}) {
    const fmt = (format || "markdown").toLowerCase();
    const handler = registry[fmt];
    if (!handler || !ENABLED_FORMATS.has(fmt)) {
      return { template: emptyTemplate("", DEFAULT_ROOT), run: emptyRun("", "", ""), warnings: [`Format ${fmt} not enabled`] };
    }
    return handler.import(text, opts);
  }

  function exportContent(format, templateIR, runIR) {
    const fmt = (format || "markdown").toLowerCase();
    const handler = registry[fmt];
    if (!handler || !ENABLED_FORMATS.has(fmt)) {
      return { text: "", warnings: [`Format ${fmt} not enabled`] };
    }
    return handler.export(templateIR, runIR);
  }

  window.importExport = {
    importContent,
    exportContent,
    applyBatch,
    irToBatchCalls,
    apiPayloadToIr,
    registry: () => clone(registry),
    ENABLED_FORMATS: Array.from(ENABLED_FORMATS),
    STUB_FORMATS,
  };
})();
