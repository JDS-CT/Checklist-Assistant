# Checklist: checklist_authoring

## Section: _meta

### Procedure: Checklist metadata
- Action: Define scope
- Spec: Metadata row present
- Result:
- Status:
- Comment:

#### Instructions
Use this checklist to author a new checklist pack from prose instructions. Read `data/prompt-message-pair.jsonl` first, then continue through this checklist in order. Use `docs/user_manual.md` when a rule or edge case is unclear. Keep examples generic unless the source material requires a real product or organization name.

#### Relationships

## Section: Context Load

### Procedure: Capture source text
- Action: Collect source text
- Spec: Source text isolated
- Result:
- Status:
- Comment:

#### Instructions
Copy only the paragraphs needed for the target checklist. Mark stable nouns, repeated measurements, required tools, pass/fail cues, and external systems. If a paragraph contains more than one action or hidden assumptions, mark it for splitting before drafting rows.

#### Relationships

---

### Procedure: Choose pack + checklist
- Action: Set pack + checklist
- Spec: Stable names chosen
- Result:
- Status:
- Comment:

#### Instructions
Create or target a path shaped like `checklists/<pack>/<checklist>/`. Keep the checklist heading aligned with the intended checklist name. Avoid temporary names such as `draft`, `test2`, or `new checklist` unless the checklist is explicitly disposable.

#### Relationships

## Section: Structure Draft

### Procedure: Create sections
- Action: Group into sections
- Spec: One theme per section
- Result:
- Status:
- Comment:

#### Instructions
Create sections that help a human or small model understand the workflow order. Use a `_meta` section first, then separate setup, execution, review, automation, or asset work as needed. If a section grows too large, split it before writing procedure rows.

#### Relationships

---

### Procedure: Split source into rows
- Action: Split into row drafts
- Spec: One action per row
- Result:
- Status:
- Comment:

#### Instructions
If one paragraph says to prepare, act, observe, and document, split that paragraph into multiple procedure rows. Keep the `Procedure` noun-like and keep the `Action` verb-like. Reuse the same canonical noun every time the same object appears.

#### Relationships

---

### Procedure: Draft canonical blocks
- Action: Write canonical blocks
- Spec: Required fields in order
- Result:
- Status:
- Comment:

#### Instructions
For every row, write `### Procedure:`, then the five bullets in this exact order: `Action`, `Spec`, `Result`, `Status`, `Comment`. Keep `Result`, `Status`, and `Comment` empty in the template. Add `#### Instructions` and `#### Relationships` after the bullets for every row.

#### Relationships

---

### Procedure: Shorten top fields
- Action: Trim top fields
- Spec: Summary fields stay short
- Result:
- Status:
- Comment:

#### Instructions
Treat `Procedure`, `Action`, and `Spec` as short cues, not paragraph inputs. Default soft limit is about 32 characters each. Only stretch toward about 64 when preserving a standardized external label or address/company/city-style data contract. Move the larger explanation into `Instructions`.

#### Relationships

---

### Procedure: Write explicit instructions
- Action: Write explicit instructions
- Spec: Steps are unambiguous
- Result:
- Status:
- Comment:

#### Instructions
Avoid pronouns, vague shorthand, and assumed domain knowledge. If a row needs a tool, menu name, button, file path, or measurement location, name it explicitly. If success is observable, say what the user should see, record, or compare before marking the row complete.

#### Relationships

---

### Procedure: Choose spec style
- Action: Set concise specs
- Spec: Short, stable expectation
- Result:
- Status:
- Comment:

#### Instructions
Use plain-language specs for manual reviews and simple scalar forms when automation or Verify rules are expected. Prefer forms such as `Unknown`, `Visible and intact`, `[min,max] unit`, or `>0 unit` over freeform prose. If the acceptable range is not actually known, do not fake precision.

#### Relationships

## Section: Automation and Assets

### Procedure: Handle relationships
- Action: Add safe relationships
- Spec: No invented IDs
- Result:
- Status:
- Comment:

#### Instructions
If you are editing exported Markdown, preserve the first bullet under `#### Relationships` because it is the source identity line. If you are authoring from scratch and do not have real IDs yet, leave the relationship block empty until import or export gives you real identifiers. Do not invent `slug_id` or `address_id` values.

#### Relationships

---

### Procedure: Add media paths
- Action: Record media paths
- Spec: Relative paths only
- Result:
- Status:
- Comment:

#### Instructions
Put reusable files under `media/` or `img/` inside the checklist pack. Use forms such as `./media/example.png` or `media/example.png`. Do not use root-style paths such as `/media/example.png`, and do not use absolute machine paths.

#### Relationships

---

### Procedure: Add API script bundle
- Action: Add API-only scripts
- Spec: Patch mutable fields only
- Result:
- Status:
- Comment:

#### Instructions
If the checklist needs automation, put it under `scripts/` with a `scripts.json` manifest. Resolve `slug_id` to `address_id` through API reads, then use `PATCH /api/v1/slugs/{address_id}` for `result`, `status`, and `comment`. Respect `CHAX_HOST`, `CHAX_PORT`, and `CHAX_INSTANCE_ID` instead of hardcoding environment-specific values.

#### Relationships

## Section: Validation

### Procedure: Run API self-test
- Action: Run API self-test
- Spec: Create/read/patch/list pass
- Result:
- Status:
- Comment:

#### Instructions
Create a temporary row with `POST /api/v1/slugs` using `checklist`, `section`, `procedure`, `action`, `spec`, `instructions`, and `instance_principal`. Save `address_id` and `instance_id` from the response. Read the row with `GET /api/v1/slugs/{address_id}`, patch it with `PATCH /api/v1/slugs/{address_id}`, and confirm discovery with `GET /api/v1/slugs?checklist=<name>&instance_id=<id>`. If report output matters, also run `POST /api/v1/export/report`.

#### Relationships

---

### Procedure: Review handoff
- Action: Review handoff quality
- Spec: Authoring baseline passes
- Result:
- Status:
- Comment:

#### Instructions
Verify that the checklist has a metadata row, canonical block order, explicit instructions, no invented IDs, and no hidden dependencies on brand-specific knowledge unless intentionally required. If authoring conventions changed during this work, update `data/prompt-message-pair.jsonl` in the same change so future small-model runs start from the current compressed context.

#### Relationships
