# MCP Tools

This file tracks every Model Context Protocol tool exposed by the repository. The native
`chax-mcp-bridge` binary proxies requests to the local `checklist_assistant_server` HTTP API so agent hosts can
interact with the backend using the MCP transport.

## Tool catalog

| Tool name                          | Method/Path                                  | Description                                                                    | Arguments                                                                                              |
| ---------------------------------- | -------------------------------------------- | ------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------ |
| `chax.list_commands`               | `GET /api/v1/commands`                       | Returns the canonical list of HTTP commands.                                   | _none_                                                                                                 |
| `chax.health`                      | `GET /api/v1/health`                         | Reports readiness, uptime, and server version metadata.                        | _none_                                                                                                 |
| `chax.hello`                       | `GET /api/v1/hello`                          | Sends a greeting to the provided `name` (defaults to `world`).                 | `name` _(string, optional)_                                                                            |
| `chax.echo`                        | `POST /api/v1/echo`                          | Echoes the JSON payload that agents supply for smoke testing.                  | `payload` _(string, required)_                                                                         |
| `chax.list_checklists`             | `GET /api/v1/checklists`                     | Lists all checklist names currently in the SQLite runtime store.               | _none_                                                                                                 |
| `chax.list_slugs`                  | `GET /api/v1/slugs`                          | Lists slugs with filters and cursor-based pagination.                          | `checklist`, `section`, `status`, `limit`, `cursor` _(optional)_                                       |
| `chax.get_slug`                    | `GET /api/v1/slugs/{address_id}`             | Fetches a single slug by Address ID.                                           | `address_id` _(string, required)_                                                                      |
| `chax.get_checklist`               | `GET /api/v1/checklists/{checklist}`         | Fetches every slug for the named checklist.                                    | `checklist` _(string, required)_                                                                       |
| `chax.relationships`               | `GET /api/v1/relationships/address/{id}`     | Returns incoming/outgoing edges for the supplied Address ID.                   | `address_id` _(string, required)_                                                                      |
| `chax.list_template_relationships` | `GET /api/v1/relationships/template`         | Lists template-level relationships with optional filters and cursor.           | `subject_slug_id`, `target_slug_id`, `predicate`, `limit`, `cursor` _(optional)_                       |
| `chax.create_template_relationship`| `POST /api/v1/relationships/template`        | Creates a template-level relationship triple.                                  | `subject_slug_id`, `predicate`, `target_slug_id` _(all strings, required)_                             |
| `chax.list_address_relationships`  | `GET /api/v1/relationships/address`          | Lists address-level relationships with optional filters and cursor.            | `subject_address_id`, `target_address_id`, `predicate`, `limit`, `cursor` _(optional)_                 |
| `chax.create_address_relationship` | `POST /api/v1/relationships/address`         | Creates an address-level relationship triple.                                  | `subject_address_id`, `predicate`, `target_address_id` _(all strings, required)_                       |
| `chax.create_entity`               | `POST /api/v1/entities`                      | Upserts an entity principal into the catalog.                                  | `principal` _(required)_, `kind`, `display_name`                                                       |
| `chax.list_entities`               | `GET /api/v1/entities`                       | Lists entity catalog entries.                                                  | `limit`, `cursor` _(optional)_                                                                         |
| `chax.create_instance`             | `POST /api/v1/instances`                     | Upserts an instance principal into the catalog.                                | `principal` _(required)_, `label`, `meta`                                                              |
| `chax.list_instances`              | `GET /api/v1/instances`                      | Lists instance catalog entries.                                                | `limit`, `cursor` _(optional)_                                                                         |
| `chax.evaluate_slug`               | `GET /api/v1/evaluate/slug/{address_id}`     | Read-only evaluation of a single slug.                                         | `address_id` _(string, required)_                                                                      |
| `chax.evaluate_graph`              | `POST /api/v1/evaluate/graph`                | Read-only evaluation of a set of address IDs.                                  | `root_address_ids` _(array of strings, required)_                                                      |
| `chax.update_slug`                 | `PATCH /api/v1/slugs/{address_id}`           | Applies the minimal update contract (result/status/comment).                   | `address_id` _(string, required)_, `status`, `result`, `comment`, `timestamp` _(strings)_              |
| `chax.export_json`                 | `GET /api/v1/export/json`                    | Exports all slugs as a JSON array.                                             | _none_                                                                                                 |
| `chax.export_markdown`             | `GET /api/v1/export/markdown/{checklist}`    | Exports a checklist as canonical Markdown for authors.                         | `checklist` _(string, required)_                                                                       |
| `chax.import_markdown`             | `POST /api/v1/import/markdown`               | Ingests canonical Markdown for a checklist; supports `instance_principal`.     | `checklist` _(string, required)_, `markdown` _(string, required)_, `instance_principal` _(optional)_   |

## Usage

1. Build and launch the MCP bridge:

   ```powershell
   cmake --build build --target chax-mcp-bridge
   .\build\chax-mcp-bridge.exe
   ```

2. Ensure the C++ server is running locally (default `127.0.0.1:8080`). The MCP bridge reads the
   same `CHAX_HOST` and `CHAX_PORT` environment variables; you can also override everything
   with `CHAX_MCP_BASE_URL`.
3. Point your MCP host (Cursor, Claude Desktop, etc.) at the running bridge. The host can now call
   the tools above and receive the CHAX v1 response envelope `{ok,data,warnings}`.

## Instance targeting (avoid template updates)

- Template rows live under the root/template instance (principal `template||default`) and are
  treated as the read-only source of truth. Do not PATCH those rows.
- Always filter list calls by `instance_id` or `instance_principal` when preparing to update
  slugs so you do not accidentally operate on template rows.
- If only the template exists, create or select a real instance first (for MCP: call
  `chax.import_markdown` with an `instance_principal` to seed instance slugs; for direct HTTP
  clients: `POST /api/v1/slugs` with `instance_principal` or import the Markdown).

## Relationship authoring context

Relationship triples are authored behavior, not an invitation to invent a second lookup engine. A common composition is: an upstream update writes a selector to a target row; the target row then becomes the source of a self-prefill relationship and resolves its own checklist-local CSV. The normal server default permits those two hops. Use `CHAX_PREDICATE_CHAIN_DEPTH` only for an intentionally longer finite chain.

Before proposing a new runtime feature for a prefill path, inspect the active instance's rows and relationships, the checklist-local `data/` assets, and the Relationship Workbench. Prefer a small authoring change that uses the existing predicate chain when the target can own its lookup table. Test the exact input against a freshly imported, explicitly named instance; do not PATCH template rows or edit SQLite directly.

## Testing

`mcp-bridge-test` (invoked via `ctest`) spins up a lightweight HTTP stub and verifies the MCP bridge
implements `tools/list`, `chax.hello`, `chax.echo`, and Markdown import/export end-to-end.

```powershell
ctest --output-on-failure
```
