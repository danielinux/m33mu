// Typed client for the v1 worker envelope.

export type WorkerEvent = { event: string; sequence: number; payload: any };

type Pending = { resolve: (value: any) => void; reject: (error: WorkerRequestError) => void };

export class WorkerRequestError extends Error {
  code: string;
  constructor(code: string, message: string) {
    super(message);
    this.code = code;
  }
}

export class WorkerClient {
  private worker: Worker;
  private pending = new Map<number, Pending>();
  private nextId = 1;
  private listeners = new Set<(e: WorkerEvent) => void>();
  private failed: ((message: string) => void) | null = null;

  constructor(url: URL) {
    this.worker = new Worker(url, { type: 'module' });
    this.worker.onmessage = ({ data }) => {
      if (data?.kind === 'event') {
        for (const l of this.listeners) l(data as WorkerEvent);
        return;
      }
      if (data?.kind !== 'response') return;
      const p = this.pending.get(data.requestId);
      if (!p) return;
      this.pending.delete(data.requestId);
      if (data.ok) p.resolve(data.result);
      else p.reject(new WorkerRequestError(data.error?.code ?? 'ERROR', data.error?.message ?? 'Request failed'));
    };
    this.worker.onerror = (e) => {
      const message = e.message || 'Worker failed to start';
      for (const p of this.pending.values()) p.reject(new WorkerRequestError('WORKER_ERROR', message));
      this.pending.clear();
      this.failed?.(message);
    };
  }

  onFailure(handler: (message: string) => void): void {
    this.failed = handler;
  }

  subscribe(listener: (e: WorkerEvent) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  request<T = any>(operation: string, payload: Record<string, unknown> = {}, transfer: Transferable[] = []): Promise<T> {
    const requestId = this.nextId++;
    const response = new Promise<T>((resolve, reject) => this.pending.set(requestId, { resolve, reject }));
    this.worker.postMessage({ schemaVersion: 1, kind: 'request', requestId, operation, payload }, transfer);
    return response;
  }

  terminate(): void {
    this.worker.terminate();
    for (const p of this.pending.values()) p.reject(new WorkerRequestError('CLOSED', 'Workbench closed'));
    this.pending.clear();
  }
}
