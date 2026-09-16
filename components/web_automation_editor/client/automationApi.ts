// TypeScript client for the web_automation_editor HTTP API.
//
// Lives with the backend component that owns the <url_prefix>/api/* routes, so
// the client and the C++ contract stay together; a frontend injects only the
// base URL and its fetch wrapper through createAutomationApi().
//
// Backend behaviour (verified against web_automation_editor.cpp):
//  - save, delete and reboot answer POST only, the rest GET only; any other
//    method is 405 with an Allow header;
//  - get/save/delete return HTTP 400/404 with a {success:false,error} body, a
//    save body over 16 KiB 413;
//  - /ping returns {status:"ok"} (NOT the {success:true} envelope).
import type {
  AutomationListResponse,
  AutomationConfig,
  AutomationSaveResponse,
  AutomationEntitiesResponse,
  AutomationSchema,
  AutomationExport
} from './types'

export type * from './types'

/** Injectable fetch — pass one that adds a timeout / auth / logging. Defaults to window.fetch. */
export type FetchImpl = (url: string, init?: RequestInit) => Promise<Response>

export interface AutomationApiOptions {
  /** Base URL for the automation API, e.g. `${basePath}/automation-editor/api` (no trailing slash). */
  base: string
  /** Fetch implementation. Defaults to `window.fetch`. */
  fetchImpl?: FetchImpl
}

export interface AutomationApi {
  /** GET /list — all automations (summary metadata). */
  list(): Promise<AutomationListResponse>
  /** GET /get?id= — full config for editing. */
  get(id: number): Promise<AutomationConfig>
  /** POST /save — create (id 0/absent) or update (id>0). JSON body. */
  save(config: AutomationConfig): Promise<AutomationSaveResponse>
  /** POST /delete?id= — delete an automation (no body). */
  remove(id: number): Promise<AutomationSaveResponse>
  /** GET /entities — grouped object_id/name for editor dropdowns. */
  entities(): Promise<AutomationEntitiesResponse>
  /** GET /schema — static catalog of trigger/condition/action types + cron presets. */
  schema(): Promise<AutomationSchema>
  /** GET /export — every automation as a full config (same shape as /get), for a backup file. */
  exportAll(): Promise<AutomationExport>
  /** POST /reboot — reboot the device (no confirm token). */
  reboot(): Promise<{ success: boolean; message?: string }>
  /** GET /ping — liveness; returns { status:"ok" }. */
  ping(): Promise<{ status: string }>
}

export function createAutomationApi(options: AutomationApiOptions): AutomationApi {
  const base = options.base
  const doFetch: FetchImpl = options.fetchImpl ?? ((url, init) => fetch(url, init))

  // Surface the backend's {success:false,error} message when present, else status.
  async function toError(res: Response): Promise<Error> {
    let msg = `Request failed: ${res.status}`
    try {
      const data: unknown = await res.json()
      if (data && typeof data === 'object' && 'error' in data) {
        const e = (data as { error?: unknown }).error
        if (typeof e === 'string' && e) msg = e
      }
    } catch {
      /* non-JSON error body */
    }
    return new Error(msg)
  }

  async function jget<T>(path: string): Promise<T> {
    const res = await doFetch(`${base}${path}`)
    if (!res.ok) throw await toError(res)
    return (await res.json()) as T
  }

  async function jpost<T>(path: string, body?: unknown): Promise<T> {
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

  return {
    list() {
      return jget<AutomationListResponse>('/list')
    },
    get(id) {
      return jget<AutomationConfig>(`/get?id=${id}`)
    },
    save(config) {
      return jpost<AutomationSaveResponse>('/save', config)
    },
    remove(id) {
      return jpost<AutomationSaveResponse>(`/delete?id=${id}`)
    },
    entities() {
      return jget<AutomationEntitiesResponse>('/entities')
    },
    schema() {
      return jget<AutomationSchema>('/schema')
    },
    exportAll() {
      return jget<AutomationExport>('/export')
    },
    reboot() {
      return jpost<{ success: boolean; message?: string }>('/reboot')
    },
    ping() {
      return jget<{ status: string }>('/ping')
    }
  }
}
