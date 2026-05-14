# CHAX MCP Voice Assistant Helper

This package is a self-contained client helper for voice-first MCP hosts (Cursor, Claude Desktop, or custom GPT wrappers). It teaches an agent how to talk to the CHAX HTTP API via the MCP bridge without altering other clients.

## What this package includes
- Prompt pack for onboarding: see `prompts/onboarding.md` for the high-level system prompt and reply flows.
- Quick utterance cheatsheet for operators: see `prompts/utterances.md`.
- Sample MCP host guidance: see `prompts/tool_calls.md` for tool wiring notes and example arguments.

## Usage
1. Start the checklist server locally (or point to your deployment).
2. Launch the MCP bridge that mirrors the HTTP API: `cmake --build build --target chax-mcp-bridge` then `./build/chax-mcp-bridge.exe`.
3. In your MCP host, load the onboarding prompt and make the tool catalog available. The host should expose:
   - `base_url` (default `http://127.0.0.1:8080`)
   - MCP tools in the `chax.*` namespace (see `docs/mcp_tools.md`)
4. Drop `prompts/onboarding.md` into the host's system/instruction slot and keep `prompts/tool_calls.md` nearby so the agent knows how to form requests.

## Behavioral goals
- Voice agent guides the human through checklist execution: query slugs, present actions, capture Pass/Fail/NA with optional comments, then call `chax.update_slug`.
- Agent can browse relationships (`chax.relationships`, `chax.list_*_relationships`) to explain dependencies before prompting the user.
- Agent can read-only evaluate status (`chax.evaluate_slug` / `chax.evaluate_graph`) to summarize progress without mutating state.
- Agent keeps context modular: no assumptions about other clients; everything routes through the MCP tools.
- Agent targets a concrete instance: when listing slugs for updates, filter by `instance_id`
  or `instance_principal` to avoid updating template rows (`template||default`).

## Testing
- `tests/mcp_voice_helper_test.cpp` asserts the prompt pack stays present and references the expected tools/flows.
- Run `ctest --output-on-failure` to execute all MCP and HTTP API tests after changes.

## Notes
- Do not embed secrets in prompts or configs. Rely on host-provided environment variables for connectivity.
- Keep this helper isolated; adding or removing it must not change other clients.
