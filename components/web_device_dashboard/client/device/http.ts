// Shared HTTP core of the device API client (device / network / entity-settings).
//
// web_device_dashboard uses HTTP status codes semantically (400/404 on bad requests, 405
// on the wrong method, 413 on an oversized body), so — unlike the plain-Error automation
// SDK — this SDK's error carries the status. That status contract belongs with
// the backend, so ApiError is owned here and re-exported by consumers.

/** Error thrown by every device API call on a non-2xx response. */
export class ApiError extends Error {
  constructor(message: string, public readonly status: number) {
    super(message)
    this.name = 'ApiError'
  }
}

/** Injectable fetch — pass one that adds a timeout / auth / logging. Defaults to window.fetch. */
export type FetchImpl = (url: string, init?: RequestInit) => Promise<Response>

export interface HttpOptions {
  /** Base URL for the API, e.g. `${basePath}/api/device` (no trailing slash). */
  base: string
  /** Fetch implementation. Defaults to `window.fetch`. */
  fetchImpl?: FetchImpl
}

export interface HttpClient {
  jget<T>(path: string): Promise<T>
  jpost<T>(path: string, body?: unknown): Promise<T>
}

export function createHttp(options: HttpOptions): HttpClient {
  const base = options.base
  const doFetch: FetchImpl = options.fetchImpl ?? ((url, init) => fetch(url, init))

  // Surface the backend's {error} message when present, else the status.
  async function toError(res: Response): Promise<ApiError> {
    let msg = `Request failed: ${res.status}`
    try {
      const data: unknown = await res.clone().json()
      if (data && typeof data === 'object' && 'error' in data) {
        const e = (data as { error?: unknown }).error
        if (typeof e === 'string' && e) msg = e
      }
    } catch {
      /* non-JSON error body */
    }
    return new ApiError(msg, res.status)
  }

  return {
    async jget<T>(path: string): Promise<T> {
      const res = await doFetch(`${base}${path}`)
      if (!res.ok) throw await toError(res)
      return (await res.json()) as T
    },
    async jpost<T>(path: string, body?: unknown): Promise<T> {
      const init: RequestInit = { method: 'POST' }
      if (body !== undefined) {
        init.headers = { 'Content-Type': 'application/json' }
        init.body = JSON.stringify(body)
      }
      const res = await doFetch(`${base}${path}`, init)
      if (!res.ok) throw await toError(res)
      const text = await res.text()
      return (text ? JSON.parse(text) : undefined) as T
    }
  }
}
