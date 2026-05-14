# TODO

## p1 Public Seed Readiness

- [p1][T-PUB-0001] Review the first public seed diff before the initial commit; confirm only source, public docs, generic examples, and third-party source dependencies are staged.
- [p1][T-PUB-0002] Run a clean configure/build/test pass from this repository after the first seed, preferably with `cmake --preset windows-gcc-ninja`, `cmake --build --preset windows-gcc-ninja`, and `ctest --preset windows-gcc-ninja --output-on-failure`.
- [p1][T-PUB-0003] Confirm no private checklist packs, customer/vendor artifacts, release binaries, local databases, logs, or Whisper/VUI assets are present in the initial public commit.

## p2 Public Release Follow-Up

- [p2][T-PUB-0100] Add public conformance tests for the specification once the first external compatibility policy settles.
- [p2][T-PUB-0101] Add richer generic `.chk` downloadable examples after reviewing each pack for public-safe content.
- [p2][T-PUB-0102] Decide whether the optional VUI/Whisper client should live in this repository later or in a companion repository.

## p3 Security And Hosting

- [p3][T-PUB-0200] Hosted/LAN security remains a follow-up. Document and harden deployment boundaries before recommending internet-facing or shared-LAN operation; the first public release is intended for local personal/workstation use.

## p4 Project Hygiene

- [p4][T-PUB-0300] Expand public contributor guidance as outside contributions arrive.
- [p4][T-PUB-0301] Add issue templates after the first real feedback cycle.
