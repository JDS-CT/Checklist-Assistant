# CHAX Web UI (Checklist Assistant)

## OAuth PKCE Login (Web)

1) Configure env on the server: `OAUTH_CLIENT_ID`, `OAUTH_CLIENT_SECRET`, `BASE_URL`, `ADMIN_USER`, `ADMIN_PASSWORD` (for local dev auth), and ensure the redirect `http://<host>:<port>/CHAX-CLIENT/web/oauth_callback.html` is allow‑listed.
2) Open `CHAX-CLIENT/web/checklist_assistant.html` in the browser. Set host/port fields to your server (defaults to `127.0.0.1:8080`).
3) Click **Login with OAuth**. The UI generates PKCE (S256), redirects to `/oauth/authorize`, and returns to `/CHAX-CLIENT/web/oauth_callback.html` to exchange the code for an access token (no client secret stored in the browser).
4) After redirect, the session panel shows token status, scopes, entity_id, and (optionally) principal. All API calls use the bearer token automatically.
5) Use **Logout** to clear the in-memory/session token. Dev-only entity simulation remains available behind `window.CHAX_CONFIG.devMode = true` for testing `/api/v1/entities`.

Optional UI config (set in `checklist_assistant.html`):
- `window.CHAX_CONFIG.autoRefreshMs`: polling interval in milliseconds (set to `0` to disable).
- `window.CHAX_CONFIG.verifyAutoRefreshMs`: minimum interval for auto-refresh Verify graph checks (set to `0` to disable auto Verify refresh).
- `window.CHAX_CONFIG.autoRefreshPauseAfterInteractionMs`: pauses polling briefly after user scroll/interaction to keep large checklist browsing responsive.
- `window.CHAX_CONFIG.largeChecklistWarnThreshold`: row-count threshold where the instance indicator switches to a warning state (`⚠`) with a large-checklist advisory tooltip.
- `window.CHAX_CONFIG.deferVerifyRenderThreshold`: row-count threshold to defer Verify indicator refresh until after initial table render.
- `window.CHAX_CONFIG.instanceSeedConcurrency`: parallelism for seeding missing instance rows from template rows during instance creation/loading.
