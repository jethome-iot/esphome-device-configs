// TypeScript client for the web_climate_editor HTTP API.
//
// Lives with the backend component that owns the <url_prefix>/api/* routes, so
// the client and the C++ contract stay together; a frontend injects only the
// base URL and its fetch wrapper through createClimateApi().
//
// Backend behaviour (web_climate_editor.cpp follows ./types.ts):
//  - save, delete, enable and setpoint answer POST only, the rest GET only; a GET
//    or POST to the wrong route, or an OPTIONS, is 405 with an Allow header, and
//    an unknown route is 404. The server answers PUT, DELETE, HEAD and PATCH with
//    its own text 405 and closes the connection; this client sends none of them;
//  - failures are {success:false,error} with 400/404/405/409/413/500/503/507;
//  - GET /get?id= returns the bare document, not the success envelope;
//  - /ping returns {status:"ok"} (NOT the success envelope);
//  - every read and write runs on the device's main loop, so an answer describes
//    what actually happened; 503 means the loop did not get to it and nothing changed.
import type {
  ClimateEntitiesResponse,
  ClimateSchema,
  ControllerDocument,
  ControllerDraft,
  ControllerSaveInput,
  ControllersResponse,
  EnableResponse,
  SaveResponse,
  StatusResponse,
  SuccessResponse
} from './types'

export type * from './types'

/** Injectable fetch — pass one that adds a timeout / auth / logging. Defaults to window.fetch. */
export type FetchImpl = (url: string, init?: RequestInit) => Promise<Response>

export interface ClimateApiOptions {
  /** Base URL for the climate API, e.g. `${basePath}/climate-editor/api` (no trailing slash). */
  base: string
  /** Fetch implementation. Defaults to `window.fetch`. */
  fetchImpl?: FetchImpl
}

export interface EnableOptions {
  /** Stop (and persist as disabled) whichever running controller holds this one's relay first. */
  takeOver?: boolean
}

export interface ClimateApi {
  /** GET /list — summaries, the count and the firmware's limit. */
  list(): Promise<ControllersResponse>
  /** GET /get?id= — the bare document, for editing. */
  get(id: string): Promise<ControllerDocument>
  /** POST /save — create (id absent or "") or replace (an existing id). JSON body. */
  save(doc: ControllerSaveInput): Promise<SaveResponse>
  /** POST /save with no id — the device assigns and returns the slug. */
  create(draft: ControllerDraft): Promise<SaveResponse>
  /** POST /save with `id` — replace that controller's document; a rename keeps the id. */
  update(id: string, doc: ControllerDocument | ControllerDraft): Promise<SaveResponse>
  /** POST /delete?id= — remove it (no body). */
  remove(id: string): Promise<SuccessResponse>
  /** POST /enable?id=&value=[&take_over=true] — start or stop without deleting (no body). */
  setEnabled(id: string, value: boolean, options?: EnableOptions): Promise<EnableResponse>
  /** POST /setpoint?id=&value= — move the target alone, running or not (no body). */
  setSetpoint(id: string, value: number): Promise<SuccessResponse>
  /** GET /status[?id=] — live readings; recomputed per request, nothing persisted. */
  status(id?: string): Promise<StatusResponse>
  /** GET /entities — bindable sensors and relays, with current relay owners. */
  entities(): Promise<ClimateEntitiesResponse>
  /** GET /schema — the tunable surface, straight from the firmware's own table. */
  schema(): Promise<ClimateSchema>
  /** GET /ping — liveness; returns { status:"ok" }. */
  ping(): Promise<{ status: string }>
}

export function createClimateApi(options: ClimateApiOptions): ClimateApi {
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

  const q = encodeURIComponent

  function save(doc: ControllerSaveInput): Promise<SaveResponse> {
    return jpost<SaveResponse>('/save', doc)
  }

  return {
    list() {
      return jget<ControllersResponse>('/list')
    },
    get(id) {
      return jget<ControllerDocument>(`/get?id=${q(id)}`)
    },
    save,
    create(draft) {
      // An id left on the object would turn the create into an update.
      return save({ ...draft, id: '' })
    },
    update(id, doc) {
      return save({ ...doc, id })
    },
    remove(id) {
      return jpost<SuccessResponse>(`/delete?id=${q(id)}`)
    },
    setEnabled(id, value, options = {}) {
      const takeOver = value && options.takeOver ? '&take_over=true' : ''
      return jpost<EnableResponse>(`/enable?id=${q(id)}&value=${value}${takeOver}`)
    },
    setSetpoint(id, value) {
      return jpost<SuccessResponse>(`/setpoint?id=${q(id)}&value=${q(String(value))}`)
    },
    status(id) {
      return jget<StatusResponse>(id ? `/status?id=${q(id)}` : '/status')
    },
    entities() {
      return jget<ClimateEntitiesResponse>('/entities')
    },
    schema() {
      return jget<ClimateSchema>('/schema')
    },
    ping() {
      return jget<{ status: string }>('/ping')
    }
  }
}
