/* Local, deterministic graph projections for the Checklist Assistant Flow tab. */
(function () {
  "use strict";

  function compareOrder(left, right) {
    const leftOrder = Number(left.address_order);
    const rightOrder = Number(right.address_order);
    if (Number.isFinite(leftOrder) && Number.isFinite(rightOrder) && leftOrder !== rightOrder) return leftOrder - rightOrder;
    return String(left.address_id || "").localeCompare(String(right.address_id || ""));
  }

  function compactOrder(nodes) {
    return [...(nodes || [])].sort(compareOrder).map((node, index) => ({ ...node, display_order: index + 1 }));
  }

  function edgeKey(edge) {
    return [edge.source_address_id || "", edge.predicate || "", edge.target_address_id || ""].join("\u001f");
  }

  function classifyGraphEdges(graph) {
    const nodeIds = new Set((graph?.nodes || []).map((node) => node.address_id));
    const partitions = { order: [], local: [], lineage: [], external: [] };
    (graph?.edges || []).forEach((edge) => {
      if (edge.kind === "checklistOrder") {
        partitions.order.push(edge);
      } else if (edge.is_lineage) {
        partitions.lineage.push(edge);
      } else if (edge.is_external || !nodeIds.has(edge.source_address_id) || !nodeIds.has(edge.target_address_id)) {
        partitions.external.push(edge);
      } else {
        partitions.local.push(edge);
      }
    });
    return partitions;
  }

  function nodeLabel(node) {
    return node?.procedure || node?.action || node?.address_id || "(unnamed row)";
  }

  function shorten(value, limit) {
    const text = String(value || "");
    return text.length > limit ? `${text.slice(0, Math.max(1, limit - 1))}...` : text;
  }

  function layoutNodes(nodes, operationalEdges) {
    const ordered = compactOrder(nodes);
    const levels = new Map(ordered.map((node) => [node.address_id, 0]));
    const orderById = new Map(ordered.map((node) => [node.address_id, node.display_order]));
    const forward = operationalEdges.filter((edge) => (orderById.get(edge.source_address_id) || 0) <= (orderById.get(edge.target_address_id) || 0));
    for (let round = 0; round < Math.min(ordered.length, 12); round += 1) {
      let changed = false;
      forward.forEach((edge) => {
        const next = (levels.get(edge.source_address_id) || 0) + 1;
        if (next > (levels.get(edge.target_address_id) || 0)) {
          levels.set(edge.target_address_id, Math.min(next, 8));
          changed = true;
        }
      });
      if (!changed) break;
    }
    const columns = new Map();
    ordered.forEach((node) => {
      const level = levels.get(node.address_id) || 0;
      if (!columns.has(level)) columns.set(level, []);
      columns.get(level).push(node);
    });
    const positions = new Map();
    const sortedColumns = [...columns.entries()].sort((left, right) => left[0] - right[0]);
    sortedColumns.forEach(([level, members]) => {
      members.sort(compareOrder).forEach((node, index) => {
        positions.set(node.address_id, { x: 54 + level * 260, y: 64 + index * 76 });
      });
    });
    return { ordered, positions, columnCount: Math.max(1, sortedColumns.length) };
  }

  function buildFullConcentratedProjection(graph) {
    const nodes = compactOrder(graph?.nodes || []);
    const partition = classifyGraphEdges(graph);
    const operational = [...partition.local, ...partition.lineage, ...partition.external];
    const layout = layoutNodes(nodes, partition.local);
    const predicateGroups = new Map();
    operational.forEach((edge) => {
      const kind = edge.is_lineage ? "lineage" : edge.is_external ? "external" : "local";
      const key = `${kind}|${edge.predicate || "(unnamed predicate)"}`;
      if (!predicateGroups.has(key)) predicateGroups.set(key, { key, predicate: edge.predicate || "(unnamed predicate)", kind, edges: [] });
      predicateGroups.get(key).edges.push(edge);
    });
    const hubs = [...predicateGroups.values()];
    const hubPositions = new Map();
    hubs.forEach((hub, index) => {
      const memberPositions = hub.edges.flatMap((edge) => [layout.positions.get(edge.source_address_id), layout.positions.get(edge.target_address_id)]).filter(Boolean);
      const averageX = memberPositions.length ? memberPositions.reduce((sum, position) => sum + position.x, 0) / memberPositions.length : 240;
      hubPositions.set(hub.key, { x: averageX + 126, y: 38 + index * 72 });
    });
    const externalTargets = new Map();
    partition.external.forEach((edge) => {
      if (!layout.positions.has(edge.target_address_id)) {
        externalTargets.set(edge.target_address_id, { id: edge.target_address_id, category: edge.external_category || "external" });
      }
    });
    const externalPositions = new Map();
    [...externalTargets.values()].forEach((target, index) => {
      externalPositions.set(target.id, { x: 54 + layout.columnCount * 260, y: 64 + index * 76 });
    });
    return {
      type: "full",
      title: "Full concentrated graph",
      nodes,
      edges: operational,
      orderEdges: partition.order,
      hubs,
      positions: layout.positions,
      hubPositions,
      externalTargets: [...externalTargets.values()],
      externalPositions,
      width: Math.max(960, 310 + (layout.columnCount + 1) * 260),
      height: Math.max(340, 140 + Math.max(nodes.length, hubs.length, externalTargets.size) * 76),
    };
  }

  function buildPredicateOverviewProjection(graph) {
    const nodes = compactOrder(graph?.nodes || []);
    const nodeById = new Map(nodes.map((node) => [node.address_id, node]));
    const partition = classifyGraphEdges(graph);
    const aggregate = new Map();
    [...partition.local, ...partition.lineage, ...partition.external].forEach((edge) => {
      const source = nodeById.get(edge.source_address_id);
      const target = nodeById.get(edge.target_address_id);
      const kind = edge.is_lineage ? "lineage" : edge.is_external ? "external" : "local";
      const sourceLabel = source?.section || "(no section)";
      const targetLabel = target?.section || (edge.is_lineage ? "Lineage target" : edge.external_category === "legacy" ? "Legacy target" : "External target");
      const key = [sourceLabel, edge.predicate || "(unnamed predicate)", targetLabel, kind].join("|");
      if (!aggregate.has(key)) aggregate.set(key, { key, sourceLabel, predicate: edge.predicate || "(unnamed predicate)", targetLabel, kind, edges: [] });
      aggregate.get(key).edges.push(edge);
    });
    const groups = [...aggregate.values()];
    const sourceLabels = [...new Set(groups.map((item) => item.sourceLabel))];
    const targetLabels = [...new Set(groups.map((item) => item.targetLabel))];
    const predicates = [...new Set(groups.map((item) => item.predicate))];
    return { type: "predicate", title: "Predicate overview", groups, sourceLabels, targetLabels, predicates };
  }

  function buildHierarchyProjection(graph) {
    const projection = buildFullConcentratedProjection(graph);
    projection.type = "hierarchy";
    projection.title = "Hierarchy relationships";
    projection.edges = classifyGraphEdges(graph).local;
    projection.hubs = projection.hubs.filter((hub) => hub.kind === "local");
    projection.externalTargets = [];
    projection.externalPositions = new Map();
    return projection;
  }

  function buildSwimlaneProjection(graph) {
    const nodes = compactOrder(graph?.nodes || []);
    const sections = new Map();
    nodes.forEach((node) => {
      const section = node.section || "(no section)";
      if (!sections.has(section)) sections.set(section, []);
      sections.get(section).push(node);
    });
    return { type: "swimlanes", title: "Section swimlanes", nodes, sections: [...sections.entries()], partition: classifyGraphEdges(graph) };
  }

  function buildDrilldownProjection(graph, selectedMeta) {
    const nodes = compactOrder(graph?.nodes || []);
    const partition = classifyGraphEdges(graph);
    const allEdges = [...partition.local, ...partition.lineage, ...partition.external];
    let selectedEdges = [];
    let selectedNodeIds = new Set();
    if (selectedMeta?.type === "node") {
      selectedNodeIds.add(selectedMeta.raw.address_id);
      selectedEdges = allEdges.filter((edge) => edge.source_address_id === selectedMeta.raw.address_id || edge.target_address_id === selectedMeta.raw.address_id);
    } else if (selectedMeta?.type === "hub" || selectedMeta?.type === "aggregate") {
      selectedEdges = allEdges.filter((edge) => edge.predicate === selectedMeta.raw.predicate).slice(0, 80);
    } else if (selectedMeta?.type === "edge") {
      selectedEdges = [selectedMeta.raw];
    }
    selectedEdges.forEach((edge) => {
      selectedNodeIds.add(edge.source_address_id);
      selectedNodeIds.add(edge.target_address_id);
    });
    const subset = { nodes: nodes.filter((node) => selectedNodeIds.has(node.address_id)), edges: selectedEdges };
    const projection = buildFullConcentratedProjection(subset);
    projection.type = "drilldown";
    projection.title = "Drilldown";
    projection.notice = selectedMeta?.type === "hub" && allEdges.filter((edge) => edge.predicate === selectedMeta.raw.predicate).length > selectedEdges.length
      ? "Showing the first 80 relationships for this predicate."
      : "Connected rows and targets for the selected item.";
    return projection;
  }

  function svgPath(source, target) {
    const middle = (source.x + target.x) / 2;
    return `M ${source.x} ${source.y} C ${middle} ${source.y}, ${middle} ${target.y}, ${target.x} ${target.y}`;
  }

  function renderControls() {
    return '<div class="flow-graph-controls"><button type="button" data-flow-zoom="out" title="Zoom out">-</button><button type="button" data-flow-zoom="fit" title="Fit graph">Fit</button><button type="button" data-flow-zoom="in" title="Zoom in">+</button></div>';
  }

  function renderGraphProjection(container, projection, options) {
    const escape = options.escapeHtml;
    const escapeAttr = options.escapeHtmlAttr;
    const color = options.predicateColor;
    const nodeById = new Map(projection.nodes.map((node) => [node.address_id, node]));
    const edgePaths = [];
    projection.edges.forEach((edge) => {
      const source = projection.positions.get(edge.source_address_id);
      const target = projection.positions.get(edge.target_address_id) || projection.externalPositions?.get(edge.target_address_id);
      const kind = edge.is_lineage ? "lineage" : edge.is_external ? "external" : "operational";
      const hubKey = `${edge.is_lineage ? "lineage" : edge.is_external ? "external" : "local"}|${edge.predicate || "(unnamed predicate)"}`;
      const hub = projection.hubPositions.get(hubKey);
      if (!source || !target || !hub) return;
      const sourcePoint = { x: source.x + 174, y: source.y + 24 };
      const hubLeft = { x: hub.x - 68, y: hub.y + 16 };
      const hubRight = { x: hub.x + 68, y: hub.y + 16 };
      const targetPoint = { x: target.x, y: target.y + 24 };
      const attrs = `class="flow-graph-edge ${kind}" data-flow-edge-key="${escapeAttr(edgeKey(edge))}" stroke="${escapeAttr(color(edge.predicate))}"`;
      edgePaths.push(`<path ${attrs} d="${svgPath(sourcePoint, hubLeft)}"></path><path ${attrs} d="${svgPath(hubRight, targetPoint)}"></path>`);
    });
    const hubs = projection.hubs.map((hub) => {
      const point = projection.hubPositions.get(hub.key);
      return `<g class="flow-graph-hub ${hub.kind}" data-flow-hub="${escapeAttr(hub.predicate)}" data-flow-hub-kind="${escapeAttr(hub.kind)}" tabindex="0" role="button"><rect x="${point.x - 68}" y="${point.y}" width="136" height="32" rx="6"></rect><text x="${point.x}" y="${point.y + 20}" text-anchor="middle">${escape(shorten(hub.predicate, 21))}</text></g>`;
    }).join("");
    const nodes = projection.nodes.map((node) => {
      const point = projection.positions.get(node.address_id);
      if (!point) return "";
      const shape = ["action", "decision", "terminal", "metric"].includes(node.visual_shape) ? node.visual_shape : "action";
      return `<g class="flow-graph-node ${shape}" data-flow-node="${escapeAttr(node.address_id)}" tabindex="0" role="button"><rect x="${point.x}" y="${point.y}" width="174" height="48" rx="6"></rect><text x="${point.x + 9}" y="${point.y + 19}">${escape(shorten(nodeLabel(node), 25))}</text><text class="flow-graph-subtext" x="${point.x + 9}" y="${point.y + 36}">${escape(shorten(node.section || node.slug_id || "", 28))}</text></g>`;
    }).join("");
    const external = (projection.externalTargets || []).map((target) => {
      const point = projection.externalPositions.get(target.id);
      return `<g class="flow-graph-external" data-flow-external="${escapeAttr(target.id)}" tabindex="0" role="button"><rect x="${point.x}" y="${point.y}" width="174" height="48" rx="6"></rect><text x="${point.x + 9}" y="${point.y + 20}">${escape(shorten(target.id, 25))}</text><text class="flow-graph-subtext" x="${point.x + 9}" y="${point.y + 36}">${escape(target.category)}</text></g>`;
    }).join("");
    const notice = projection.notice ? `<div class="note">${escape(projection.notice)}</div>` : "";
    container.innerHTML = `${notice}<div class="flow-svg-shell">${renderControls()}<svg class="flow-topology" viewBox="0 0 ${projection.width} ${projection.height}" role="img" aria-label="${escapeAttr(projection.title)}"><defs><marker id="flowArrow" markerWidth="8" markerHeight="8" refX="7" refY="3.5" orient="auto"><path d="M0,0 L0,7 L7,3.5 z"></path></marker></defs>${edgePaths.join("")}${hubs}${nodes}${external}</svg></div>`;
  }

  function renderPredicateProjection(container, projection, options) {
    const escape = options.escapeHtml;
    const escapeAttr = options.escapeHtmlAttr;
    const color = options.predicateColor;
    const sourceY = new Map(projection.sourceLabels.map((value, index) => [value, 58 + index * 72]));
    const targetY = new Map(projection.targetLabels.map((value, index) => [value, 58 + index * 72]));
    const predicateY = new Map(projection.predicates.map((value, index) => [value, 58 + index * 72]));
    const height = Math.max(300, 120 + Math.max(projection.sourceLabels.length, projection.targetLabels.length, projection.predicates.length) * 72);
    const node = (kind, label, x, y, attr) => `<g class="flow-aggregate-node ${kind}" ${attr} tabindex="0" role="button"><rect x="${x}" y="${y}" width="190" height="42" rx="6"></rect><text x="${x + 9}" y="${y + 25}">${escape(shorten(label, 27))}</text></g>`;
    const paths = projection.groups.map((group) => {
      const source = { x: 210, y: sourceY.get(group.sourceLabel) + 21 };
      const hub = { x: 505, y: predicateY.get(group.predicate) + 21 };
      const target = { x: 800, y: targetY.get(group.targetLabel) + 21 };
      const stroke = color(group.predicate);
      const first = `<path class="flow-graph-edge ${group.kind}" data-flow-edge-key="${escapeAttr(group.key)}" stroke="${escapeAttr(stroke)}" d="${svgPath(source, hub)}"></path>`;
      const second = `<path class="flow-graph-edge ${group.kind}" data-flow-edge-key="${escapeAttr(group.key)}" stroke="${escapeAttr(stroke)}" d="${svgPath(hub, target)}"></path>`;
      return `${first}${second}<text class="flow-edge-count" x="${(source.x + hub.x) / 2}" y="${(source.y + hub.y) / 2 - 5}">${group.edges.length}</text>`;
    }).join("");
    const sources = projection.sourceLabels.map((label) => node("section", label, 20, sourceY.get(label), `data-flow-section="${escapeAttr(label)}"`)).join("");
    const predicates = projection.predicates.map((label) => node("predicate", label, 410, predicateY.get(label), `data-flow-hub="${escapeAttr(label)}" data-flow-hub-kind="predicate"`)).join("");
    const targets = projection.targetLabels.map((label) => node("target", label, 800, targetY.get(label), `data-flow-section="${escapeAttr(label)}"`)).join("");
    container.innerHTML = `<div class="flow-svg-shell">${renderControls()}<svg class="flow-topology" viewBox="0 0 1020 ${height}" role="img" aria-label="Predicate overview"><defs><marker id="flowArrow" markerWidth="8" markerHeight="8" refX="7" refY="3.5" orient="auto"><path d="M0,0 L0,7 L7,3.5 z"></path></marker></defs>${paths}${sources}${predicates}${targets}</svg></div>`;
  }

  function renderHierarchyProjection(container, projection, options) {
    const escape = options.escapeHtml;
    const escapeAttr = options.escapeHtmlAttr;
    const color = options.predicateColor;
    const nodeById = new Map(projection.nodes.map((node) => [node.address_id, node]));
    const positions = new Map(projection.nodes.map((node, index) => [node.address_id, { x: 160, y: 52 + index * 70 }]));
    const predicates = [...new Set(projection.edges.map((edge) => edge.predicate || "(unnamed predicate)"))];
    const hubs = new Map(predicates.map((predicate, index) => [predicate, { x: 545, y: 52 + index * 70 }]));
    const height = Math.max(300, 120 + Math.max(projection.nodes.length, predicates.length) * 70);
    const paths = projection.aggregates.map((bundle) => {
      const edge = bundle.edges[0];
      const source = positions.get(edge.source_address_id);
      const target = positions.get(edge.target_address_id);
      const hub = hubs.get(edge.predicate || "(unnamed predicate)");
      if (!source || !target || !hub) return "";
      const isSourceBundle = bundle.key.startsWith("source|");
      const from = isSourceBundle ? { x: source.x + 210, y: source.y + 21 } : { x: hub.x + 150, y: hub.y + 21 };
      const to = isSourceBundle ? { x: hub.x, y: hub.y + 21 } : { x: target.x + 210, y: target.y + 21 };
      const attrs = `class="flow-graph-edge operational" data-flow-edge-key="${escapeAttr(edgeKey(edge))}" stroke="${escapeAttr(color(edge.predicate))}"`;
      const count = bundle.edges.length > 1 ? `<text class="flow-edge-count" x="${(from.x + to.x) / 2}" y="${(from.y + to.y) / 2 - 5}">${bundle.edges.length}</text>` : "";
      return `<path ${attrs} d="${svgPath(from, to)}"></path>${count}`;
    }).join("");
    const rowNodes = projection.nodes.map((node) => {
      const point = positions.get(node.address_id);
      return `<g class="flow-aggregate-node section" data-flow-node="${escapeAttr(node.address_id)}" tabindex="0" role="button"><rect x="${point.x}" y="${point.y}" width="210" height="42" rx="6"></rect><text x="${point.x + 8}" y="${point.y + 18}">${escape(shorten(node.section || "(no section)", 29))}</text><text class="flow-graph-subtext" x="${point.x + 8}" y="${point.y + 34}">${escape(shorten(nodeLabel(node), 30))}</text></g>`;
    }).join("");
    const hubNodes = predicates.map((predicate) => {
      const point = hubs.get(predicate);
      return `<g class="flow-aggregate-node predicate" data-flow-hub="${escapeAttr(predicate)}" data-flow-hub-kind="predicate" tabindex="0" role="button"><rect x="${point.x}" y="${point.y}" width="150" height="42" rx="6"></rect><text x="${point.x + 8}" y="${point.y + 25}">${escape(shorten(predicate, 20))}</text></g>`;
    }).join("");
    container.innerHTML = `<div class="flow-svg-shell">${renderControls()}<svg class="flow-topology" viewBox="0 0 980 ${height}" role="img" aria-label="Hierarchy relationships"><defs><marker id="flowArrow" markerWidth="8" markerHeight="8" refX="7" refY="3.5" orient="auto"><path d="M0,0 L0,7 L7,3.5 z"></path></marker></defs>${paths}${rowNodes}${hubNodes}</svg></div>`;
  }

  function renderSwimlanes(container, projection, options) {
    const escape = options.escapeHtml;
    const escapeAttr = options.escapeHtmlAttr;
    const card = options.nodeCard;
    const order = new Map(projection.nodes.map((node) => [node.address_id, node.display_order]));
    container.innerHTML = `<div class="flow-lanes-visual" style="--flow-cols: ${Math.max(1, projection.nodes.length)}">${projection.sections.map(([section, rows]) => `<section class="flow-lane"><div class="flow-lane-title">${escape(section)}</div><div class="flow-cards">${rows.map((node) => card(node, order.get(node.address_id))).join("")}</div></section>`).join("")}</div>`;
  }

  let vizPromise;

  function dotId(value) {
    return JSON.stringify(String(value));
  }

  function dotAttrs(attrs) {
    return Object.entries(attrs).filter(([, value]) => value !== undefined && value !== null && value !== "")
      .map(([key, value]) => `${key}=${dotId(value)}`).join(", ");
  }

  function dotLabel(...parts) {
    return parts.filter(Boolean).map((part) => String(part).replace(/\\/g, "\\\\").replace(/\n/g, " ")).join("\\n");
  }

  function buildDotProjection(projection) {
    const isPredicate = projection.type === "predicate";
    const lines = [
      "digraph flow {",
      "graph [rankdir=\"LR\", bgcolor=\"#0f131a\", fontname=\"Segoe UI\", fontcolor=\"#e8edf5\", nodesep=\"0.28\", ranksep=\"0.62\", splines=\"polyline\", concentrate=\"true\", pad=\"0.22\"];",
      "node [shape=\"box\", style=\"rounded,filled\", color=\"#526173\", fillcolor=\"#18212c\", fontcolor=\"#edf5ff\", fontname=\"Segoe UI\", fontsize=\"11\", margin=\"0.08,0.05\"];",
      "edge [fontname=\"Segoe UI\", fontsize=\"9\", fontcolor=\"#c9d4e5\", arrowsize=\"0.7\"];",
    ];
    const nodeIds = new Map();
    const hubIds = new Map();
    const externalIds = new Map();
    const edgeIds = [];
    if (isPredicate) {
      projection.sourceLabels.forEach((label, index) => {
        const id = `section_source_${index}`; nodeIds.set(`source:${label}`, id);
        lines.push(`${dotId(id)} [${dotAttrs({ label: label, shape: "folder", fillcolor: "#18212c" })}];`);
      });
      projection.predicates.forEach((predicate, index) => {
        const id = `predicate_${index}`; hubIds.set(predicate, id);
        lines.push(`${dotId(id)} [${dotAttrs({ label: predicate, shape: "hexagon", fillcolor: "#18263a", color: "#7fb4ff" })}];`);
      });
      projection.targetLabels.forEach((label, index) => {
        const id = `section_target_${index}`; nodeIds.set(`target:${label}`, id);
        lines.push(`${dotId(id)} [${dotAttrs({ label: label, shape: "folder", fillcolor: "#18212c" })}];`);
      });
      projection.groups.forEach((group, index) => {
        const source = nodeIds.get(`source:${group.sourceLabel}`);
        const hub = hubIds.get(group.predicate);
        const target = nodeIds.get(`target:${group.targetLabel}`);
        const attrs = { label: String(group.edges.length), color: group.kind === "lineage" ? "#8fb6ff" : group.kind === "external" ? "#b98b54" : "#7fb4ff", style: group.kind === "lineage" ? "dashed" : "solid", id: `flow-edge-${index}` };
        lines.push(`${dotId(source)} -> ${dotId(hub)} [${dotAttrs(attrs)}];`);
        lines.push(`${dotId(hub)} -> ${dotId(target)} [${dotAttrs({ ...attrs, id: `flow-edge-${index}-b` })}];`);
        edgeIds.push({ ids: [`flow-edge-${index}`, `flow-edge-${index}-b`], titles: [`${source}->${hub}`, `${hub}->${target}`], key: group.key });
      });
    } else {
      const connectedIds = new Set(projection.edges.flatMap((edge) => [edge.source_address_id, edge.target_address_id]));
      projection.nodes.filter((node) => connectedIds.has(node.address_id)).forEach((node, index) => {
        const id = `row_${index}`; nodeIds.set(node.address_id, id);
        const shape = node.visual_shape === "decision" ? "diamond" : node.visual_shape === "terminal" ? "ellipse" : node.visual_shape === "metric" ? "note" : "box";
        lines.push(`${dotId(id)} [${dotAttrs({ label: dotLabel(nodeLabel(node), node.action), shape, tooltip: `${node.slug_id || ""} | ${node.section || ""}` })}];`);
      });
      projection.hubs.forEach((hub, index) => {
        const id = `hub_${index}`; hubIds.set(hub.key, id);
        lines.push(`${dotId(id)} [${dotAttrs({ label: hub.predicate, shape: "hexagon", fillcolor: "#18263a", color: hub.kind === "external" ? "#b98b54" : "#7fb4ff" })}];`);
      });
      (projection.externalTargets || []).forEach((target, index) => {
        const id = `external_${index}`; externalIds.set(target.id, id);
        lines.push(`${dotId(id)} [${dotAttrs({ label: dotLabel(target.category, target.id), shape: "component", fillcolor: "#2a2018", color: "#b98b54" })}];`);
      });
      projection.edges.forEach((edge, index) => {
        const source = nodeIds.get(edge.source_address_id);
        const target = nodeIds.get(edge.target_address_id) || externalIds.get(edge.target_address_id);
        const hubKey = `${edge.is_lineage ? "lineage" : edge.is_external ? "external" : "local"}|${edge.predicate || "(unnamed predicate)"}`;
        const hub = hubIds.get(hubKey);
        if (!source || !target || !hub) return;
        const color = edge.is_lineage ? "#8fb6ff" : edge.is_external ? "#b98b54" : "#7fb4ff";
        const attrs = { color, style: edge.is_lineage ? "dashed" : "solid", id: `flow-edge-${index}` };
        lines.push(`${dotId(source)} -> ${dotId(hub)} [${dotAttrs(attrs)}];`);
        lines.push(`${dotId(hub)} -> ${dotId(target)} [${dotAttrs({ ...attrs, id: `flow-edge-${index}-b` })}];`);
        edgeIds.push({ ids: [`flow-edge-${index}`, `flow-edge-${index}-b`], titles: [`${source}->${hub}`, `${hub}->${target}`], key: edgeKey(edge) });
      });
    }
    lines.push("}");
    return { dot: lines.join("\n"), nodeIds, hubIds, externalIds, edgeIds };
  }

  async function renderDotProjection(container, projection, options) {
    if (!window.Viz?.instance) throw new Error("Local Viz.js renderer is unavailable.");
    const model = buildDotProjection(projection);
    container.innerHTML = '<div class="note">Rendering GraphViz layout...</div>';
    const viz = await (vizPromise ??= window.Viz.instance());
    const svg = viz.renderSVGElement(model.dot, { engine: "dot" });
    svg.classList.add("flow-topology");
    const nodeKeys = new Map();
    model.nodeIds.forEach((id, key) => nodeKeys.set(id, key));
    const hubKeys = new Map();
    model.hubIds.forEach((id, key) => hubKeys.set(id, key));
    const externalKeys = new Map();
    model.externalIds.forEach((id, key) => externalKeys.set(id, key));
    for (const element of svg.querySelectorAll("g.node")) {
      const title = element.querySelector("title")?.textContent?.trim() || element.id;
      const key = nodeKeys.get(title);
      if (key) {
        if (key.startsWith("source:") || key.startsWith("target:")) element.dataset.flowHub = key.slice(key.indexOf(":") + 1);
        else element.dataset.flowNode = key;
      }
      const hubKey = hubKeys.get(title);
      if (hubKey) { element.dataset.flowHub = hubKey.split("|").slice(1).join("|"); element.dataset.flowHubKind = hubKey.split("|")[0]; }
      const externalKey = externalKeys.get(title);
      if (externalKey) element.dataset.flowExternal = externalKey;
    }
    const edgeKeys = new Map();
    model.edgeIds.forEach((entry) => entry.titles.forEach((title) => edgeKeys.set(title, entry.key)));
    for (const element of svg.querySelectorAll("g.edge")) {
      const title = element.querySelector("title")?.textContent?.trim() || element.id;
      const key = edgeKeys.get(title);
      if (key) element.dataset.flowEdgeKey = key;
    }
    const shell = document.createElement("div");
    shell.className = "flow-svg-shell";
    shell.innerHTML = renderControls();
    shell.append(svg);
    container.innerHTML = "";
    container.append(shell);
    applyZoom(container, "fit");
  }

  function renderProjection(container, projection, options) {
    if (projection.type === "swimlanes") return renderSwimlanes(container, projection, options);
    return renderDotProjection(container, projection, options);
  }

  function applyZoom(container, action) {
    const svg = container?.querySelector(".flow-topology");
    if (!svg) return;
    const current = Number(svg.dataset.flowZoom || "1");
    const fit = svg.viewBox?.baseVal?.width ? Math.min(1, Math.max(0.12, (container.clientWidth - 32) / svg.viewBox.baseVal.width)) : 1;
    const next = action === "fit" ? fit : Math.max(0.12, Math.min(4, current + (action === "in" ? 0.2 : -0.2)));
    svg.dataset.flowZoom = String(next);
    svg.style.transform = `scale(${next})`;
    svg.style.transformOrigin = "top left";
    const width = svg.scrollWidth || svg.getBoundingClientRect().width;
    const height = svg.scrollHeight || svg.getBoundingClientRect().height;
    svg.style.marginRight = `${Math.max(0, (next - 1) * width)}px`;
    svg.style.marginBottom = `${Math.max(0, (next - 1) * height)}px`;
  }

  window.flowGraphEngine = {
    classifyGraphEdges,
    compactOrder,
    buildFullConcentratedProjection,
    buildPredicateOverviewProjection,
    buildHierarchyProjection,
    buildSwimlaneProjection,
    buildDrilldownProjection,
    renderProjection,
    applyZoom,
  };
})();
