# Relationship Workbench Contract

The Relationship Workbench is an analysis and visualization surface. It makes existing workflow, lookup/binding, and mutation/provenance connections observable before any checklist data is normalized or predicate behavior is changed.

## Non-Goals

- It does not replace CHAX predicate triples.
- It does not require a domain-specific machine, customer, contract, or service schema.
- It does not rewrite a CSV, add a runtime gate, or create a profile table merely because it finds a candidate.

## Checklist Relationship Directory

Relationships are a first-class part of a checklist package. The canonical
location is a `relationships/` directory beside `checklist.md`:

```text
<checklist>/
  checklist.md
  relationships/
    bindings.json
```

`relationships/bindings.json` declares how a dataset provides values to
checklist row fields and how the package accounts for relationship
completeness. A new or actively maintained checklist is expected to have this
directory even when its declarations are initially empty. A legacy checklist
without it remains loadable, but the workbench reports
`RELATIONSHIP_PACKAGE_MISSING` rather than treating the missing model as
normal.

```json
{
  "schema": "chax-relationships-v1",
  "completeness": {
    "orphan_policy": "warn",
    "terminal_rows": []
  },
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

`terminal_rows` is an explicit declaration, not an escape hatch for an
unexamined row. Each entry identifies a row whose meaningful connection ends
in a named stakeholder, handoff, or retained outcome:

```json
{
  "slug_id": "<16-character-slug-id>",
  "stakeholder": "<person, role, system, or retained outcome>",
  "reason": "<why this is a valid terminal relationship>"
}
```

The workbench renders this as a `declared_terminal` relationship. It remains
inspectable and exportable like every other relationship.

`header-derived` recognizes the existing portable CSV convention:

```text
<slug_id>-<field>(optional human label)
```

where `field` is `result`, `status`, or `comment`. The dataset lookup is declared explicitly because it cannot be inferred safely from a wide CSV alone.

When a header carries a historical slug ID, the Workbench first resolves an exact
current slug ID and then follows a declared `slugPredecessor`/`slugSuccessor`
lineage alias in `checklist.md`. The exported graph makes that
`legacy_alias` bridge visible; it never substitutes a row merely because a
human label happens to match.

## Workbench Edge Classes

| Class | Meaning |
| --- | --- |
| `predicate` | Existing CHAX template/address relationship. |
| `lookup_key` | A slug field selects a dataset record or external record. |
| `column_binding` | A dataset column maps to a target slug field. |
| `direct_write` | A human, agent, API client, script, or daemon writes a mutable field. |
| `derived_write` | A rule/evaluator produces a later field or status. |
| `applicability` | A value determines whether a row is applicable, hidden, or `NA`. |
| `declared_terminal` | An explicit final stakeholder, handoff, or retained outcome accounts for a terminal row. |
| `legacy_alias` | A historical slug ID is explicitly mapped through lineage to a current row. |

## Relationship Completeness

Every checklist row should have at least one observable relationship: a CHAX
predicate, lookup/binding, mutation/provenance declaration, applicability
rule, or declared terminal relationship. A row with none is reported as
`ORPHAN_ROW`; it is not silently interpreted as a complete procedure step.

This is a Relationship Workbench policy and compatibility migration path, not
an immediate change to the core Markdown parser. The workbench therefore
makes legacy gaps visible first, so maintainers can decide whether to connect,
retire, or explicitly account for each row before a future normative
specification revision.

## Initial Findings

The analysis exporter must emit structured findings rather than silently optimize:

- `CONSTANT_COLUMN`
- `REPEATED_LITERAL`
- `HIGH_FAN_OUT_LOOKUP_KEY`
- `DIRECT_WRITE_UNDECLARED`
- `APPLICABILITY_UNDECLARED`
- `RELATIONSHIP_PACKAGE_MISSING`
- `ORPHAN_ROW`

The UI should render a connected workbench graph and supporting diagnostics. Text findings alone are insufficient.
