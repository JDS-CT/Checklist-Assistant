# Checklist Assistant Compatibility Policy

Checklist Assistant compatibility is based on observable behavior, not private implementation details.

For the initial public release, a compatible implementation should preserve the canonical checklist folder shape, handle Markdown import/export identity fields, and treat `.chk` as a 7-Zip-format transport archive. `.7z` and `.zip` are accepted transport formats in the reference implementation.

Do not call a fork, asset pack, service, or implementation "official" or "certified" unless that status is granted separately by CVMEWT Inc. Prefer factual compatibility wording that names the version and tested behavior.
