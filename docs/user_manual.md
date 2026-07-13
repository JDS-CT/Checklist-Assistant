---
- Version: 0.0.0-0.0.2
- Date: 2026-05-09T07:43:18-05:00
- Project: Checklist Assistant User Manual
- Repository: https://github.com/JDS-CT/checklist-assistant.git
- License: MIT (for reference implementation code)
- Status: Draft public user manual
---

# Checklist Assistant User Manual

This manual is the practical entry point for Checklist Assistant users: humans at the GUI, small local models authoring checklist drafts, larger models using the API, scripts writing measured values, and MCP clients controlling the server. The full specification lives at `spec/checklist-assistant-spec.md`; load this manual first unless you are verifying a specific edge case.

## Table of Contents

- [1. Quick Start: GUI Hello World](#1-quick-start-gui-hello-world)
- [2. Recommended Context Load Order](#2-recommended-context-load-order)
- [3. Checklist Folder Layout](#3-checklist-folder-layout)
- [4. Human Authoring](#4-human-authoring)
- [5. Minimal Markdown Skeleton](#5-minimal-markdown-skeleton)
- [6. SOP-7B: Runtime Execution Assistant](#6-sop-7b-runtime-execution-assistant)
- [7. SOP-24B: Local Assistant Authoring and Operation](#7-sop-24b-local-assistant-authoring-and-operation)
- [8. SOP-70B: Larger Local Model Operation](#8-sop-70b-larger-local-model-operation)
- [9. API Self-Test](#9-api-self-test)
- [10. Agent and GPT-Style HTTP Clients](#10-agent-and-gpt-style-http-clients)
- [11. MCP Clients](#11-mcp-clients)
- [12. Checklist-Local Scripts](#12-checklist-local-scripts)
- [13. Reports, Templates, and Media](#13-reports-templates-and-media)
- [14. Login and Roles](#14-login-and-roles)
- [15. Testing](#15-testing)
- [16. Private Packs and Public Examples](#16-private-packs-and-public-examples)
- [17. Troubleshooting](#17-troubleshooting)
- [18. FAQ](#18-faq)
- [19. Escalate Instead of Guessing](#19-escalate-instead-of-guessing)

## 1. Quick Start: GUI Hello World

In a packaged local build, double-click `checklist_assistant.exe` if it is present and opens the Checklist Assistant UI. In a development build, start the server from PowerShell:

```powershell
.\build\checklist_assistant_server.exe start --host 127.0.0.1 --port 8080
```

Open `http://127.0.0.1:8080/ui/` in a browser. Use the checklist page, choose `Service` mode if you need authoring controls, enable `Allow edits`, and create a test checklist by typing a new checklist name into the checklist filter/select control and confirming creation when prompted.

Add a first row by filling the visible row fields: `Section`, `Procedure`, `Action`, `Spec`, and `Instructions`. Keep `Result`, `Status`, and `Comment` blank for a reusable template row, then click `Save`. Use `Add Row Below` or `New Section` to create a second row.

Run the checklist by entering a `Result`, choosing a `Status` such as `Pass`, `Fail`, `NA`, or `Other`, adding a short `Comment`, and clicking `Save`. Generate output from the report panel with `Generate HTML` or `Generate LaTeX`, then use `Open folder` to inspect the generated report artifacts.

When you want the checklist to become a portable asset, use the Markdown workspace controls to export the current checklist to `checklists/<pack>/<checklist>/checklist.md`. The exported Markdown is the safer canonical source because it includes generated identity lines under `#### Relationships`. To move a whole checklist folder as one file, use the asset-pack archive endpoints to export `.chk`, `.7z`, or `.zip`; `.chk` is a 7-Zip-format archive with a Checklist Assistant extension.

## 2. Recommended Context Load Order

For quick authoring or local model work, load the smallest durable context first:

1. `checklists/examples/checklist_authoring/data/prompt-message-pair.jsonl`
2. `checklists/examples/checklist_authoring/checklist.md`
3. This manual
4. `docs/mcp_tools.md` when using MCP tools
5. `spec/checklist-assistant-spec.md` only for targeted edge cases

The goal is to make the system easy to use even when the implementation is hard to modify. Small models should not have to infer current rules from old examples, long plans, or private checklist packs.

## 3. Checklist Folder Layout

The canonical asset root is `checklists/<pack>/<checklist>/`. The checklist folder is the unit you should share, archive, test, or move when the checklist belongs together.

Checklist Assistant scans for Markdown templates only at `checklists/<pack>/<checklist>/checklist.md`. The `pack` layer is required. If you point Portal Settings at an asset bundle folder, that folder may be either the `checklists` folder itself or a parent folder that contains `checklists`, but the files below it still need the full `checklists/<pack>/<checklist>/checklist.md` shape.

Correct examples:

- `D:\Checklist Assets\checklists\example_pack\startup_checklist\checklist.md`
- `D:\Checklist Assets\checklists\private_pack\calibration\checklist.md`
- `./checklists/chax/initial_setup/checklist.md`

Incorrect examples:

- `D:\Checklist Assets\checklists\startup_checklist\checklist.md`: missing the pack layer, so `startup_checklist` is treated as a pack and no checklist folder is found below it.
- `D:\Checklist Assets\startup_checklist\checklist.md`: missing the `checklists/<pack>/` layers for an asset root.

- `checklist.md`: canonical Markdown template source.
- `data/`: lookup tables, prefill CSVs, parameter maps, and other structured inputs.
- `templates/tex/` and `templates/html/`: custom report templates.
- `media/` or `img/`: images referenced by instructions or reports.
- `scripts/`: checklist-local automation and optional `scripts.json`.
- `reports/`, `saves/`, and `logs/`: runtime artifacts, usually ignored by git.
- `docs/`: checklist-local notes, generated manuals, connector examples, or private/vendor references.

Use relative paths inside checklist packs. Prefer `media/...`, `img/...`, or `./media/...`; avoid machine-specific absolute paths.

### 3.1 Transportable Asset Archives

Checklist Assistant recognizes `.chk`, `.7z`, and `.zip` as transportable asset-pack archives. The preferred extension is `.chk`; it is intentionally just a 7-Zip archive so normal archive tools can inspect it, while Checklist Assistant can treat it as a first-class checklist bundle.

Use `POST /api/v1/workspace/asset-pack/export` to archive one selected `checklists/<pack>/<checklist>/` folder. The archive stores the pack/checklist folder structure plus sidecar files such as `data/`, `media/`, `templates/`, `scripts/`, and checklist-local docs or runtime artifacts that are present in that folder.

Use `POST /api/v1/workspace/asset-pack/import` with `archive_path`, optional `source_name`, optional `pack` or `checklist_dir` overrides, and `replace_files=true` when intentionally overwriting an existing checklist folder. Import restores the files to the selected checklist source, then imports the restored `checklist.md` into SQLite using the normal workspace Markdown import path.

Only the checklist runtime model is imported into the database: rows/slugs, address IDs for the selected instance principal, template relationships, derived address relationships, import order, and source/pack/checklist ownership. Sidecar assets such as CSVs, images, templates, scripts, local docs, reports, saves, and logs remain files in the restored checklist folder and are referenced from there.

## 4. Human Authoring

Use the running server as the source of truth when editing a real checklist. Drafting in Markdown is useful, but import/export through the server is what confirms IDs, relationships, and report behavior.

Core authoring rules:

- `slug_id` is derived from `checklist`, `section`, `procedure`, `action`, `spec`, and `instructions`; changing any of those fields creates a new slug identity.
- A canonical checklist has exactly one `# Checklist: <name>` heading, one or more `## Section: <name>` blocks, and one or more `### Procedure: <name>` blocks inside each section.
- Every procedure block has exactly these bullets in this order: `Action`, `Spec`, `Result`, `Status`, `Comment`.
- Every procedure block has `#### Instructions`; new authoring should also include `#### Relationships`, even when empty.
- Template rows leave `Result`, `Status`, and `Comment` empty. Runtime status values should be `Pass`, `Fail`, `NA`, or `Other`.
- `Procedure`, `Action`, and `Spec` are short summary fields. Put longer operational detail in `Instructions`.
- Each row should describe one primary action. Split setup, execution, and review into separate rows when they can be checked separately.
- Do not invent `slug_id` or `address_id`. Preserve exported source identity lines, or leave relationship blocks empty until an import/export round trip creates real IDs.

Start new checklists with a metadata row in section `_meta`, procedure `Checklist metadata`, and instructions that explain scope, intended operator, required assets, and known limits.

### Composed Prefill

Use existing predicates to keep lookup data with the row that owns it. For example, an upstream row can write a compact selector into a target row; the target row can then use a self-prefill relationship and its own CSV to derive its `result`, `status`, or `comment`. This is a normal predicate chain, not a separate target-traversal feature.

The normal server default permits two predicate hops, which is enough for that source-to-target-to-target-self composition. Set `CHAX_PREDICATE_CHAIN_DEPTH` only when a deliberately longer chain is required. Keep the limit finite and use the Relationship Workbench to inspect loops and dependency scope before raising it.

## 5. Minimal Markdown Skeleton

```markdown
---
Checklist: <CHECKLIST_NAME>
Version: <VERSION>
Date: <ISO_TIMESTAMP>
---

# Checklist: <CHECKLIST_NAME>

## Section: _meta

### Procedure: Checklist metadata
- Action: <purpose>
- Spec: <scope>
- Result:
- Status:
- Comment:

#### Instructions
<freeform checklist context>

#### Relationships

## Section: <SECTION_NAME>

### Procedure: <PROCEDURE_NAME>
- Action: <action>
- Spec: <spec>
- Result:
- Status:
- Comment:

#### Instructions
<instructions text>

#### Relationships
```

Minimal prompt for a general-purpose model:

```text
You are generating a checklist markdown file for Checklist Assistant.
Output only markdown, no commentary.
Use the canonical format exactly as shown.
Checklist name: <CHECKLIST_NAME>
Sections and procedures:
- <SECTION_1>
  - <PROCEDURE_1>: Action=<...>, Spec=<...>, Instructions=<...>
Rules:
- Use # Checklist: <name> exactly once.
- Each procedure block must have Action, Spec, Result, Status, Comment in that order.
- Include Instructions and Relationships for every procedure.
- Leave Result, Status, and Comment empty for template rows.
- Use ASCII punctuation and avoid tabs.
```

## 6. SOP-7B: Runtime Execution Assistant

A 7B-class model is not expected to author checklists. Its realistic job is to be a small execution-time assistant on top of a running Checklist Assistant server: read the next action, optionally read the instructions, accept noisy typed or voice-transcribed operator input, write the operator's result to the API, wait briefly for Checklist Assistant's relationship/predicate logic to update status, read the row back, request a comment only when useful, and move to the next action.

This is the right target for small local models because the hard logic stays in Checklist Assistant. The model should not invent checklist rows, rewrite specs, create relationships, choose status from its own reasoning, or repair unclear procedure content. It is a fuzzy input layer for deterministic commands.

### 6.1 Runtime Assumptions

- The Checklist Assistant server is already running.
- The model is given `CHAX_BASE_URL`, defaulting to `http://127.0.0.1:8080`.
- The model is given either an existing `checklist` + `instance_id`, or a `pack` + `checklist` + `instance_principal` it may import as a new instance.
- The checklist's automatic behavior, such as `BoolVerify...Status` relationships, has already been authored by humans or larger models.
- The model may receive imperfect typed input or speech transcripts, but it should normalize only simple control phrases such as `skip`, `details`, `repeat`, `comment`, `next`, and `stop`.

### 6.2 Minimum API Surface

The small model should know only these calls:

1. Optional server check: `GET /api/v1/health`.
2. Optional template discovery: `GET /api/v1/workspace/markdown/templates`.
3. Spawn a known checklist instance from a workspace template: `POST /api/v1/workspace/markdown/import`.
4. Load rows for one instance: `GET /api/v1/slugs?checklist=<checklist>&instance_id=<instance_id>&limit=200`.
5. Read one row after an update: `GET /api/v1/slugs/<address_id>`.
6. Write operator input: `PATCH /api/v1/slugs/<address_id>` with only `result`, `comment`, or an explicit operator status such as `NA` when the user asks for that.
7. Optional diagnostic read: `GET /api/v1/evaluate/slug/<address_id>`.

Do not use `POST /api/v1/slugs`, relationship endpoints, Markdown authoring endpoints, direct SQLite access, or report generation in the 7B runtime loop unless a larger controller explicitly gives a narrow scripted task.

### 6.3 Spawn or Attach

If the operator already selected an instance, attach to it:

```text
checklist = chax-demo
instance_id = 6668WBKBWQ1K5ZEE
GET /api/v1/slugs?checklist=chax-demo&instance_id=6668WBKBWQ1K5ZEE&limit=200
```

If the operator asks for a new instance from a known workspace template, import it server-side:

```http
POST /api/v1/workspace/markdown/import
Content-Type: application/json

{
  "pack": "chax",
  "checklist": "chax-demo",
  "instance_principal": "operator-session||<local-name-or-timestamp>",
  "replace_instance": true,
  "apply_data": false
}
```

Use the returned `data.instance_id` for every later list/read/update call. The 7B model should not parse `checklist.md`; the server imports the known template.

### 6.4 Execution Loop

For each row, ordered as returned by `list_slugs`:

1. Say or display `section`, `procedure`, and `action`.
2. If the operator says `details`, `instructions`, or `more`, display `instructions`.
3. If the operator says `repeat`, repeat the action.
4. If the operator says `skip`, move to the next row without writing.
5. If the operator says `not applicable` or `mark NA`, patch `{"status":"NA","comment":"Marked NA by operator."}` or ask for a short comment first when deployment policy requires one.
6. Otherwise treat the operator text as the row `result` and patch `{"result":"<operator text>"}`.
7. Wait about 500-1000 ms.
8. Read the same row with `GET /api/v1/slugs/<address_id>`.
9. Report the current `status`, `spec`, and normalized `result` back to the operator.
10. If `status` is `Fail` or `Other`, ask for a comment; patch `{"comment":"<operator comment>"}` if provided, or continue if the operator says `skip comment`.
11. Move to the next row.

The model should not decide that a result passed or failed. It should observe the status returned by Checklist Assistant. If status remains `Unknown`, it may read `GET /api/v1/evaluate/slug/<address_id>` and report the diagnostic, but it should not invent a final status.

### 6.5 Minimal Command Examples

Load rows:

```http
GET /api/v1/slugs?checklist=chax-demo&instance_id=<INSTANCE_ID>&limit=200
```

Present to operator:

```text
Action: <row.action>
Say "details" for instructions, "skip" to move on, or enter the result.
```

Write a result:

```http
PATCH /api/v1/slugs/<ADDRESS_ID>
Content-Type: application/json

{
  "result": "25"
}
```

Read back status after a short delay:

```http
GET /api/v1/slugs/<ADDRESS_ID>
```

Optional evaluation diagnostic:

```http
GET /api/v1/evaluate/slug/<ADDRESS_ID>
```

Write a comment only when needed:

```http
PATCH /api/v1/slugs/<ADDRESS_ID>
Content-Type: application/json

{
  "comment": "Operator entered 26, which does not match spec 25."
}
```

### 6.6 Unit-Test Target

The minimum automated test for this 7B runtime surface should cover exactly this path:

1. Import a known workspace checklist into a new instance with `POST /api/v1/workspace/markdown/import`.
2. List rows with `GET /api/v1/slugs?checklist=...&instance_id=...`.
3. Pick or fixture a row with a `BoolVerify...Status` relationship.
4. Patch only `result` and verify Checklist Assistant updates status.
5. Read the row back with `GET /api/v1/slugs/<address_id>`.
6. For a failing or `Other` status, patch only `comment`.

This test should intentionally avoid checklist authoring, relationship design, direct database writes, and model reasoning about whether the result should pass.

## 7. SOP-24B: Local Assistant Authoring and Operation

A 24B-class local model can usually load this manual, the authoring example, selected spec sections, and the active checklist. It can help with import/export loops, relationship cleanup, report token placement, and script mapping reviews as long as the task stays bounded.

Recommended 24B workflow:

1. Load the active checklist plus Sections 3, 4, 9, 12, and 13 of this manual.
2. Use the API self-test to confirm create/read/patch/list behavior before declaring a checklist operational.
3. Use targeted spec links for relationships, predicate grammar, report placeholders, and compatibility decisions.
4. Prefer HTTP API writes over direct database edits.
5. Treat scripts, reports, and generated logs as checklist-local assets.

This model size is a good baseline for secure local deployment on one strong workstation. It should be able to operate Checklist Assistant comfortably, but codebase-wide development still belongs to a larger model or a human reviewer.

## 8. SOP-70B: Larger Local Model Operation

A 70B-class model can usually reason over the manual, active spec sections, API schemas, and a larger slice of implementation context. It may be able to author through the API or database concepts directly, but normal operation should still use documented HTTP/MCP contracts so the audit trail and entity attribution stay intact.

Recommended 70B workflow:

1. Load this manual, the active checklist pack, `docs/mcp_tools.md` when applicable, and only the spec sections needed for the feature family under review.
2. Use SQLite and source-code context for diagnosis, not as the default mutation path.
3. Keep generated authoring changes round-tripped through import/export so IDs and relationships are normalized by the server.
4. When proposing new features, add generic examples to public packs instead of relying on private deployments as documentation.
5. Run targeted tests or smoke workflows after API, script, report, or predicate changes.

This section is deliberately provisional. As local model capability rises, Checklist Assistant should keep shifting routine operation toward smaller, cheaper, local executors while keeping implementation work testable and reviewable.

## 9. API Self-Test

Use this loop before calling a new checklist or connector ready:

1. Start or connect to `http://127.0.0.1:8080` unless `CHAX_HOST`, `CHAX_PORT`, or `CHAX_BASE_URL` override it.
2. Create one test row with `POST /api/v1/slugs` and include `checklist`, `section`, `procedure`, `action`, `spec`, `instructions`, and `instance_principal`.
3. Save the returned `address_id`, `slug_id`, and `instance_id`.
4. Read the created row with `GET /api/v1/slugs/{address_id}` and confirm immutable fields match.
5. Patch mutable fields with `PATCH /api/v1/slugs/{address_id}` using `result`, `status`, or `comment`.
6. List rows with `GET /api/v1/slugs?checklist=<name>&instance_id=<id>` and confirm the row can be discovered without guessing identifiers.
7. For reports, run `POST /api/v1/export/report` with `checklist` and `instance_id`, then confirm the output folder and JSONL sidecar exist.

When authentication is enabled, use a bearer token or the admin login flow described below.

## 10. Agent and GPT-Style HTTP Clients

Agent clients should be thin HTTP clients. They should validate required fields, call documented endpoints, report method/URL/status/error details, and avoid local file access unless the deployment explicitly grants it.

Core endpoints for an HTTP action client:

- `GET /api/v1/health`
- `GET /api/v1/commands`
- `POST /api/v1/slugs`
- `GET /api/v1/slugs/{address_id}`
- `GET /api/v1/slugs?checklist=<name>&section=<section>&status=<status>&limit=<n>&offset=<n>`
- `PATCH /api/v1/slugs/{address_id}`
- `POST /api/v1/slugs/bulk-update`
- `GET /api/v1/export/markdown/{checklist}`
- `POST /api/v1/import/markdown?checklist=<name>&instance_principal=<principal>`
- `POST /api/v1/export/report`

Do not invent endpoints or fields. Prefer `GET /api/v1/commands` for discovery, keep batches small, and surface server `error` or `message` fields instead of hiding them in prose.

## 11. MCP Clients

The MCP bridge is documented in `docs/mcp_tools.md`. Keep that file as the tool catalog source because tool names and schemas need to stay stable for agents and voice clients.

MCP clients should treat tools as wrappers around the Checklist Assistant HTTP API. Prefer small composable tools, pass explicit `checklist` and `instance_id` values when mutating state, and keep error handling machine-parseable so an agent can retry or ask for missing context.

## 12. Checklist-Local Scripts

Checklist-local automation belongs under `checklists/<pack>/<checklist>/scripts/`. A `scripts.json` manifest may define labels, descriptions, commands, default args, `start_args`, and `stop_args`; without a manifest, the reference implementation can auto-discover common runnable scripts.

Scripts should use HTTP API calls, not direct database writes. Resolve `slug_id` to `address_id` through API reads when needed, then patch mutable fields with `PATCH /api/v1/slugs/{address_id}`.

When launched through the scripts API, scripts receive context through environment variables such as `CHAX_HOST`, `CHAX_PORT`, `CHAX_BASE_URL`, `CHAX_CHECKLIST`, `CHAX_PACK`, `CHAX_SCRIPT_ID`, `CHAX_INSTANCE_ID`, and `CHAX_INSTANCE_PRINCIPAL`. Logs belong beside the checklist under `logs/`.

## 13. Reports, Templates, and Media

Place report templates under `templates/tex/` or `templates/html/` inside the checklist folder. Store images under `media/` or `img/`, and reference them with relative paths.

Report placeholders use `{{name}}` syntax. Row placeholders can use either address IDs or slug IDs:

- `slug_<address_id>_<field>` or `slug_<slug_id>_<field>`
- `slug-<address_id>-<field>` or `slug-<slug_id>-<field>`

Supported row fields include `procedure`, `action`, `spec`, `result`, `status`, `comment`, `instructions`, and `instance_principal`. Add `_keep` or `-keep` to inject a row field while keeping that row in auto-generated tables.

Generated report folders live under `checklists/<pack>/<checklist>/reports/<instance_id>_<timestamp>/` and usually contain renderer output plus a JSONL sidecar.

### Captured Report Images

Images captured while a checklist is running are staged separately from authored `media/` and `img/` assets. Put an ignored, instance-specific manifest at `reports/<instance_id>/images/manifest.json`; the next HTML or LaTeX export copies the manifest and every declared file into the generated report folder's `images/` directory. That makes the generated report self-contained and lets the source capture area be cleaned independently later.

The manifest uses UTF-8 JSON and the `chax-report-images-v1` schema:

```json
{
  "schema": "chax-report-images-v1",
  "images": [
    {
      "preview": "capture-001.png",
      "original": "capture-001.tif",
      "procedure_slug_id": "optional-row-id",
      "procedure": "Optional procedure label",
      "caption": "What the operator observed",
      "captured_at": "2026-01-01T00:00:00Z",
      "source": "Optional capture tool"
    }
  ]
}
```

`preview` is required and must be a PNG or JPEG relative to the manifest directory so both HTML and LaTeX can render it. `original` is optional and may name the retained full-resolution source file. All declared paths must be relative and may not contain `.` or `..`; exports reject unsafe manifests rather than reading files outside the checklist's evidence folder.

Use `{{CapturedImages}}` in an HTML template and `{{CapturedImageFigures}}` in a LaTeX template. The default templates include these placeholders. The renderer groups previews in pages of four (a two-by-two grid), carries captions and provenance into the visible report, and preserves the original files under `images/`. Export responses include `image_count` and `images_manifest_path` when image evidence is present.

## 14. Login and Roles

Runtime admin credentials come from environment variables or generated startup credentials. Tests may use a hard-coded helper password in in-memory servers, but the normal runtime should not assume that password.

Admin login flow:

1. Start the server with `.\build\checklist_assistant_server.exe start --host 127.0.0.1 --port 8080`.
2. Provide `ADMIN_USER` and `ADMIN_PASSWORD`, or read the generated password from `.chax/logs/server-*.err`.
3. Open `http://127.0.0.1:8080/ui/` and click `Login with OAuth`.
4. Sign in as `admin` or the configured admin user.

Deterministic local password example:

```powershell
$env:ADMIN_PASSWORD="password"
.\build\checklist_assistant_server.exe start --host 127.0.0.1 --port 8080
```

Current roles are still evolving. The practical model is bootstrap admin for setup, admin for checklist/user management, user for assigned checklist work, and guest for view-only access unless elevated by deployment policy.

## 15. Testing

Local read-only smoke test:

```powershell
ctest -R e2e-smoke --output-on-failure
```

Useful environment variables:

- `CHAX_BASE_URL`: target URL, default `http://127.0.0.1:8080`.
- `CHAX_TOKEN`: bearer token, skips login.
- `CHAX_ADMIN_USER`: admin username, default `admin`.
- `CHAX_ADMIN_PASSWORD`: admin password for token acquisition.
- `CHAX_CLIENT_ID` and `CHAX_CLIENT_SECRET`: OAuth client, default `chax-ui-client` and `chax-ui-secret`.
- `CHAX_ALLOW_REMOTE`: set to `1` to allow write operations during remote smoke tests.
- `CHAX_READONLY`: force read-only behavior even when remote writes are otherwise allowed.

Read-only mode calls health and command discovery. Write mode creates a smoke checklist/slug, patches it, lists it, generates a report, and attempts cleanup. Remote write tests require `CHAX_ALLOW_REMOTE=1`.

## 16. Private Packs and Public Examples

Keep private or customer-specific material inside the relevant private checklist pack, usually under `checklists/<private-pack>/<checklist>/docs/`, `data/`, or `scripts/`. Public docs should use generic examples unless a private artifact has been intentionally redacted into a public demo.

For local private assets, prefer a sibling asset bundle outside the public repo and add it in Portal Settings as `source_name=path`. The saved path stays in `.chax/local_settings.json` on that PC, and the bundle should contain the same required layout: `checklists/<pack>/<checklist>/checklist.md`.

`CHAX-CLIENT/` is the application client. Avoid using root-level `clients/` for customer material or external-system experiments; those belong in the checklist pack that owns the integration.

## 17. Troubleshooting

### Checklists From an Extra Asset Root Do Not Load

Verify the folder shape first. The scanner does not load a checklist directly inside the `checklists` folder; it expects `checklists/<pack>/<checklist>/checklist.md`.

If Portal Settings says the root saved but the Markdown workspace does not show the checklist, inspect the path and make sure there is one folder for the pack and one folder for the checklist. For example, move `D:\Assets\checklists\startup\checklist.md` to `D:\Assets\checklists\my_pack\startup\checklist.md`, then click `Refresh` in the Markdown workspace.

If you selected a parent folder, confirm it contains a child folder named `checklists`. If you selected the `checklists` folder itself, confirm its immediate children are pack folders, not checklist folders.

### Export Creates a Checklist in the Public Root

Use the Markdown workspace selection when exporting so the UI can send `source_name` and `pack` back to the server. If you type only a checklist name and leave source context ambiguous, the server may fall back to the default public pack for new exports.

Before deleting any accidental export folder, refresh the Markdown workspace and confirm the intended private/source copy exists and contains the expected `checklist.md`.

### Duplicate Checklist Names Are Ambiguous

The same checklist name can exist in more than one source or pack, but humans and agents should treat that as an explicit choice point. Select the exact source/pack/checklist item in the Markdown workspace before importing, exporting, running scripts, or generating reports.

Checklist Assistant now persists source/pack/checklist-folder ownership for imported workspace rows. When one checklist instance has multiple owners, workspace export and report generation require explicit source/pack context or return an ambiguity error instead of guessing.

## 18. FAQ

### What Is A Pack?

A pack is the folder between `checklists` and the checklist folder. It groups related checklists and assets, such as public examples, unit tests, a private asset bundle, or a stakeholder-specific integration. The required shape is always `checklists/<pack>/<checklist>/checklist.md`.

### Can I Point Portal Settings At A Parent Folder?

Yes. If the folder contains a child named `checklists`, Checklist Assistant stores and loads that `checklists` child. If you point directly at a `checklists` folder, it uses that folder as the asset root.

### Should Private Asset Packs Live Inside The Public Repo?

No. Keep private packs in sibling folders or sibling repos, then add them through Portal Settings. This keeps the public repo usable by itself while preventing private material from becoming mixed with public source control.

### Can Two Packs Use The Same Checklist Name?

They can, but duplicate names should be handled deliberately. The UI carries source and pack context for workspace import/export paths, and the runtime stores ownership in `slug_ownership` and `address_ownership` so matching row identities can be reused without losing the owning pack.

## 19. Escalate Instead of Guessing

Stop and ask for review when a relationship target is ambiguous, a deterministic spec grammar matters and is unclear, a workflow depends on legacy compatibility, a script would need direct database access, or a checklist pack would need embedded secrets or hidden filesystem writes.
