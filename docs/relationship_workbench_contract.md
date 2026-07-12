# Relationship Workbench Contract

The Relationship Workbench is an analysis and visualization surface. It makes existing workflow, lookup/binding, and mutation/provenance connections observable before any checklist data is normalized or predicate behavior is changed.

## Non-Goals

- It does not replace CHAX predicate triples.
- It does not require a domain-specific machine, customer, contract, or service schema.
- It does not rewrite a CSV, add a runtime gate, or create a profile table merely because it finds a candidate.

## Checklist-Local Binding Sidecar

An optional `relationship_bindings.json` beside `checklist.md` declares how a dataset provides values to checklist row fields.

```json
{
  "schema": "chax-relationship-bindings-v1",
  "datasets": [
    {
      "path": "data/records.csv",
      "lookup": {
        "column": "<source-column>",
        "slug_id": "<16-character-slug-id>",
        "field": "result"
      },
      "bindings": "header-derived"
    }
  ],
  "mutation_sources": []
}
```

`header-derived` recognizes the existing portable CSV convention:

```text
<slug_id>-<field>(optional human label)
```

where `field` is `result`, `status`, or `comment`. The dataset lookup is declared explicitly because it cannot be inferred safely from a wide CSV alone.

## Workbench Edge Classes

| Class | Meaning |
| --- | --- |
| `predicate` | Existing CHAX template/address relationship. |
| `lookup_key` | A slug field selects a dataset record or external record. |
| `column_binding` | A dataset column maps to a target slug field. |
| `direct_write` | A human, agent, API client, script, or daemon writes a mutable field. |
| `derived_write` | A rule/evaluator produces a later field or status. |
| `applicability` | A value determines whether a row is applicable, hidden, or `NA`. |

## Initial Findings

The analysis exporter must emit structured findings rather than silently optimize:

- `CONSTANT_COLUMN`
- `REPEATED_LITERAL`
- `HIGH_FAN_OUT_LOOKUP_KEY`
- `DIRECT_WRITE_UNDECLARED`
- `APPLICABILITY_UNDECLARED`

The UI should render a connected workbench graph and supporting diagnostics. Text findings alone are insufficient.
