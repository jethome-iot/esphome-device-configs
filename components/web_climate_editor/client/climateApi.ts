// TypeScript client for the web_climate_editor HTTP API.
//
// Lives with the backend component that owns the <url_prefix>/api/* routes, so
// the client and the C++ contract stay together; a frontend injects only the
// base URL and its fetch wrapper through createClimateApi().
//
// Backend behaviour (web_climate_editor.cpp follows ./types.ts):
//  - save, import, delete, enable, setpoint, preset, autotune, gains/add,
//    gains/rename, gains/delete, gains/apply and gains/derive answer POST only,
//    the rest GET only; a GET or POST to the wrong route, or an OPTIONS, is 405
//    with an Allow header, and an unknown route is 404. The server answers PUT,
//    DELETE, HEAD and PATCH with its own text 405 and closes the connection; this
//    client sends none of them;
//  - failures are {success:false,error} with 400/404/405/409/413/500/503/507;
//  - GET /get?id= returns the bare document, not the success envelope, and /save
//    takes it back as it is: the device gives the presets' keys and keeps the
//    active preset, so a form sends back what it got. A document whose `version`
//    is above CONFIG_VERSION came from a newer firmware, and its Save is 409; so is
//    the Save of one whose `revision` the device has moved past since it was read;
//  - /import takes what /get answered, or a backup's file, under its own id: it
//    replaces the thermostat with that id or creates it, presets' keys, active
//    preset and last_on_mode kept, and answers as /save does. The body's
//    `revision` is not read: a replaced thermostat's moves on, so a form read
//    before the import is 409;
//  - gains lists a thermostat's saved gain sets a page at a time (`after`,
//    `limit`), oldest first; a set's number is never given twice. An applied set
//    moves the revision on as a calibration does, and the answer says to what;
//  - /ping returns {status:"ok"} (NOT the success envelope);
//  - every route but /schema and /ping does its read or write on the device's main
//    loop, so an answer describes what actually happened; 503 means the loop did not
//    get to it and nothing changed. /schema and /ping answer from build-time data
//    without waiting for the loop, so they are never 503.
import type {
  AutotuneDirection,
  AutotuneRule,
  ClimateEntitiesResponse,
  ClimateSchema,
  ControllerDocument,
  ControllerDraft,
  ControllerImportInput,
  ControllerSaveInput,
  ControllersResponse,
  DeleteResponse,
  EnableResponse,
  GainSetApplyResponse,
  GainSetDeriveResponse,
  GainSetResponse,
  GainSetsResponse,
  PresetResponse,
  RelayDirection,
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
  /** Store as disabled every other enabled controller on its relays, stopping the running one. */
  takeOver?: boolean
}

export interface AutotuneOptions {
  /**
   * The relay to swing. A calibration swings the relay of the thermostat's mode, so this may be
   * left out; one given has to be the mode's, or the start is 400.
   */
  direction?: AutotuneDirection
  /** How Ku and Pu become gains; zn_pi when left out. */
  rule?: AutotuneRule
}

export interface GainSetPage {
  /** Only the sets numbered above this one; 0 when left out. */
  after?: number
  /** At most this many, 1..GAIN_SET_PAGE_MAX; GAIN_SET_PAGE_MAX when left out. */
  limit?: number
}

export interface AddGainSetOptions {
  /** The relay whose gains to save; the one the thermostat's mode, or the mode it turns on in, drives when left out. */
  direction?: RelayDirection
  /** Left out or blank, the device names it. */
  name?: string
}

export interface DeriveGainSetOptions {
  /** Apply the new set at once, refused as /gains/apply would be before anything is added. */
  apply?: boolean
  /** Left out or blank, the device names it. */
  name?: string
}

export interface ClimateApi {
  /** GET /list — summaries with the active preset, the count and the firmware's limit. */
  list(): Promise<ControllersResponse>
  /** GET /get?id= — the bare document, for editing. */
  get(id: string): Promise<ControllerDocument>
  /** POST /save — create (id absent or "") or replace (an existing id). JSON body. */
  save(doc: ControllerSaveInput): Promise<SaveResponse>
  /** POST /save with no id — the device assigns and returns the slug, and the presets' keys. */
  create(draft: ControllerDraft): Promise<SaveResponse>
  /**
   * POST /save with `id` — replace that controller's document; a rename keeps the id, and a
   * preset sent back with its key keeps it.
   */
  update(id: string, doc: ControllerDocument | ControllerDraft): Promise<SaveResponse>
  /**
   * POST /import — bring a thermostat back under the id `doc` names, as a restore does: the one
   * with that id is replaced, otherwise it is created with it. The presets' keys and the active
   * preset are kept, so the rules naming them still find them, and so is the mode a turn-on goes
   * back to.
   */
  importController(doc: ControllerImportInput): Promise<SaveResponse>
  /** POST /delete?id= — remove it (no body). */
  remove(id: string): Promise<DeleteResponse>
  /** POST /enable?id=&value=[&take_over=true] — start or stop without deleting (no body). */
  setEnabled(id: string, value: boolean, options?: EnableOptions): Promise<EnableResponse>
  /** POST /setpoint?id=&value= — move the target alone, running or not (no body). */
  setSetpoint(id: string, value: number): Promise<SuccessResponse>
  /**
   * POST /preset?id=&key= — pick a preset by its key, running or not (no body): its target, its
   * mode unless `keep`, and the label. A stopped one keeps it for when it starts.
   */
  applyPreset(id: string, key: string): Promise<PresetResponse>
  /**
   * POST /autotune?id=&value=true[&direction=][&rule=] — calibrate a running PID thermostat (no
   * body): the device swings the relay of its mode around the target for hours, then writes the
   * gains it found as that relay's and runs with them. /status follows it.
   */
  startAutotune(id: string, options?: AutotuneOptions): Promise<SuccessResponse>
  /** POST /autotune?id=&value=false — end the calibration that runs; 409 when none does. */
  cancelAutotune(id: string): Promise<SuccessResponse>
  /** GET /gains?id=[&after=][&limit=] — one page of the thermostat's saved gain sets, oldest first. */
  listGainSets(id: string, page?: GainSetPage): Promise<GainSetsResponse>
  /** POST /gains/add?id=[&direction=] — save a relay's gains as they are now as a set, named or not. */
  addGainSet(id: string, options?: AddGainSetOptions): Promise<GainSetResponse>
  /** POST /gains/rename?id=&set= — a set's new name. */
  renameGainSet(id: string, n: number, name: string): Promise<GainSetResponse>
  /** POST /gains/delete?id=&set= — remove a set; its number is not given again (no body). */
  deleteGainSet(id: string, n: number): Promise<SuccessResponse>
  /**
   * POST /gains/apply?id=&set= — make a set's gains its relay's (no body). 409 while the
   * thermostat calibrates; the answer's revision is the one a form Saves with next.
   */
  applyGainSet(id: string, n: number): Promise<GainSetApplyResponse>
  /**
   * POST /gains/derive?id=&set=&rule=[&apply=true] — a new set of another rule's gains for a set's
   * Ku and Pu; 409 for a set without them.
   */
  deriveGainSet(
    id: string,
    n: number,
    rule: AutotuneRule,
    options?: DeriveGainSetOptions
  ): Promise<GainSetDeriveResponse>
  /** GET /status[?id=] — live readings and the active preset; recomputed per request, nothing persisted. */
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

  // No body is no name: the device gives the set one.
  const nameBody = (name: string | undefined) => (name === undefined ? undefined : { name })

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
    importController(doc) {
      return jpost<SaveResponse>('/import', doc)
    },
    remove(id) {
      return jpost<DeleteResponse>(`/delete?id=${q(id)}`)
    },
    setEnabled(id, value, options = {}) {
      const takeOver = value && options.takeOver ? '&take_over=true' : ''
      return jpost<EnableResponse>(`/enable?id=${q(id)}&value=${value}${takeOver}`)
    },
    setSetpoint(id, value) {
      return jpost<SuccessResponse>(`/setpoint?id=${q(id)}&value=${q(String(value))}`)
    },
    applyPreset(id, key) {
      return jpost<PresetResponse>(`/preset?id=${q(id)}&key=${q(key)}`)
    },
    startAutotune(id, options = {}) {
      const direction = options.direction ? `&direction=${options.direction}` : ''
      const rule = options.rule ? `&rule=${options.rule}` : ''
      return jpost<SuccessResponse>(`/autotune?id=${q(id)}&value=true${direction}${rule}`)
    },
    cancelAutotune(id) {
      return jpost<SuccessResponse>(`/autotune?id=${q(id)}&value=false`)
    },
    listGainSets(id, page = {}) {
      const after = page.after !== undefined ? `&after=${page.after}` : ''
      const limit = page.limit !== undefined ? `&limit=${page.limit}` : ''
      return jget<GainSetsResponse>(`/gains?id=${q(id)}${after}${limit}`)
    },
    addGainSet(id, options = {}) {
      const direction = options.direction ? `&direction=${options.direction}` : ''
      return jpost<GainSetResponse>(`/gains/add?id=${q(id)}${direction}`, nameBody(options.name))
    },
    renameGainSet(id, n, name) {
      return jpost<GainSetResponse>(`/gains/rename?id=${q(id)}&set=${n}`, { name })
    },
    deleteGainSet(id, n) {
      return jpost<SuccessResponse>(`/gains/delete?id=${q(id)}&set=${n}`)
    },
    applyGainSet(id, n) {
      return jpost<GainSetApplyResponse>(`/gains/apply?id=${q(id)}&set=${n}`)
    },
    deriveGainSet(id, n, rule, options = {}) {
      const apply = options.apply ? '&apply=true' : ''
      const path = `/gains/derive?id=${q(id)}&set=${n}&rule=${rule}${apply}`
      return jpost<GainSetDeriveResponse>(path, nameBody(options.name))
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
