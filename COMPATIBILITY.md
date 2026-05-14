# Compatibility Policy

The public compatibility policy is intentionally early. The reference implementation, public specification, and tests in this repository are the current source of truth.

## Current Guidance

- `.chk` files are 7-Zip-format archives with a Checklist Assistant extension.
- `.7z` and `.zip` asset-pack archives are also accepted by the reference implementation.
- A valid checklist asset pack should preserve the `checklists/<pack>/<checklist>/checklist.md` folder shape.
- Compatibility claims should name the tested version, feature area, and any unsupported behavior.

## Not Yet Certification

This repository does not yet provide a complete conformance suite or certification program. A future public conformance suite is tracked in `TODO.md`.
