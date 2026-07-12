# Design Principles Adoption

`PRINCIPLES.md` is the short, operational source of truth. This record explains its application.

## Interoperability and Longevity

For low-impact choices, prefer documented, openly implementable, durable formats and conventions. Do not weaken a general quality check merely because legacy data happens to pass the application parser. Preserve an exception only when it has an identified semantic purpose, a narrow scope, and an explicit owner.

For CHAX Markdown, parser behavior trims field values. Therefore empty `Result`, `Status`, and `Comment` fields do not require trailing whitespace. Those field lines should use the standard no-trailing-whitespace form. Any remaining Markdown trailing spaces must be reviewed as possible explicit hard breaks rather than suppressed wholesale.

## Local Maxima and Alternatives

An implementation may appropriately optimize a local workflow. It must not hide that a standard or scalable alternative exists. Plans and decision records should state the selected path, the viable alternative, why the current choice fits now, and the data/export boundary that keeps a later transition possible.

## Material Refactor Gate

The default review threshold is 10% of tracked production files. The gate also applies before changing a public data contract or a migration boundary even when the file count is small. The proposing work must describe the current state, the proposed state, expected rewrite/removal cost, compatibility/migration effect, and a lower-impact alternative. This is a review gate, not a prohibition on substantial improvements.
