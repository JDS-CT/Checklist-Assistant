# Checklist: Unit Tests

## Section: automation_and_reporting

### Procedure: export Unit Tests report
- Action: generate the report after tests
- Spec: report tex & json present
- Result: 
- Status: 
- Comment: 

#### Instructions
1. Ensure `CHAX_TOKEN` or `CHAX_ADMIN_PASSWORD` is set.
2. Run: `build/unit-test-controller --skip-tests --export-report --export-markdown`
   (Windows: `build\unit-test-controller.exe --skip-tests --export-report --export-markdown`)
   Add `--export-include-data` to include runtime fields. The default export filename is
   `Unit Tests.export.md` to keep the template file untouched; use `--export-filename Unit Tests.md`
   only when you intend to overwrite the template.
3. Confirm the report output exists under `workspace/checklists/reports`.

#### Relationships
- JVJW4XC75Y6E3641


---

### Procedure: export Unit Tests report
- Action: generate the report after tests
- Spec: report tex & json present
- Result: 
- Status: 
- Comment: 

#### Instructions
1. Ensure `CHAX_TOKEN` or `CHAX_ADMIN_PASSWORD` is set.
2. Run: `build/unit-test-controller --skip-tests --export-report --export-markdown`
   (Windows: `build\unit-test-controller.exe --skip-tests --export-report --export-markdown`)
   Add `--export-include-data` to include runtime fields, or `--export-filename Unit Tests.export.md`
   to keep the template file untouched.
3. Confirm the report output exists under `workspace/checklists/reports`.

#### Relationships
- P96MW01A7HT47S8B


---

### Procedure: export Unit Tests report
- Action: generate the report after tests
- Spec: report tex & json present
- Result: 
- Status: 
- Comment: 

#### Instructions
1. Ensure `CHAX_TOKEN` or `CHAX_ADMIN_PASSWORD` is set.
2. Run: `build/unit-test-controller --skip-tests --export-report --export-markdown`
   (Windows: `build\unit-test-controller.exe --skip-tests --export-report --export-markdown`)
   Add `--export-include-data` to include runtime fields and `--pack unit-tests` to target the unit-tests asset pack.
3. Confirm the report output exists under `checklists/<pack>/<checklist>/reports`.

#### Relationships
- KND94RKRY3ZP0BAG
- slugPredecessor NA07BYKAXQGTTDH3
- slugPredecessor P96MW01A7HT47S8B


---

### Procedure: update Unit Tests row via API wrapper
- Action: run wrapper and PATCH row
- Spec: status/result/comment set
- Result: 
- Status: 
- Comment: 

#### Instructions
1. Ensure the server is running and set `CHAX_TOKEN` or `CHAX_ADMIN_PASSWORD` (must match the
   server's `ADMIN_PASSWORD`).
   (Optional: `CHAX_ADMIN_USER`, `CHAX_CLIENT_ID`, `CHAX_CLIENT_SECRET`.)
2. Build: `cmake --build build`
3. Run: `build/unit-test-controller --export-report --export-markdown`
   (Windows: `build\unit-test-controller.exe --export-report --export-markdown`)
4. Override the target row with `--section` and `--procedure` as needed.
5. Export writes to `workspace/checklists/templates/Unit Tests.export.md` by default.
   Results remain blank unless `--export-include-data` is set.
6. Use `--export-filename Unit Tests.md` only when you intentionally want to overwrite the
   template file.
7. Use `--replace-checklist` after template changes to refresh stored slugs.
8. Run all wired tests with `build/unit-test-controller --all --export-report --export-markdown --refresh-template`.

#### Relationships
- ASDC55PZSC03RPWN


---

### Procedure: update Unit Tests row via API wrapper
- Action: run wrapper and PATCH row
- Spec: status/result/comment set
- Result: 
- Status: 
- Comment: 

#### Instructions
1. Ensure the server is running and set `CHAX_TOKEN` or `CHAX_ADMIN_PASSWORD` (must match the
   server's `ADMIN_PASSWORD`).
   (Optional: `CHAX_ADMIN_USER`, `CHAX_CLIENT_ID`, `CHAX_CLIENT_SECRET`.)
2. Build: `cmake --build build`
3. Run: `build/unit-test-controller --export-report --export-markdown`
   (Windows: `build\unit-test-controller.exe --export-report --export-markdown`)
4. Override the target row with `--section` and `--procedure` as needed.
5. Export writes to `checklists/<pack>/<checklist>/checklist.md` by default; results remain blank unless `--export-include-data` is set.
6. Use `--replace-checklist` after template changes to refresh stored slugs.
7. Run all wired tests with `build/unit-test-controller --all --export-report --export-markdown --refresh-template`.

#### Relationships
- 57VA1RXJ53NQ89GR
- slugPredecessor 3P0ZAG8RFVJ9YYRF
- slugPredecessor MYGWQFFBFT2EHT6J


---

### Procedure: update Unit Tests row via API wrapper
- Action: run wrapper and PATCH row
- Spec: status/result/comment set
- Result: 
- Status: 
- Comment: 

#### Instructions
1. Ensure the server is running and set `CHAX_TOKEN` or `CHAX_ADMIN_PASSWORD` (must match the
   server's `ADMIN_PASSWORD`).
   (Optional: `CHAX_ADMIN_USER`, `CHAX_CLIENT_ID`, `CHAX_CLIENT_SECRET`.)
2. Build: `cmake --build build`
3. Run: `build/unit-test-controller --export-report --export-markdown`
   (Windows: `build\unit-test-controller.exe --export-report --export-markdown`)
4. Override the target row with `--section` and `--procedure` as needed.
5. Export overwrites `workspace/checklists/templates/Unit Tests.md` with address_id relationships.
   Results remain blank unless `--export-include-data` is set.
6. Use `--export-filename Unit Tests.export.md` to keep the template file untouched.
7. Use `--replace-checklist` after template changes to refresh stored slugs.

#### Relationships
- MYGWQFFBFT2EHT6J


---

### Procedure: update Unit Tests row via API wrapper
- Action: run wrapper and PATCH row
- Spec: status/result/comment set
- Result: 
- Status: 
- Comment: 

#### Instructions
1. Ensure the server is running and set `CHAX_TOKEN` or `CHAX_ADMIN_PASSWORD` (must match the
   server's `ADMIN_PASSWORD`).
   (Optional: `CHAX_ADMIN_USER`, `CHAX_CLIENT_ID`, `CHAX_CLIENT_SECRET`.)
2. Build: `cmake --build build`
3. Run: `build/unit-test-controller --export-report --export-markdown`
   (Windows: `build\unit-test-controller.exe --export-report --export-markdown`)
4. Override the target row with `--section` and `--procedure` as needed.
5. Export overwrites `workspace/checklists/templates/Unit Tests.md` with address_id relationships.
   Results remain blank unless `--export-include-data` is set.
6. Use `--export-filename Unit Tests.export.md` to keep the template file untouched.
7. Use `--replace-checklist` after template changes to refresh stored slugs.
8. Run all wired tests with `build/unit-test-controller --all --export-report --export-markdown --refresh-template`.

#### Relationships
- 2DAAQ60ZMNQ4HRPE


---

## Section: integration_schema_test

### Procedure: run integration-schema test
- Action: run integration-schema test
- Spec: integration-schema test passed
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R integration-schema`. The test fails if any integration-schema step
fails.

#### Relationships
- 7MF201SVD949SR0T


---

### Procedure: create primary slug
- Action: create primary slug
- Spec: slug created
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R integration-schema`. The test fails if the first slug is not created.

#### Relationships
- J9XSA5KTE7N458GF


---

### Procedure: create second slug
- Action: create second slug
- Spec: slug created
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R integration-schema`. The test fails if the second slug is not created.

#### Relationships
- KW3DZ0J2BWF2BNP4


---

### Procedure: list checklists
- Action: list checklists
- Spec: checklist listed
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R integration-schema`. The test fails if the checklist list does not
return the expected name.

#### Relationships
- 9D4HBE8K76ZR8RTY


---

### Procedure: list slugs for checklist
- Action: list slugs for checklist
- Spec: slugs matched
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R integration-schema`. The test fails if the checklist slug list does
not return the two expected entries.

#### Relationships
- 6K9VMHDAJVG45SCX


---

### Procedure: filter slugs by status
- Action: filter slugs
- Spec: filtered query matched
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R integration-schema`. The test fails if the status filter does not
return the expected slug.

#### Relationships
- JAC2YDR1705G8BQQ


---

### Procedure: history limit returns entries
- Action: query history
- Spec: history returned entries
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R integration-schema`. The test fails if history limit returns
no entries.

#### Relationships
- ZDYXNRSRM4XTCR33


---

## Section: markdown_compat_tests

### Procedure: checklist heading sets checklist name
- Action: verify H1 checklist match
- Spec: checklist == Generic Intake Form
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-compat`. The test fails with "Unexpected checklist name:"
if the heading is not parsed correctly.

#### Relationships
- THYFKYCPYETW0JF4


---

### Procedure: first procedure fields parsed
- Action: verify first row fields
- Spec: section/proc/action/spec ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-compat`. The test fails with "Unexpected section",
"Unexpected procedure", "Unexpected action", or "Unexpected spec" if any field is misread.

#### Relationships
- FFQDYWJWBV5XNH2V


---

### Procedure: front-matter mismatch is tolerated
- Action: accept front-matter mismatch.
- Spec: parse succeeds
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-compat`. The fixture front-matter uses
`<sample_intake_form>` while the H1 heading is "Generic Intake Form"; parsing should
still succeed.

#### Relationships
- PPS9KPMS4NS4Q9NR


---

### Procedure: instructions allow nested headings
- Action: keep nested instr heading
- Spec: Notes heading preserved
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-compat`. The test fails with "Unexpected instructions"
if the nested heading is lost or mis-parsed.

#### Relationships
- 4E9H8H9GTX8HV6HT


---

### Procedure: parses two procedures from fixture
- Action: count parsed procedures
- Spec: slug count == 2
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-compat`. The test fails with "Expected 2 slugs"
if the parser misses a procedure.

#### Relationships
- Y6HXME4W1M1T84BK


---

### Procedure: run markdown-compat test
- Action: run markdown-compat test
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
1. Build if needed: `cmake --build build`
2. Run: `ctest --test-dir build -R markdown-compat`
3. On failure, read stdout/stderr for the first "Unexpected ..." message from `tests/markdown_compat_test.cpp`.

#### Relationships
- 1WR91HJMG584WQQ6


---

### Procedure: second procedure fields parsed
- Action: verify second row fields
- Spec: procedure/action match
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-compat`. The test fails with "Unexpected second procedure"
or "Unexpected second action" if the second row is misread.

#### Relationships
- S2J6FMQEE7KNHXQJ


---

## Section: oauth_store_repro_test

### Procedure: get client first
- Action: get client first
- Spec: client found
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-store-repro`. The test fails if the first client lookup fails.

#### Relationships
- 3P0B83SDQBZKVXWP


---

### Procedure: get client second
- Action: get client second
- Spec: client found
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-store-repro`. The test fails if the second client lookup fails.

#### Relationships
- W8DADE1P44HAYKHK


---

### Procedure: list checklists
- Action: list checklists
- Spec: checklist listed
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R integration-schema`. The test fails if the checklist list is empty
or missing the expected name.

#### Relationships
- G5SAS7NFM5YS5WGW


---

### Procedure: list slugs for checklist
- Action: list slugs
- Spec: slugs matched
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R integration-schema`. The test fails if the slugs are missing or
do not match the inserted addresses.

#### Relationships
- 767XRQK3WZWWVSQ0


---

### Procedure: run integration-schema test
- Action: run integration-schema test
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R integration-schema`. For controller wiring, use:
`build/unit-test-controller --test-regex integration-schema --section integration_schema_test --procedure run integration-schema test`

#### Relationships
- VPFE02GTMPY814CP


---

### Procedure: run oauth-store-repro test
- Action: run oauth-store-repro test
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-store-repro`. For controller wiring, use:
`build/unit-test-controller --test-regex oauth-store-repro --section oauth_store_repro_test --procedure run oauth-store-repro test`

#### Relationships
- ACVZEMBQ520839RG


---

### Procedure: upsert oauth client
- Action: upsert oauth client
- Spec: client upserted
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-store-repro`. The test fails if the client upsert is missing.

#### Relationships
- 7CZTBZXMCVMVBNDM


---

## Section: markdown_relationships_test

### Procedure: parse markdown
- Action: parse markdown
- Spec: slugs parsed
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-relationships`. The test fails if the markdown fixture does
not parse into two slugs.

#### Relationships
- 8SDYT3GDRK6PEFC8


---

### Procedure: validate slug ids
- Action: check slug ids
- Spec: slug ids match
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-relationships`. The test fails if computed slug IDs do not
match the parsed values.

#### Relationships
- TZG2XPYMY39Y40YV


---

### Procedure: template relationships
- Action: check template rels
- Spec: rels parsed
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-relationships`. The test fails if template relationships
are missing.

#### Relationships
- 5R10KRDWHPT2DA2K


---

### Procedure: address relationships
- Action: check address rels
- Spec: rel parsed
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-relationships`. The test fails if the address relationship
edge is missing.

#### Relationships
- 9RYTD57365KZF8MX


---

### Procedure: export markdown
- Action: export markdown
- Spec: export ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-relationships`. The test fails if export output is missing
expected relationship lines.

#### Relationships
- W2Y09KSD3C131RHA


---

### Procedure: run markdown-relationships test
- Action: run markdown-rel test
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R markdown-relationships`. For controller wiring, use:
`build/unit-test-controller --test-regex markdown-relationships --section markdown_relationships_test --procedure run markdown-relationships test`

#### Relationships
- BZ52FDRB3CHFFWGA


---

## Section: report_generation_test

### Procedure: auto tables
- Action: render auto tables
- Spec: auto tables ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-generation`. The test fails if auto tables include NA or
blank rows.

#### Relationships
- 6E2D3TNN1HQW2GV5


---

### Procedure: template only
- Action: render template only
- Spec: injected fields ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-generation`. The test fails if template injection does not
work or tables appear.

#### Relationships
- BRW9373NFP39M64K


---

### Procedure: template + tables
- Action: render template + tables
- Spec: injected + tables ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-generation`. The test fails if injected fields are missing
or tables drop kept rows.

#### Relationships
- BY4T2SXM2PCY7090


---

### Procedure: repeat fields
- Action: repeat field tokens
- Spec: repeats ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-generation`. The test fails if repeated field tokens are not
rendered twice.

#### Relationships
- 2CRDDWNM3WPJ4ZTF


---

### Procedure: omit autotable
- Action: omit from tables
- Spec: omitted ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-generation`. The test fails if injected rows still appear in
AutoTables without _keep.

#### Relationships
- K93FPBPQMC559WNY


---

### Procedure: keep autotable
- Action: keep in tables
- Spec: kept ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-generation`. The test fails if kept rows disappear from
AutoTables.

#### Relationships
- RKPFEY8EK1JP2WGS


---

### Procedure: run report-generation test
- Action: run report-gen test
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-generation`. For controller wiring, use:
`build/unit-test-controller --test-regex report-generation --section report_generation_test --procedure run report-generation test`

#### Relationships
- V0RCM8TNV13EN01Q


---

## Section: http_api_test

### Procedure: create slugs
- Action: create slugs
- Spec: slugs created
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R http-api`. The test fails if the API does not create initial slugs.

#### Relationships
- E91Q0CQS4EHER5ZD


---

### Procedure: list and history
- Action: list and history
- Spec: list/history ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R http-api`. The test fails if list/history endpoints or auth checks
break.

#### Relationships
- MNE9ZA94JQKDTKWR


---

### Procedure: export import md
- Action: export/import md
- Spec: markdown ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R http-api`. The test fails if Markdown export/import fails.

#### Relationships
- ANJH7S9F8SF5WZ1W


---

### Procedure: workspace markdown
- Action: workspace markdown
- Spec: workspace md ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R http-api`. The test fails if workspace markdown list/import/export
fails.

#### Relationships
- ANMHHCCX352VRWAR


---

### Procedure: relationships api
- Action: relationships api
- Spec: rels ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R http-api`. The test fails if predicate or relationship APIs fail.

#### Relationships
- 6VDJDREYVNXZGC1D


---

### Procedure: entity and instance
- Action: entity/instance api
- Spec: entity + instance ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R http-api`. The test fails if entity/instance catalog APIs fail.

#### Relationships
- RD4G8P6XQ4FB7748


---

### Procedure: evaluation api
- Action: evaluation api
- Spec: evaluation ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R http-api`. The test fails if evaluation endpoints fail.

#### Relationships
- G2D05EPKGTZAWNDZ


---

### Procedure: report export
- Action: report export
- Spec: report written
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R http-api`. The test fails if report export does not create files.

#### Relationships
- 9W2SFSX1P6GNVHSG


---

### Procedure: run http-api test
- Action: run http-api test
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R http-api`. For controller wiring, use:
`build/unit-test-controller --test-regex http-api --section http_api_test --procedure run http-api test`

#### Relationships
- 6MQQJSATK2ZBVXRE


---

## Section: report_api_flow_test

### Procedure: create slugs
- Action: create slugs
- Spec: slugs created
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-api-flow`. The test fails if slug creation fails.

#### Relationships
- FT6V3AHPKEFQARWM


---

### Procedure: export report
- Action: export report
- Spec: report exported
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-api-flow`. The test fails if report export fails.

#### Relationships
- F0FXZ0BPMFW1W580


---

### Procedure: validate report
- Action: validate report
- Spec: report content ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-api-flow`. The test fails if report content checks fail.

#### Relationships
- AB2JZW61RJBKH9R9


---

### Procedure: run report-api-flow test
- Action: run report-api-flow
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R report-api-flow`. For controller wiring, use:
`build/unit-test-controller --test-regex report-api-flow --section report_api_flow_test --procedure run report-api-flow test`

#### Relationships
- WKFNC2VA2AMD6JBC


---

## Section: predicate_daemon_exhaustive_test

### Procedure: predicate matrix
- Action: predicate matrix
- Spec: matrix ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R predicate-daemon-exhaustive`. The test fails if any predicate matrix
case fails.

#### Relationships
- TJ12PGFGEY37PD7P


---

### Procedure: predicate matrix
- Action: predicate matrix
- Spec: matrix ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R predicate-daemon-exhaustive`. The test fails if any predicate matrix case fails. Artifacts: `.chax/test-artifacts/predicate-daemon-exhaustive/predicate_matrix.jsonl`.

#### Relationships
- T0SF4M6E9Q1HTSMF


---

### Procedure: csv predicate matrix
- Action: csv predicate matrix
- Spec: csv matrix ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R predicate-daemon-exhaustive-csv`. The test fails if any CSV-prefill slot permutation fails. Artifacts: `.chax/test-artifacts/predicate-daemon-exhaustive-csv/prefill_slot_matrix.jsonl`.

#### Relationships
- K9MGMXMCTCA9JFDE


---

### Procedure: multi-edge
- Action: multi-edge
- Spec: multi-edge ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R predicate-daemon-exhaustive`. The test fails if multi-edge
propagation does not work.

#### Relationships
- AK9622BETSHY953H


---

### Procedure: single-trigger
- Action: single-trigger
- Spec: single-trigger ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R predicate-daemon-exhaustive`. The test fails if only matching
predicates are not enforced.

#### Relationships
- BAG8YT6M1RBHRBQE


---

### Procedure: fanout
- Action: fanout
- Spec: fanout ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R predicate-daemon-exhaustive`. The test fails if fanout propagation
does not set all targets.

#### Relationships
- WM7JZQSYH88RHD80


---

### Procedure: cycle safety
- Action: cycle safety
- Spec: cycle safe
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R predicate-daemon-exhaustive`. The test fails if cycles overwrite the
initiating row.

#### Relationships
- 6JBDMB06K6ARXAF5


---

### Procedure: export import rels
- Action: export/import rels
- Spec: export ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R predicate-daemon-exhaustive`. The test fails if relationship export
or import loses edges.

#### Relationships
- YW4FZA2EP44NJ15E


---

### Procedure: run predicate-daemon test
- Action: run predicate-daemon
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R predicate-daemon-exhaustive`. For controller wiring, use:
`build/unit-test-controller --test-regex predicate-daemon-exhaustive --section predicate_daemon_exhaustive_test --procedure run predicate-daemon test`

#### Relationships
- EEZ1MCGW1Q9C2G7E


---

## Section: oauth_flow_test

### Procedure: missing state
- Action: missing state
- Spec: missing state blocked
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-flow`. The test fails if authorize allows missing state.

#### Relationships
- Z0GRYTEQHHT7AJPZ


---

### Procedure: bad redirect
- Action: bad redirect
- Spec: bad redirect blocked
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-flow`. The test fails if authorize allows disallowed redirects.

#### Relationships
- SH4KHKHN7W2580T1


---

### Procedure: single-use code
- Action: single-use code
- Spec: single-use enforced
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-flow`. The test fails if auth codes can be reused.

#### Relationships
- 1X8P458EBRMAFWRX


---

### Procedure: expired code
- Action: expired code
- Spec: expiry enforced
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-flow`. The test fails if expired codes still exchange.

#### Relationships
- X2SRE9H6AHQAQXNP


---

### Procedure: scope enforcement
- Action: scope enforcement
- Spec: scope enforcement ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-flow`. The test fails if read-only scopes allow writes.

#### Relationships
- 8WXRGHVDQKH9GV2B


---

### Procedure: full access
- Action: full access
- Spec: full access ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-flow`. The test fails if full scopes cannot read/write.

#### Relationships
- AVGRC2NWH6G9P3CK


---

### Procedure: run oauth-flow test
- Action: run oauth-flow test
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-flow`. For controller wiring, use:
`build/unit-test-controller --test-regex oauth-flow --section oauth_flow_test --procedure run oauth-flow test`

#### Relationships
- B3HXP9FMQW7RN3DM


---

## Section: oauth_authorize_ui_test

### Procedure: authorize page
- Action: authorize page
- Spec: login page ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-authorize-ui`. The test fails if the authorize page does not
render.

#### Relationships
- JA36DYKB463NYGCA


---

### Procedure: login
- Action: authorize login
- Spec: login ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-authorize-ui`. The test fails if login does not set a session
cookie.

#### Relationships
- 2BJWV4A88E33H0HD


---

### Procedure: approve redirect
- Action: approve redirect
- Spec: redirect ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-authorize-ui`. The test fails if approval does not redirect
with a code.

#### Relationships
- JB2D9TN4SYJERG0K


---

### Procedure: run oauth-authorize-ui test
- Action: run oauth-auth ui
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R oauth-authorize-ui`. For controller wiring, use:
`build/unit-test-controller --test-regex oauth-authorize-ui --section oauth_authorize_ui_test --procedure run oauth-authorize-ui test`

#### Relationships
- W29PHMFPFNSQNA4D


---

## Section: mcp_bridge_test

### Procedure: tool schemas
- Action: tool schemas
- Spec: schemas ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-bridge`. The test fails if tool schemas are missing or count
differs.

#### Relationships
- BVEAW9FKM6K3FK4E


---

### Procedure: hello and echo
- Action: hello and echo
- Spec: hello/echo ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-bridge`. The test fails if hello/echo tools fail.

#### Relationships
- 5YRGT1J1F2QZZ4QP


---

### Procedure: export list slugs
- Action: export/list slugs
- Spec: export/list ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-bridge`. The test fails if export/list slugs fail.

#### Relationships
- V4PEQCFPDPMMC8MK


---

### Procedure: get and update slug
- Action: get/update slug
- Spec: get/update ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-bridge`. The test fails if get/update tool calls fail.

#### Relationships
- B7QV18Q3P2DTHB4R


---

### Procedure: template relationships
- Action: template rels
- Spec: template rels ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-bridge`. The test fails if template relationship tools fail.

#### Relationships
- 22NR7C5GXS8NP5Q8


---

### Procedure: address relationships
- Action: address rels
- Spec: address rels ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-bridge`. The test fails if address relationship tools fail.

#### Relationships
- XZDVAQP74HZ4VRWB


---

### Procedure: evaluate graph
- Action: evaluate graph
- Spec: graph ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-bridge`. The test fails if evaluation tools fail.

#### Relationships
- MYXHB3Y432CXRMMH


---

### Procedure: entity and instance
- Action: entity/instance
- Spec: entity/instance ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-bridge`. The test fails if entity/instance tools fail.

#### Relationships
- 9HRNAPFV065BWVE7


---

### Procedure: markdown import export
- Action: markdown import/export
- Spec: markdown io ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-bridge`. The test fails if markdown import/export tools fail.

#### Relationships
- TWT7SXKRWH7Z79GW


---

### Procedure: run mcp-bridge test
- Action: run mcp-bridge test
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-bridge`. For controller wiring, use:
`build/unit-test-controller --test-regex mcp-bridge --section mcp_bridge_test --procedure run mcp-bridge test`

#### Relationships
- 5CY1F4VM8571RXQ2


---

## Section: mcp_voice_helper_test

### Procedure: onboarding prompt
- Action: onboarding prompt
- Spec: onboarding ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-voice-helper`. The test fails if onboarding prompt checks fail.

#### Relationships
- 86H513EFGCB629WG


---

### Procedure: tool calls prompt
- Action: tool calls prompt
- Spec: tool calls ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-voice-helper`. The test fails if tool call docs are missing.

#### Relationships
- ZYHBM9WYQ26ZBNXX


---

### Procedure: utterances prompt
- Action: utterances prompt
- Spec: utterances ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-voice-helper`. The test fails if utterances docs are missing.

#### Relationships
- XSB2QE8RXV7FMX6V


---

### Procedure: voice helper readme
- Action: voice helper readme
- Spec: readme ok
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-voice-helper`. The test fails if the voice assistant README
is missing key text.

#### Relationships
- 7VSF99D7WDJ93EMW


---

### Procedure: run mcp-voice test
- Action: run mcp-voice test
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R mcp-voice-helper`. For controller wiring, use:
`build/unit-test-controller --test-regex mcp-voice-helper --section mcp_voice_helper_test --procedure run mcp-voice test`

#### Relationships
- VFBSMZ1A21845AVK


---

## Section: e2e_smoke_test

### Procedure: run e2e-smoke test
- Action: run e2e-smoke test
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R e2e-smoke`. This requires PowerShell to be available.

#### Relationships
- Q1A8DAVQB3VVC5QV


---

## Section: server_start_stop_test

### Procedure: run server-start-stop test
- Action: run server-start-stop
- Spec: exit code == 0
- Result: 
- Status: 
- Comment: 

#### Instructions
Run `ctest --test-dir build -R server-start-stop`. This requires PowerShell to be available.

#### Relationships
- ZVVD8H0VS5CQJ50T
