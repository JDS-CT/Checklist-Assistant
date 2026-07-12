# Relationship Schema View

Checklist Assistant exports `runtime-schema.dbml` with every workspace visualization export. The file is a DBML/ERD view of the implemented SQLite runtime schema and can be opened with DBML-capable tooling such as dbdiagram.

It is deliberately a **Mode A core-schema view**:

- `slugs` is the canonical runtime row store.
- `template_relationships` represents template-level triples using logical `slug_id` values.
- `address_relationships` represents runtime-level triples whose subject and target both reference `slugs.address_id`.
- `slug_ownership` and `address_ownership` preserve the source/pack/checklist boundary separately from row identity.
- `predicates` is an optional catalog. Predicate text remains an opaque runtime value rather than a required relational foreign key.

The schema view makes the self-referential address relationship explicit without claiming that every self-edge has the same meaning. A predicate determines whether a self-reference is a valid self-rule, lineage record, operational relationship, or a lint concern.

It intentionally does not create or infer deployment-specific domain tables such as equipment, customer, contract, or report-slot structures. Those belong to a future optional profile layer, whose annotations may classify existing rows and edges without changing the CHAX core schema.

## Exported Views

`POST /api/v1/workspace/visualizations/export` writes these files under the selected checklist's `visualizations/<instance_id>/` folder:

- `graph.json` — versioned graph interchange data for the selected instance.
- `section-flow.dot` — GraphViz relationship view.
- `section-flow.mmd` — Mermaid relationship view.
- `runtime-schema.dbml` — DBML core SQLite schema view.

The DBML file is stable for a given runtime schema rather than for a checklist instance. It is included with instance exports so authors and maintainers can inspect the graph and its persistence model together.
