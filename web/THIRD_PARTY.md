# Web workbench third-party notices

This inventory covers material currently present in `web/`. It is updated when dependencies, solver code, models, or artwork are added.

| Material | License / provenance | Included here | Notes |
|---|---|---:|---|
| m33mu emulator source | AGPL-3.0-or-later; repository `LICENSE` | No copied source in this directory | A future WASM build will compile repository code and must ship the applicable license and source offer. |
| Board sprite pack | CC0-1.0, declared by `m33mu_board_sprites.zip` manifest; generated original artwork and layout geometry | No artwork copied into `web/` yet | Manufacturer names and marks remain their owners' trademarks. Preserve attribution/provenance metadata if the assets are imported. |
| ST STM32CubeH5 pin evidence | STMicroelectronics source at pinned commit `bac48badf93cb496f6445afba956082490d3f543` | Facts and URL only; no source text or code copied | The cited README supports CN7/10→PA5 and CN9/1→PA6. Review the upstream license before copying code or documentation. |
| EEcircuit `eecircuit-engine` (`eelab-dev/EEcircuit-engine`) | MIT, upstream repository | No code included | Public API runs a complete netlist to completion and exports `out.raw`; not suitable as-is for interactive live co-simulation. Its compiled ngspice WASM and bundled models require separate license/provenance review before reuse. |
| CircuitJS1 (`pfalstad/circuitjs1`) | GPL-2.0-or-later, upstream repository | No code included | `CircuitMatrix` looks computationally separable, but `CirSim` imports substantial UI/model infrastructure. A kernel extraction and GPL/AGPL combination review remain required. |
| ngspice, Velxio, Breadboard | Evaluation candidates only | No code, models, or artwork included | No solver has been selected. Before importing anything, record exact upstream revision, file-level license, model provenance, notices, and compatibility with the project's AGPL distribution. |

No npm package manifest or third-party runtime dependency has been added yet. SPDX identifiers above describe declared provenance; they are not a substitute for checking the license text and distribution obligations at the time of adoption.
