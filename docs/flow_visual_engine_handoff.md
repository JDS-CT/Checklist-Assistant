# Flow Visual Engine Implementation Handoff

## Objective

Replace the current Flow tab's placeholder graph renderers with connected, deterministic graph projections over the existing Checklist Assistant graph API.

The current data/API work is good. The missing work is visual projection:

- `Full concentrated graph` currently places rows in a fixed five-column tile grid. It is not a relationship-aware graph layout.
- `Predicate overview` currently renders metric cards only. It is not a graph.
- `Drill down` currently changes trace/highlight state. It must render a smaller connected subgraph.

Do not treat this as a database, schema, or relationship-semantic rewrite. The needed graph fields are already returned by:

```text
GET /api/v1/visualizations/graph?checklist=<checklist>&instance_id=<instance_id>
```

The response schema is `chax-graph-view-v1`.

## Privacy And Scope

Use public synthetic checklists for fixtures, screenshots, and tests. Do not add non-public checklist content to this repository.

Do not read SQLite from the client. The client must consume the graph projection API and relationship mutation APIs only.

## Canonical Versus Derived Data

Canonical data supplied by the API:

- row identity: `address_id`, `slug_id`
- row order: `address_order`
- row fields: `section`, `procedure`, `action`, `spec`, `result`, `status`, `comment`, `instructions`
- relationship triples: source, predicate, target

Derived view data:

- compact display-order ordinal
- `checklistOrder` display edges
- `spec_kind`, `visual_shape`
- graph layout coordinates
- swimlanes, predicate hubs, aggregation counts, colors, and trace state

`address_order` is intentionally gapped. Never use the raw value as a CSS grid column or pixel coordinate. Sort it, then assign compact visual ordinals `1..N`.

`checklistOrder` edges are display-only. They must never be written back as relationship predicates.

Lineage predicates (`slugPredecessor`, `slugSuccessor`, `addressPredecessor`, `addressSuccessor`) are metadata, not ordinary workflow routing.

## Required Edge Partitions

Create one shared classifier used by every view:

```text
order edges
  kind == "checklistOrder"

local operational edges
  kind == "relationship"
  !is_lineage
  !is_external
  source and target resolve to visible local nodes

lineage edges
  kind == "relationship"
  is_lineage

external operational edges
  kind == "relationship"
  !is_lineage
  is_external
```

Do not make a dense operational flow unreadable by drawing lineage and unresolved external edges in the same visual channel. Preserve them in the inspector, external-target view, lint view, and full audit graph.

## Rendering Architecture

Create a small graph-rendering module rather than continuing to grow `checklist_prototype_common.js`. A suggested split is:

```text
CHAX-CLIENT/web/flow_graph_engine.js
  classifyGraphEdges(graph)
  compactOrder(nodes)
  buildFullConcentratedProjection(graph, focus)
  buildPredicateOverviewProjection(graph, focus)
  buildHierarchyProjection(graph, focus)
  buildSwimlaneProjection(graph, focus)
  buildDrilldownProjection(graph, selectedMeta)
  renderProjection(container, projection)
```

The engine may use a bundled, local GraphViz/Viz.js-style renderer for graph-oriented views. Do not rely on Kroki or external network rendering; Checklist Assistant is local-first. If an external graph layout dependency is undesirable, implement the same node/hub/edge projection in SVG, but do not return to fixed tile grids for graph views.

For large graphs, do not try to force every node and every edge into one browser canvas. Use deterministic projections, filtering, and drilldown. Keep the full graph as an audit view, but provide focused views as the normal authoring path.

## View Contracts

### 1. Section Swimlane Flow

This is the operational flow view.

- One horizontal lane per canonical `Section`.
- All lanes share the same compact global order columns.
- Each row card is placed at its global order ordinal, not its lane-local index.
- Render faint `checklistOrder` edges as the execution/display backbone.
- Render local operational relationships above the backbone in predicate-specific colors.
- Keep cards and sticky Section labels above the SVG edge layer.
- Exclude lineage and unresolved external edges from this dense canvas.
- Redraw SVG paths after layout settles and on resize.

This view already has a partial implementation in the current Flow tab. Preserve the shared-order behavior while moving the rendering into the reusable engine.

### 2. Full Concentrated Graph

This is a connected audit graph, not a row gallery.

Required topology:

```text
source row -> predicate hub -> target row or external target hub
```

Rules:

- Use one visible node per checklist row.
- Aggregate repeated relationships through predicate hubs rather than drawing every long edge directly across the canvas.
- Show edge direction and predicate labels or predicate hubs.
- Distinguish local operational, lineage, and external edges by color/style.
- Make external and legacy targets explicit terminal/component nodes, never silent omissions.
- Preserve click/right-click inspection on nodes, hubs, and edges.
- Let the layout engine decide positions from graph topology. Do not use the current fixed five-column placement in `renderFlowGraphMap`.

The existing deterministic GraphViz export is the reference for the topology and density handling, not a mandatory byte-for-byte UI rendering.

### 3. Predicate Overview

This must be an actual aggregated graph, not metric tiles.

Required topology:

```text
source Section -> predicate hub -> target Section or target category
```

Aggregate edges by:

```text
source section + predicate + target section/category
```

Render one edge with a visible count when multiple relationships share the same aggregate. The graph should visibly answer:

- Which Sections produce each predicate?
- Where do those relationships go?
- Which predicates are local, external, or lineage?
- How dense is each connection?

Keep a compact metrics panel only as supporting detail, not as the entire view.

### 4. Hierarchy Relationships

Render ordered checklist rows in Section bands, then connect them to predicate hubs with visible SVG paths. This is the best detailed authoring/review view when the full graph is too dense.

It should preserve the following reading order:

```text
Section -> ordered rows -> predicate hubs -> target rows
```

### 5. Drilldown

Drilldown is not trace/highlight.

On Drill down for a selected row:

1. Create an induced subgraph containing the selected row.
2. Include its direct incoming and outgoing relationship edges.
3. Include the connected endpoint rows, external targets, and lineage targets needed to explain those edges.
4. For a selected predicate hub, include all edges using that predicate, subject to a deterministic maximum with an explicit truncation notice.
5. Render that subgraph using the same connected graph renderer as the full graph.
6. Show a `Back to <previous view>` control and retain the original filter/focus state.

Trace remains a separate operation: it highlights matches in the current projection and must not replace the projection with a yellow outline.

## Interaction Rules

- Click: inspect node, edge, or predicate hub in the existing inspector panel.
- Right-click: inspect and open context menu.
- Drill down: render a new connected projection as defined above.
- Show family: keep the current projection and apply a one-hop trace/focus highlight.
- Trace search: match procedure, action, Section, slug/address, and predicate; highlight matched nodes and connected edges.
- Source: show the deterministic source/interchange representation for the current projection, not a hidden unrelated export.

## Styling Rules

- Use visual shape only as a derived diagnostic. Never require authors to set `shape`.
- Keep row text readable. Do not make a diamond or ellipse clip essential procedure text.
- Make relationship lines readable at normal zoom; a low-opacity line that disappears at default zoom is not a useful backbone.
- Use predicate colors consistently across swimlane, hierarchy, full, predicate, and drilldown views.
- Use a visibly different style for order, lineage, external, and operational edges.
- Maintain pan/zoom/fit controls for topology-heavy graph views.

## Existing Code To Replace Or Reuse

Replace these current placeholders in `CHAX-CLIENT/web/checklist_prototype_common.js`:

- `renderFlowGraphMap`: fixed five-column SVG tile map; replace with connected concentrated graph projection.
- `renderFlowOverview`: metric cards; keep only as a supporting summary, not the predicate graph.
- context-menu `drill` action: replace trace-only behavior with `buildDrilldownProjection` plus graph render.

Reuse:

- `loadFlowGraph` and the graph API request.
- existing inspector, trace input, context menu, and edge-key identity conventions.
- the current swimlane DOM/SVG layering as the baseline for the operational flow view.

## Acceptance Tests

Use a public synthetic fixture with at least three Sections, multiple predicates, one external target, one lineage edge, and a branch.

1. Swimlane view places all rows by compact global order and shows order plus local operational paths.
2. Predicate overview has visible source-to-predicate-to-target paths and aggregate count labels.
3. Full concentrated graph has visible connected topology; it is not a fixed tile grid.
4. Clicking Drill down on a connected row replaces the view with an induced connected subgraph and provides a Back control.
5. Trace only highlights; it does not mutate graph layout or masquerade as drilldown.
6. External and lineage edges are visible in audit/drilldown contexts and distinct from normal operational routing.
7. Node, hub, and edge inspection works in every graph projection.
8. Browser screenshots at desktop and mobile widths show nonblank rendered paths and readable labels.
9. Graph rendering does not write runtime/export files merely from opening a view.

## Build And Verification

Follow repository guidance:

```powershell
cmake --preset windows-gcc-ninja
cmake --build --preset windows-gcc-ninja --target checklist_assistant_server
```

Use the repository's normal runtime staging process only when it is safe to replace the runtime directory. Do not delete or overwrite a live runtime directory containing a `.chax` database merely to refresh web assets.

For web-only work, run JavaScript syntax checks, `git diff --check`, and browser-level screenshots against a synthetic fixture. For C++ changes, run the relevant target and focused tests as described in `AGENTS.md`.
