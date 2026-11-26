// TypeScript client for the upstream web_server entity control routes:
//   POST /{domain}/{name}/{action}, inputs as a form body
// `id` is the entity's display name (see types.ts) and is URL-encoded here.
// web_server reads a form body before the query string, and httpd takes 512 bytes of
// URL: a long name and a long preset, both percent-encoded, would outgrow it. A non-2xx
// surfaces the backend's {error} string when present, else the HTTP status.
export type * from './types'

/** Injectable fetch — pass one that adds a timeout / auth / logging. Defaults to window.fetch. */
export type FetchImpl = (url: string, init?: RequestInit) => Promise<Response>

export interface EntitiesApiOptions {
  /** Base URL = server root prefix, e.g. "" or `${basePath}` (no trailing slash). */
  base: string
  /** Fetch implementation. Defaults to `window.fetch`. */
  fetchImpl?: FetchImpl
}

/** Inputs for POST /light/{id}/turn_on (all optional). */
export interface LightTurnOnParams {
  brightness?: number // 0-255
  r?: number // 0-255
  g?: number // 0-255
  b?: number // 0-255
  white_value?: number // 0-255
  color_temp?: number // mireds
  effect?: string
  transition?: number // seconds
  flash?: number // seconds
}

/** Inputs for POST /fan/{id}/turn_on. */
export interface FanTurnOnParams {
  speed_level?: number
  oscillation?: boolean
}

/** Inputs for POST /cover/{id}/{action}. */
export interface CoverSetParams {
  position?: number // 0.0-1.0
  tilt?: number // 0.0-1.0
}

/** Inputs for POST /climate/{id}/set. */
export interface ClimateSetParams {
  mode?: string
  fan_mode?: string
  swing_mode?: string
  /** A standard preset or one of the entity's custom presets, by name. */
  preset?: string
  target_temperature?: number
  target_temperature_low?: number
  target_temperature_high?: number
}

export interface EntitiesApi {
  // --- switch ---
  toggleSwitch(id: string): Promise<void>
  turnOnSwitch(id: string): Promise<void>
  turnOffSwitch(id: string): Promise<void>
  // --- select ---
  setSelect(id: string, value: string): Promise<void>
  // --- number ---
  setNumber(id: string, value: number): Promise<void>
  // --- button ---
  pressButton(id: string): Promise<void>
  // --- light ---
  toggleLight(id: string): Promise<void>
  lightTurnOn(id: string, params?: LightTurnOnParams): Promise<void>
  lightTurnOff(id: string, transition?: number): Promise<void>
  // --- fan ---
  toggleFan(id: string): Promise<void>
  fanTurnOn(id: string, params?: FanTurnOnParams): Promise<void>
  fanTurnOff(id: string): Promise<void>
  // --- cover ---
  coverCommand(id: string, action: 'open' | 'close' | 'stop' | 'toggle'): Promise<void>
  coverSet(id: string, params: CoverSetParams): Promise<void>
  // --- lock ---
  lock(id: string): Promise<void>
  unlock(id: string): Promise<void>
  // --- climate ---
  setClimate(id: string, params: ClimateSetParams): Promise<void>
  // --- update entity ---
  installUpdate(id: string): Promise<void>
  checkUpdate(id: string): Promise<void>
}

type Inputs = Record<string, string | number | boolean | undefined>

/** The inputs as a form body, dropping undefined values; undefined when none is left. */
function form(params: Inputs): URLSearchParams | undefined {
  const body = new URLSearchParams()
  for (const [k, v] of Object.entries(params)) {
    if (v !== undefined) body.append(k, String(v))
  }
  return body.toString() ? body : undefined
}

export function createEntitiesApi(options: EntitiesApiOptions): EntitiesApi {
  const base = options.base
  const doFetch: FetchImpl = options.fetchImpl ?? ((url, init) => fetch(url, init))

  // Surface the backend's {error} message when present, else the status.
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

  /** POST a control action, its inputs as a form body. */
  async function ctrl(path: string, inputs: Inputs = {}): Promise<void> {
    const init: RequestInit = { method: 'POST' }
    const body = form(inputs)
    if (body) init.body = body
    const res = await doFetch(`${base}${path}`, init)
    if (!res.ok) throw await toError(res)
  }
  const enc = encodeURIComponent

  return {

    // switch
    toggleSwitch: (id) => ctrl(`/switch/${enc(id)}/toggle`),
    turnOnSwitch: (id) => ctrl(`/switch/${enc(id)}/turn_on`),
    turnOffSwitch: (id) => ctrl(`/switch/${enc(id)}/turn_off`),
    // select
    setSelect: (id, value) => ctrl(`/select/${enc(id)}/set`, { option: value }),
    // number
    setNumber: (id, value) => ctrl(`/number/${enc(id)}/set`, { value }),
    // button
    pressButton: (id) => ctrl(`/button/${enc(id)}/press`),
    // light
    toggleLight: (id) => ctrl(`/light/${enc(id)}/toggle`),
    lightTurnOn: (id, params = {}) => ctrl(`/light/${enc(id)}/turn_on`, { ...params }),
    lightTurnOff: (id, transition) => ctrl(`/light/${enc(id)}/turn_off`, { transition }),
    // fan
    toggleFan: (id) => ctrl(`/fan/${enc(id)}/toggle`),
    fanTurnOn: (id, params = {}) =>
      ctrl(`/fan/${enc(id)}/turn_on`, {
        speed_level: params.speed_level,
        oscillation: params.oscillation === undefined ? undefined : params.oscillation ? 'ON' : 'OFF'
      }),
    fanTurnOff: (id) => ctrl(`/fan/${enc(id)}/turn_off`),
    // cover
    coverCommand: (id, action) => ctrl(`/cover/${enc(id)}/${action}`),
    coverSet: (id, params) => ctrl(`/cover/${enc(id)}/set`, { position: params.position, tilt: params.tilt }),
    // lock
    lock: (id) => ctrl(`/lock/${enc(id)}/lock`),
    unlock: (id) => ctrl(`/lock/${enc(id)}/unlock`),
    // climate
    setClimate: (id, params) => ctrl(`/climate/${enc(id)}/set`, { ...params }),
    // update entity
    installUpdate: (id) => ctrl(`/update/${enc(id)}/install`),
    checkUpdate: (id) => ctrl(`/update/${enc(id)}/check`)
  }
}
