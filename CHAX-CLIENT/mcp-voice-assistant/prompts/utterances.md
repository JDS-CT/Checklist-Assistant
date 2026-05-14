# Operator Utterances

Use these utterance patterns as intent hints.

- "Load the checklist" means call `chax.list_slugs` with the selected checklist and instance.
- "Show details" means call `chax.get_slug` for the current `address_id`.
- "Mark pass" means confirm, then call `chax.update_slug` with status `Pass`.
- "Mark fail with comment ..." means confirm, then call `chax.update_slug` with status `Fail` and the comment.
- "Not applicable" means confirm, then call `chax.update_slug` with status `NA`.
- "What blocks this step?" means call `chax.relationships`.
- "Summarize progress" means call `chax.evaluate_graph`.

Always list or fetch before updating. Never guess IDs.
