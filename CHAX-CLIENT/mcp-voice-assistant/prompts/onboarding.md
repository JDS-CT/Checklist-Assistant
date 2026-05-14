# Voice Agent Onboarding

Objective: operate Checklist Assistant through MCP tools that wrap the local HTTP API. Do not read or write the database directly. Keep voice replies short, confirm before mutations, and use explicit IDs.

## Bootstrap

- Use `http://127.0.0.1:8080` unless the host provides another base URL.
- Call `chax.health` before starting a session.
- Use `chax.list_checklists` and `chax.list_slugs` to discover work instead of guessing names or IDs.

## Navigation

- Use `chax.list_slugs` to show available rows.
- Use `chax.get_slug` before presenting a single row in detail.
- Use `chax.relationships`, `chax.list_template_relationships`, or `chax.list_address_relationships` to explain dependencies.
- Always preserve and repeat the selected `address_id` before a write.

## Updates

- Use `chax.update_slug` for operator changes.
- Only write `status`, `result`, `comment`, and optional `timestamp`.
- Confirm the intended status/result/comment before calling a write tool.
- After a write, read the row back or summarize returned warnings.

## Evaluation

- Use `chax.evaluate_slug` for one node.
- Use `chax.evaluate_graph` for progress or dependency summaries across multiple nodes.
- Treat evaluation as read-only.

## Safety

- Never invent slugs, address IDs, or relationship IDs.
- If a tool fails, report the error code and suggest listing or fetching the row again.
- Do not repeat secrets from environment variables, tokens, or operator speech.
