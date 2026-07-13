# Relationship Workbench Contract

The Relationship Workbench is an analysis and visualization surface. It makes existing workflow, lookup/binding, and mutation/provenance connections observable before any checklist data is normalized or predicate behavior is changed.

## Non-Goals

- It does not replace CHAX predicate triples.
- It does not require a domain-specific machine, customer, contract, or service schema.
- It does not rewrite a CSV, add a runtime gate, or create a profile table merely because it finds a candidate.

## Import Lineage and Asset Locality

Markdown and CSV keep their historical IDs. When an instance is materialized,
an ordinary relationship target with one declared `slugPredecessor` or
`slugSuccessor` path to a current row is derived to that current address. The
stored template triple remains unchanged; a lineage declaration itself is not
turned into a resolved self-edge. A branch, cycle, absent current row, or
human-label resemblance is not enough to redirect a target.

Prefill data is resolved from the ownership record of the addressed checklist
row: `<source_path>/<pack>/<checklist_dir>/data/`. An owned checklist never
falls through to a same-named checklist in the staged public runtime, because
that could silently use the wrong customer or deployment data. Legacy rows
without ownership retain the public-library lookup fallback.

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

These are projection labels, not a replacement predicate vocabulary. A
`predicate` edge preserves the exact CHAX predicate name from
`template_relationships` or `address_relationships`. The other labels state
where a non-predicate connection came from, so an author can find its actual
source instead of reverse-engineering a generic graph term.

| JSON class | Meaning | Source of truth | v1 state |
| --- | --- | --- | --- |
| `predicate` | An existing CHAX template/address relationship. The edge label is the canonical predicate token. | Markdown `Relationships` and runtime relationship tables. | Emitted. |
| `lookup_key` | A mutable row field selects a record in a declared dataset. | `relationships/bindings.json` → `datasets[].lookup`. | Emitted. |
| `column_binding` | A dataset column supplies a specific `result`, `status`, or `comment` field. | `relationships/bindings.json` plus a `header-derived` CSV header. | Emitted. |
| `direct_write` | A declared human, API, script, agent, or daemon source writes a mutable field. | `relationships/bindings.json` → `mutation_sources`. | Emitted only for an explicit declaration. |
| `declared_terminal` | An author declares a final stakeholder, handoff, or retained outcome for a row. | `relationships/bindings.json` → `completeness.terminal_rows`. | Emitted. |
| `legacy_alias` | A historical slug ID is explicitly mapped through `slugPredecessor`/`slugSuccessor` lineage to a current row. | Markdown `Relationships`. | Emitted. |
| `contains_column` | Structural display edge from a dataset to one of its columns; it does not imply a field write. | The declared CSV header. | Emitted. |

`derived_write` and `applicability` are deliberately **not** v1 edge classes.
The runtime does not yet persist a distinct evaluator-produced mutation event
or an applicability rule declaration that could support those labels. If that
data is added, the projection must point to the canonical predicate/evaluator
or rule source; it must not create a second name for the same CHAX behavior.

`declared_terminal` is authoring metadata, not an executable predicate or
automation trigger. Marking its row Pass does not send a report or email. A
future completion-triggered report/email flow should use an explicit script or
action contract with its own confirmation, authorization, and audit record;
it must not be inferred from terminal metadata.

## Relationship Completeness and Sufficiency

In v1, every checklist row should have at least one emitted relationship: a
CHAX predicate, lookup/binding, declared mutation source, or declared terminal
relationship. A future applicability rule may count only after it has an
equally explicit declaration and projection edge. A row with none is reported
as `ORPHAN_ROW`; it is not silently interpreted as a complete procedure step.

Derived checklist order does **not** count toward this test. Order exists to
lay out the procedure, while this check asks whether a row has an explicit
workflow, data, provenance, applicability, or stakeholder connection. An
explicit declared terminal and an orphan are therefore mutually exclusive for
completeness purposes: a terminal row names its external outcome; an orphan
names nothing. `ORPHAN_ROW` is a warning, never a runtime block, so compact or
prototype checklists remain usable while the gap is visible.

`ORPHAN_ROW` is deliberately narrow. It answers whether a row has **any**
recognized connection. A row with only operational predicate edges to itself
is therefore not an orphan, but it may still be isolated from the wider
procedure. `SELF_ONLY_RELATIONSHIP` is a separate warning for that case when
the row has no non-self predicate edge and no declared dataset binding,
mutation source, or terminal purpose. It asks the author to connect the row to
a downstream consumer, stakeholder, or external automation source—or make
that purpose explicit. It does not claim that a local calculation is invalid.

Declared terminals, mutation sources, lookup/binding edges, and non-self
predicate edges suppress this advisory because they supply the missing visible
context. The Workbench summary exposes both `orphan_rows` and
`self_only_rows` so authors can track exact absence of connections separately
from potentially insufficient relationship scope.

This is a Relationship Workbench policy and compatibility migration path, not
an immediate change to the core Markdown parser. The workbench therefore
makes legacy gaps visible first, so maintainers can decide whether to connect,
retire, or explicitly account for each row before a future normative
specification revision.

Legacy aliases are not removed automatically. A legacy ID becomes a candidate
for retirement only after every declared consumer class—CSV bindings, report
templates, scripts, and other registered assets—has been checked and none
still references it. v1 can show aliases it resolves, but it does not yet have
that complete consumer registry and therefore makes no stale-alias claim.

## Findings Glossary

The Workbench emits structured findings rather than silently optimizing data.
Their severity is advisory: `info` identifies a candidate to inspect;
`warning` identifies an incomplete or unresolved declaration without blocking
checklist use.

| Code | Severity | Exact trigger | Recommended conclusion |
| --- | --- | --- | --- |
| `CONSTANT_COLUMN` | info | Every dataset record has the same non-empty value in this column. | Inspect value ownership in the authored checklist. When the literal belongs to a target row, move it through an existing predicate chain and verify the re-imported instance before removing the duplicated source column. |
| `REPEATED_LITERAL` | info | A non-empty literal repeats in every populated cell, but some records leave the column blank. | A default-with-override or dictionary candidate exists; blanks may carry meaning and must be reviewed. |
| `HIGH_FAN_OUT_LOOKUP_KEY` | info | One declared lookup provides at least three bound fields and reaches at least one quarter of the current checklist rows. | The lookup is a major operational dependency worth documenting, testing, and considering for a future normalized profile. This measures lookup bindings only, never arbitrary predicate density. |
| `RELATIONSHIP_PACKAGE_MISSING` | warning | `relationships/bindings.json` is absent. | The legacy checklist still loads, but its binding/completeness model has not been declared. |
| `COLUMN_BINDING_TARGET_MISSING` | warning | A header-derived CSV field has no current row or explicit legacy alias. | Add or repair the relevant row/lineage mapping, or remove the obsolete field after reviewing consumers. |
| `MUTATION_SOURCE_TARGET_MISSING` | warning | A declared mutation source does not identify an imported current row. | Repair the declaration; this is not evidence of an undeclared runtime write. |
| `ORPHAN_ROW` | warning | A row has no emitted predicate, lookup/binding, declared mutation source, or declared terminal edge. | Connect it, retire it, or add a justified terminal declaration. Derived display order alone does not resolve this warning. |
| `SELF_ONLY_RELATIONSHIP` | warning | A row has one or more non-lineage predicate edges only to itself, with no non-self predicate or declared dataset, mutation, or terminal support. | Inspect whether it has an external automation source, downstream consumer, stakeholder/terminal purpose, or a missing procedure connection. Keep a valid local calculation, but make its wider purpose visible. |

Input-format findings such as `RELATIONSHIP_DECLARATIONS_INVALID`,
`DATASET_PATH_INVALID`, `DATASET_MISSING`, `LOOKUP_KEY_INVALID`, and
`TERMINAL_ROW_UNKNOWN` identify an unreadable or internally inconsistent
declaration. Their JSON `details` point to the source path, header, slug, or
field involved.

`DIRECT_WRITE_UNDECLARED`, `APPLICABILITY_UNDECLARED`, and a stale-alias
finding are not emitted in v1. Each would require evidence the runtime does
not yet capture: writer provenance, an applicability declaration, or a
complete registered-consumer inventory. They remain potential future checks,
not promises about the current graph.

The UI should render a connected workbench graph and supporting diagnostics. Text findings alone are insufficient.
