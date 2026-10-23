# m33mu board sprite pack — original provisional illustrations

15 board assets matching all 15 `--cpu` profiles in m33mu `master` on 2026-10-23.

## Contents
- `svg/<cpu>.svg`: transparent vector sprite, with pin holes drawn and **no pin labels**.
- `png/<cpu>.png`: high-resolution transparent PNG raster preview.
- `layout/<cpu>.json`: original pixel `viewBox`, connector IDs, physical contact centers, sequence numbers, board source, verification flags, and (only for Pico 2) official electrical pin names.
- `manifest.json`: pack summary and validation policy.
- `contact-sheet.png`: visual overview.

## Safety and fidelity
These are **original vector approximations**, not exact manufacturer Gerbers, CAD layouts or photos. The placement of general headers and holes is provisional and may differ from actual board revisions, especially dense connectors. **Only the Raspberry Pi Pico 2 40-edge-pad physical signal names were checked against the official pinout**. All other electrical signals are intentionally `null` (unknown), so pin-to-SoC mapping MUST be verified before enabling electrical wiring. Header IDs and sequence numbers on provisional boards are locator handles, not asserted vendor pin numbering. See m33mu_webui_plan.md §6 for provenance and Codex requirements. H5F4 and U585 are explicitly labelled **white Nucleo-style proxies**, not fake commercial STM32 Nucleo SKUs. The UI must never infer electrical signal from sprite art.

Pin-less visual wire endpoints can be represented by `connectors[].contacts[].x/y` and resolved by `board instance + connector ID + contact number`, with signal mapping added as separate audited data. On the physical artwork there are **no per-pin text identifiers**, ensuring future relabeling/pinmux overlays remain accessible and independent of the bitmap.

## Replacement by final user-supplied board images
The project owner will provide board images. Replace `svg/<cpu>.svg` or `png/<cpu>.png`, update the `pixelViewbox` and recapture contact centers while keeping connector/pad identities stable. Require a board-specific visual geometry review and a separate schematic electrical mapping review.

Artwork was authored from geometric primitives for this deliverable. Board/trademark names identify the intended hardware only. Original illustration and original layout coordinates are dedicated to CC0-1.0; manufacturer logos and trademarks are not licensed by that dedication.

Reference starting points are available in per-board `manufacturerSource` fields. For Pico 2: https://datasheets.raspberrypi.com/pico/Pico-2-Pinout.pdf
