# Checklist Asset Packs

This folder is the shared drop point for authoring and exchanging checklist asset packs without touching the runtime database.

- Each pack lives under `checklists/<pack>/` and contains checklist folders (`checklist.md` plus optional `data`, `templates`, `img`, `reports`, `saves`, `scripts`).
- Use `scripts/checklist_workspace.ps1` to export the current server state into a pack or restore a pack into the server.
- Packs intended for version control should stay under explicit public pack folders such as `checklists/chax`, `checklists/examples`, and `checklists/unit-tests`; private packs should live outside this repository or remain untracked.
- For new checklist authoring, start with `docs/user_manual.md` and `checklists/examples/checklist_authoring/checklist.md`, then round-trip the draft through the running server so `GET /api/v1/export/markdown/<checklist>` can write back real `slug_id` source lines under `#### Relationships`.
