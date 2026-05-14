# MCP Tool Call Guide

Use MCP tools only. Do not call HTTP directly from the voice helper.

- `chax.list_slugs`: discover checklist rows. Include `checklist`, `instance_id`, or filters when known.
- `chax.get_slug`: fetch one row by `address_id`.
- `chax.update_slug`: update `status`, `result`, `comment`, or `timestamp` for one `address_id`.
- `chax.relationships`: inspect incoming and outgoing edges for one `address_id`.
- `chax.list_template_relationships`: inspect template-level relationships.
- `chax.list_address_relationships`: inspect instance/address-level relationships.
- `chax.evaluate_slug`: compute read-only status for one row.
- `chax.evaluate_graph`: compute read-only status for several root address IDs.
- `chax.create_entity`: create or update an operator/device principal.
- `chax.create_instance`: create or update an instance principal.

Response envelopes are shaped like `{ok, data, warnings, error}`. Surface warnings to the operator, and treat errors as a reason to pause or re-list the current checklist.
