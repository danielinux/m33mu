import createM33muModule from './m33mu_web_api.js';
import { attachM33muWorker } from './engine.mjs';

// Emulator start-up diagnostics are informational in the browser.
const quiet = (text) => console.debug(text);
attachM33muWorker(self, () => createM33muModule({ print: quiet, printErr: quiet }));
