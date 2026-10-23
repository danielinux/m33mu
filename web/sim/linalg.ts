// Dense LU solve with partial pivoting. Workbench circuits are small (tens of
// unknowns), so a dense factorization is simpler and fast enough.

export class DenseSystem {
  readonly size: number;
  readonly a: Float64Array;
  readonly b: Float64Array;
  private readonly lu: Float64Array;
  private readonly perm: Int32Array;

  constructor(size: number) {
    this.size = size;
    this.a = new Float64Array(size * size);
    this.b = new Float64Array(size);
    this.lu = new Float64Array(size * size);
    this.perm = new Int32Array(size);
  }

  clear(): void {
    this.a.fill(0);
    this.b.fill(0);
  }

  add(row: number, col: number, value: number): void {
    if (row < 0 || col < 0) return;
    this.a[row * this.size + col] += value;
  }

  addRhs(row: number, value: number): void {
    if (row < 0) return;
    this.b[row] += value;
  }

  /** Solves A x = b into x. Returns the first singular pivot row, or -1. */
  solve(x: Float64Array): number {
    const n = this.size;
    const lu = this.lu;
    const perm = this.perm;
    lu.set(this.a);
    for (let i = 0; i < n; i++) perm[i] = i;
    for (let k = 0; k < n; k++) {
      let pivot = k;
      let best = Math.abs(lu[k * n + k]);
      for (let i = k + 1; i < n; i++) {
        const v = Math.abs(lu[i * n + k]);
        if (v > best) { best = v; pivot = i; }
      }
      if (!(best > 1e-300)) return perm[k];
      if (pivot !== k) {
        for (let j = 0; j < n; j++) {
          const t = lu[k * n + j];
          lu[k * n + j] = lu[pivot * n + j];
          lu[pivot * n + j] = t;
        }
        const t = perm[k]; perm[k] = perm[pivot]; perm[pivot] = t;
      }
      const d = lu[k * n + k];
      for (let i = k + 1; i < n; i++) {
        const f = lu[i * n + k] / d;
        if (f === 0) continue;
        lu[i * n + k] = f;
        for (let j = k + 1; j < n; j++) lu[i * n + j] -= f * lu[k * n + j];
      }
    }
    for (let i = 0; i < n; i++) {
      let sum = this.b[perm[i]];
      for (let j = 0; j < i; j++) sum -= lu[i * n + j] * x[j];
      x[i] = sum;
    }
    for (let i = n - 1; i >= 0; i--) {
      let sum = x[i];
      for (let j = i + 1; j < n; j++) sum -= lu[i * n + j] * x[j];
      x[i] = sum / lu[i * n + i];
    }
    for (let i = 0; i < n; i++) {
      if (!Number.isFinite(x[i])) return i;
    }
    return -1;
  }
}
