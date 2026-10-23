// McuPort (see web/sim/bench.ts) over the m33mu Emscripten ABI.

const STOP_ON_PAD_CHANGE = 1;

function split64(value) {
  const big = BigInt(Math.max(0, Math.floor(value)));
  return [Number((big >> 32n) & 0xffffffffn), Number(big & 0xffffffffn)];
}

export function createMcuPort(api, session, onError) {
  const nowNs = () => Number((BigInt(api.virtualTimeHigh(session) >>> 0) << 32n) |
    BigInt(api.virtualTimeLow(session) >>> 0));
  return {
    nowNs,
    runUntil(deadlineNs, instructionBudget, workBudget) {
      const [hi, lo] = split64(deadlineNs);
      const status = api.runUntil(session, hi, lo, instructionBudget, workBudget,
        STOP_ON_PAD_CHANGE);
      if (status !== 0) onError(status);
      return {
        deadlineReached: api.deadlineReached() !== 0,
        padChanged: api.padChanged() !== 0,
        stopped: api.stopped() !== 0,
        systemReset: api.systemReset() !== 0,
        instructions: api.instructionCount()
      };
    },
    popPadEvents() {
      const out = [];
      for (;;) {
        const status = api.gpioOutputNext(session);
        if (status === 0) break;
        if (status < 0) { onError(api.lastError()); break; }
        const config = api.gpioOutputConfig();
        out.push({
          socPad: api.gpioOutputPad(), level: api.gpioOutputLevel(),
          mode: config & 0xff, openDrain: ((config >> 8) & 0xff) !== 0,
          pull: (config >> 16) & 0xff,
          timeNs: Number((BigInt(api.gpioOutputTimeHigh() >>> 0) << 32n) |
            BigInt(api.gpioOutputTimeLow() >>> 0))
        });
      }
      return out;
    },
    padState(socPad) {
      const packed = api.padState(session, socPad);
      if (packed < 0) onError(api.lastError());
      return {
        level: packed & 0xff, mode: (packed >> 8) & 0xff,
        openDrain: ((packed >> 16) & 0xff) !== 0, pull: (packed >>> 24) & 0xff
      };
    },
    scheduleInput(socPad, level, timeNs) {
      const at = Math.max(Math.ceil(timeNs), nowNs());
      const [hi, lo] = split64(at);
      const status = api.gpio(session, socPad, hi, lo, level ? 1 : 0);
      if (status !== 0) onError(status);
    },
    cancelInputs(fromNs) {
      const [hi, lo] = split64(Math.ceil(fromNs));
      if (api.cancelInputs(session, '', hi, lo) < 0) onError(api.lastError());
    }
  };
}
