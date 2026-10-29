// TypeScript client for the web_schedule_editor HTTP API.
//
// Lives with the backend component that owns the <url_prefix>/api/* routes, so
// the client and the C++ contract stay together; a frontend injects only the
// base URL and its fetch wrapper through createScheduleApi().
//
// Backend behaviour (web_schedule_editor.cpp follows ./types.ts):
//  - save, import, delete and enable answer POST only, the rest GET only; a GET or
//    POST to the wrong route, or an OPTIONS, is 405 with an Allow header, and an
//    unknown route is 404;
//  - failures are {success:false,error} with 400/404/405/409/413/500/503/507;
//  - GET /get?id= returns the bare document, not the success envelope, and /save
//    and /import take it back as it is. A document whose `version` is above
//    SCHEDULE_VERSION came from a newer firmware: its Save and an import over it
//    are 409;
//  - every change of a schedule moves its `revision` on, and a Save that carries
//    one other than the device's is 409 (STALE_DOCUMENT): update() of a document
//    from get() sends it; one without it is not checked;
//  - one enabled schedule per target, a target being any relay or thermostat a mode's
//    action names: a Save, an import or an enable that would make a second is 409,
//    unless the enable asks to take the targets over;
//  - /ping returns {status:"ok"} (NOT the success envelope);
//  - every route but /schema and /ping does its read or write on the device's main
//    loop, so an answer describes what actually happened; 503 means the loop did not
//    get to it and nothing changed.
import type {
  DeleteResponse,
  EnableResponse,
  SaveResponse,
  ScheduleDocument,
  ScheduleDraft,
  ScheduleImportInput,
  ScheduleSaveInput,
  ScheduleSchema,
  SchedulesResponse,
  TargetsResponse
} from './types'

export type * from './types'

/** Injectable fetch — pass one that adds a timeout / auth / logging. Defaults to window.fetch. */
export type FetchImpl = (url: string, init?: RequestInit) => Promise<Response>

export interface ScheduleApiOptions {
  /** Base URL for the schedule API, e.g. `${basePath}/schedule-editor/api` (no trailing slash). */
  base: string
  /** Fetch implementation. Defaults to `window.fetch`. */
  fetchImpl?: FetchImpl
}

export interface EnableOptions {
  /** Store as disabled every other enabled schedule that shares a target with it. */
  takeOver?: boolean
}

export interface ScheduleApi {
  /** GET /list — every schedule's targets, status and issues, the mode in force and the next point, the count, the limit and the device's clock. */
  list(): Promise<SchedulesResponse>
  /** GET /get?id= — the bare document, for editing. */
  get(id: string): Promise<ScheduleDocument>
  /** POST /save — create (id absent or "") or replace (an existing id). JSON body. */
  save(doc: ScheduleSaveInput): Promise<SaveResponse>
  /** POST /save with no id — the device derives the slug from the name and returns it. */
  create(draft: ScheduleDraft): Promise<SaveResponse>
  /**
   * POST /save with `id` — replace that schedule's document; a rename keeps the id. A document
   * from get() carries its `revision`, so a schedule another client changed since is 409.
   */
  update(id: string, doc: ScheduleDocument | ScheduleDraft): Promise<SaveResponse>
  /**
   * POST /import — bring a schedule back under the id `doc` names, as a restore does: the one with
   * that id is replaced, otherwise it is created with it, enabled or not as the document says.
   */
  importSchedule(doc: ScheduleImportInput): Promise<SaveResponse>
  /** POST /delete?id= — remove it (no body); its list stops and the targets stay as it left them. */
  remove(id: string): Promise<DeleteResponse>
  /** POST /enable?id=&value=[&take_over=true] — start or stop without deleting (no body); starting runs the list of the mode in force. */
  setEnabled(id: string, value: boolean, options?: EnableOptions): Promise<EnableResponse>
  /** GET /targets — the relays and the thermostats an action can drive, and who drives them. */
  targets(): Promise<TargetsResponse>
  /** GET /schema — the limits and the words, from the firmware's build. */
  schema(): Promise<ScheduleSchema>
  /** GET /ping — liveness; returns { status:"ok" }. */
  ping(): Promise<{ status: string }>
}

export function createScheduleApi(options: ScheduleApiOptions): ScheduleApi {
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

  function save(doc: ScheduleSaveInput): Promise<SaveResponse> {
    return jpost<SaveResponse>('/save', doc)
  }

  return {
    list() {
      return jget<SchedulesResponse>('/list')
    },
    get(id) {
      return jget<ScheduleDocument>(`/get?id=${q(id)}`)
    },
    save,
    create(draft) {
      // An id left on the object would turn the create into an update.
      return save({ ...draft, id: '' })
    },
    update(id, doc) {
      return save({ ...doc, id })
    },
    importSchedule(doc) {
      return jpost<SaveResponse>('/import', doc)
    },
    remove(id) {
      return jpost<DeleteResponse>(`/delete?id=${q(id)}`)
    },
    setEnabled(id, value, options = {}) {
      const takeOver = value && options.takeOver ? '&take_over=true' : ''
      return jpost<EnableResponse>(`/enable?id=${q(id)}&value=${value}${takeOver}`)
    },
    targets() {
      return jget<TargetsResponse>('/targets')
    },
    schema() {
      return jget<ScheduleSchema>('/schema')
    },
    ping() {
      return jget<{ status: string }>('/ping')
    }
  }
}
