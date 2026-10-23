import createM33muModule from './m33mu_web_api.js';
import { attachM33muWorker } from './engine.mjs';

attachM33muWorker(self, createM33muModule);
