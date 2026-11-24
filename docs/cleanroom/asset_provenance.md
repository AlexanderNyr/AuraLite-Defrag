# Clean-Room Asset Provenance Register (R-002, DOC-01)

Every visual asset in this product was authored for it. There are three, and all
three come from the same generator: geometric primitives drawn as SVG and
rasterised locally. No stock imagery, no icon pack, no traced silhouette, no
asset of unknown origin.

The machine-checkable half of this register lives in
`res/assets/asset_register.json`; `tools/check_licences.py` verifies the SHA-256
of every registered file, and fails if a file exists in `res/assets/` that is not
registered, or is registered but missing.

## Registered assets

| Asset | Purpose | Authoring method | Licence | SHA-256 (prefix) |
|---|---|---|---|---|
| `res/assets/auralite_logo.svg` | Project vector emblem | Original concentric cluster-grid, hand-written SVG | Apache-2.0 | `babbe6dd` |
| `res/assets/auralite_32.png` | 32×32 application icon | Rasterised from the same geometry | Apache-2.0 | `bf7bb9c3` |
| `res/assets/auralite.ico` | Windows PE resource icon | Multi-resolution ICO built from the same geometry | Apache-2.0 | `4f6c7ef6` |

## Why a hash register and not a promise

An asset register that is not verified is a statement of intent. The register is
checked in both directions — a file added without an entry fails the build, and
an entry whose hash no longer matches fails the build — because the failure mode
being guarded against is an image arriving in the repository without anyone
deciding where it came from.

## Rules applied

1. **No third-party visual material**, at all. This includes icons that ship
   with a permissive licence: "free to use" is not the same as "provenance
   recorded", and the register cannot record provenance it does not have.
2. **Colour, layout and iconography are our own.** The product must not be
   mistakable for another defragmenter at a glance; that is the trade-dress
   requirement, and it is the reason the ADR log records where familiarity was
   deliberately *not* pursued.
3. **Fonts are system fonts only.** No bundled font files, so no font licence to
   audit and no redistribution question.
4. **Documentation images are ours.** P9-T07 requires at least one screenshot of
   our own UI; the criterion forbids using anyone else's screenshot to satisfy
   it. See the honest note in the user guide about the screenshot being
   unavailable until the GUI builds on Windows.

## Verification

```
make audit            # hashes, both directions, plus undeclared includes
make audit-negative   # proves the check fails when it should
```
