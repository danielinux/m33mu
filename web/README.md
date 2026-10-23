# m33mu Web Workbench

This first browser app is a visual-only H563 workbench shell around the real m33mu Emscripten worker. It loads the supplied board illustrations and layout coordinates, accepts H563 ELF32/ARM firmware up to 32 MiB, and provides bounded run, pause, one-instruction step, reset, CPU registers, virtual time, and GPIO output events. It saves and opens the v1 JSON project format, embedding the ELF and checking its SHA-256 on restore. The current editor supports one visual H563 board and an empty circuit; projects with contacts, nets, components, or instruments are rejected until those features can be preserved by the UI.

The board geometry and connector numbering are provisional. Contact selection is a visual locator only. Wiring, analog components, voltage probes, ADC, UART, and other CPU execution profiles are not implemented; the UI labels these capabilities as unavailable.

## Build and run

Install the pinned frontend dependencies with `npm install` from this directory. Then, from the repository root, run:

```sh
scripts/build-web-app.sh
npm --prefix web run dev
```

The app build compiles the Emscripten worker into `web/public/worker/` and builds the static app into `web/dist/`. The worker and board assets are served locally. No server-side emulator, CDN font, or network service is required at runtime.

The development toolchain requires Node.js/npm, CMake, and Emscripten. The worker currently runs H563 firmware only, through the production session bridge. Run slices are bounded to 512 scheduler work units and yield to browser messages, including during long WFI/WFE waits with a scheduled wake source.
