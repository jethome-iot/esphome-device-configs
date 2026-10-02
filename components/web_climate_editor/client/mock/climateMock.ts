// In-process mock for the web_climate_editor HTTP API — the dev/QA/test double
// for the contract in ../types.ts + ../climateApi.ts. Lives WITH the SDK so the
// mock, the types, and the client stay one unit and cannot drift apart.
//
// This file is the transport-agnostic CORE and stays dependency-free (client/ has
// no package.json / node_modules): seed data + a stateful dispatcher + a FetchImpl
// adapter. The Vite dev-server glue is thin and env-specific, so it lives in each
// consumer's vite.config.ts and just wraps createClimateMockStore().
//
// It answers the way the device does, in the device's order: an unknown route is
// 404, the wrong method 405 (before any id is looked at), query parameters 400,
// and only then the lookup (404) and the change itself. Only /save reads a body,
// so only /save can be 413; the body is read the way ArduinoJson reads it. A
// document is merged over the defaults and clamped to the parameter table like
// climate_hub's codec does, a name is checked like the hub checks it, a sensor
// not in °C is 400, and a relay held by a running thermostat is 409 unless the
// enable asks to take it over. An enabled Save or an enable whose sensor or
// relay the device does not have stores the thermostat enabled to wait, not
// running, with a `warning` that /list and /status repeat as `waiting`; a
// take-over by one is 400.
// /status reads a first-order room model per sensor, heated and cooled by the
// duties of the thermostats bound to it. control() stands in for Home Assistant
// setting a running thermostat's mode or target through its climate entity.
import type {
  BindableSensor,
  BindableSwitch,
  ClimateHubAction,
  ClimateHubFault,
  ClimateHubMode,
  ClimateSchema,
  ControllerDocument,
  ControllerStatus,
  ControllerSummary,
  ParamDesc,
  PidTerms
} from '../types'
import { CONFIG_MAX_BYTES, ENTITY_ID_MAX_LENGTH, NAME_MAX_LENGTH } from '../types'
import type { FetchImpl } from '../climateApi'
import { nameError, slugify, trimName, uniqueId } from '../naming'

const MAX_CONTROLLERS = 8

// --- Seed data ---------------------------------------------------------------
// Typed against the SDK contract, so these double as canonical example payloads
// AND cannot silently drift from ../types.ts (a shape change fails tsc here).
// The entity names are the dashboard mock's, so a claim shows on its Entities page.

// The unit of every setpoint, band and cut-out: a thermostat runs on nothing else.
const CELSIUS = '°C'

// Every visible sensor on the device; /entities offers only the ones in °C.
export const seedSensors: BindableSensor[] = [
  { object_id: 'temp_1', name: 'Temp 1', unit: CELSIUS },
  { object_id: 'temp_2', name: 'Temp 2', unit: CELSIUS },
  { object_id: 'pcb_temp', name: 'PCB Temp', unit: CELSIUS },
  { object_id: 'uptime', name: 'Uptime', unit: 's' }
]

export const seedSwitches: Array<Omit<BindableSwitch, 'claimed_by'>> = [
  { object_id: 'relay_1', name: 'Relay 1' },
  { object_id: 'relay_2', name: 'Relay 2' },
  { object_id: 'relay_3', name: 'Relay 3' },
  { object_id: 'relay_4', name: 'Relay 4' },
  { object_id: 'relay_5', name: 'Relay 5' },
  { object_id: 'relay_6', name: 'Relay 6' },
  { object_id: 'red_led', name: 'Red led' }
]

// Where each sensor starts and what it settles to with nothing driving it. Temp 2
// has no reading, as on the dashboard mock, so a thermostat on it faults.
const seedRooms: Record<string, { start: number | null; ambient: number }> = {
  temp_1: { start: 23.84, ambient: 15 },
  temp_2: { start: null, ambient: 15 },
  pcb_temp: { start: 31.5, ambient: 31.5 }
}

function param(
  key: string,
  label: string,
  unit: string,
  group: string,
  kind: ParamDesc['kind'],
  def: number,
  min: number,
  max: number,
  step: number,
  integer: boolean,
  hint: string
): ParamDesc {
  return { key, label, unit, group, def, min, max, step, integer, kind, hint }
}

// climate_hub's parameter table, entry for entry: the form's limits and the codec's clamps.
export const seedParams: ParamDesc[] = [
  param('update_interval_s', 'Update interval', 's', 'control', '', 30, 1, 3600, 1, true,
    'How often the control law re-reads the sensor and recomputes its output.'),
  param('visual_min_temperature', 'Minimum temperature', '°C', 'visual', '', 5, -50, 100, 0.5, false,
    'Lowest target a client may set. Also the floor every setpoint is clamped to.'),
  param('visual_max_temperature', 'Maximum temperature', '°C', 'visual', '', 45, -50, 100, 0.5, false,
    'Highest target a client may set.'),
  param('visual_step', 'Temperature step', '°C', 'visual', '', 0.5, 0.1, 5, 0.1, false,
    'Increment a slider or a +/- button moves the target by.'),
  param('period_s', 'PWM period', 's', 'output', 'pid', 300, 1, 3600, 1, true,
    'One full on+off cycle of the slow PWM. Longer is gentler on the relay, slower to respond.'),
  param('min_on_s', 'Minimum on time', 's', 'output', '', 10, 0, 3600, 1, true,
    'Once closed, the relay stays closed at least this long — protects a compressor or a boiler from short-cycling.'),
  param('min_off_s', 'Minimum off time', 's', 'output', '', 10, 0, 3600, 1, true,
    'Once opened, the relay stays open at least this long.'),
  param('sensor_timeout_s', 'Sensor timeout', 's', 'safety', '', 300, 10, 86400, 1, true,
    'No reading for this long and the controller faults and opens every relay.'),
  param('safety_max_temperature', 'Cut-out temperature', '°C', 'safety', '', 60, -50, 200, 0.5, false,
    'Reading above this and the controller cuts out until it falls back.'),
  param('kp', 'Proportional gain', '', 'pid', 'pid', 0.6, 0, 1000, 0.001, false,
    'Output per degree of error. Raise it for a faster response, lower it if the temperature oscillates.'),
  param('ki', 'Integral gain', '', 'pid', 'pid', 0.0025, 0, 1000, 0.0001, false,
    'How fast the accumulated error closes the last gap. Too high overshoots.'),
  param('kd', 'Derivative gain', '', 'pid', 'pid', 0, 0, 1000, 0.001, false,
    'Reacts to how fast the temperature is moving. Usually left at zero for a slow room.'),
  param('min_integral', 'Minimum integral', '', 'pid', 'pid', -1, -100, 100, 0.01, false,
    'Floor for the accumulated term; keeps it from winding up while the heater cannot keep up.'),
  param('max_integral', 'Maximum integral', '', 'pid', 'pid', 1, -100, 100, 0.01, false,
    'Ceiling for the accumulated term.'),
  param('starting_integral_term', 'Starting integral', '', 'pid', 'pid', 0, -100, 100, 0.01, false,
    'Value the accumulated term starts from after a boot or a restart.'),
  param('output_samples', 'Output averaging', 'samples', 'pid', 'pid', 1, 1, 100, 1, true,
    'Number of outputs averaged before the relay sees them. Smooths a noisy sensor.'),
  param('derivative_samples', 'Derivative averaging', 'samples', 'pid', 'pid', 8, 1, 100, 1, true,
    'Number of samples the rate of change is measured over.'),
  param('deadband_threshold_low', 'Deadband low', '°C', 'pid', 'pid', 0, -50, 0, 0.1, false,
    'How far below the target the calm band reaches. Zero disables the deadband.'),
  param('deadband_threshold_high', 'Deadband high', '°C', 'pid', 'pid', 0, 0, 50, 0.1, false,
    'How far above the target the calm band reaches.'),
  param('deadband_kp_multiplier', 'Deadband kp ×', '', 'pid', 'pid', 0, 0, 1, 0.01, false,
    'Proportional gain is scaled by this inside the deadband.'),
  param('deadband_ki_multiplier', 'Deadband ki ×', '', 'pid', 'pid', 0, 0, 1, 0.01, false,
    'Integral gain is scaled by this inside the deadband.'),
  param('deadband_kd_multiplier', 'Deadband kd ×', '', 'pid', 'pid', 0, 0, 1, 0.01, false,
    'Derivative gain is scaled by this inside the deadband.'),
  param('deadband_output_samples', 'Deadband averaging', 'samples', 'pid', 'pid', 1, 1, 100, 1, true,
    'Output averaging used while inside the deadband.'),
  param('hysteresis_below', 'Deviation below target', '°C', 'bang_bang', 'bang_bang', 0.5, 0.1, 20, 0.1, false,
    'How far under the target the lower switching point sits — where heating starts, and where cooling stops.'),
  param('hysteresis_above', 'Deviation above target', '°C', 'bang_bang', 'bang_bang', 0.5, 0.1, 20, 0.1, false,
    'How far over the target the upper switching point sits — where cooling starts, and where heating stops.')
]

export function schemaFor(maxControllers: number): ClimateSchema {
  const params: Record<string, ParamDesc[]> = {}
  for (const p of seedParams) (params[p.group] ??= []).push({ ...p })
  return {
    kinds: ['pid', 'bang_bang'],
    modes: ['off', 'heat', 'cool', 'heat_cool'],
    faults: ['none', 'sensor_missing', 'sensor_stale', 'relay_missing', 'overtemp'],
    max_controllers: maxControllers,
    name_max_length: NAME_MAX_LENGTH,
    params
  }
}

export const seedSchema: ClimateSchema = schemaFor(MAX_CONTROLLERS)

function def(key: string): number {
  return seedParams.find((p) => p.key === key)?.def ?? NaN
}

// The codec's clamp: NaN takes the default, out of range the nearest end, and an
// integer knob is rounded half away from zero like std::round.
function clampParam(key: string, value: number): number {
  const p = seedParams.find((d) => d.key === key)
  if (!p) return value
  if (Number.isNaN(value)) return p.def
  if (value < p.min) return p.min
  if (value > p.max) return p.max
  return p.integer ? Math.sign(value) * Math.round(Math.abs(value)) : value
}

/** A document holding every default — what a save merges its body over. */
export function blankDocument(): ControllerDocument {
  const output = () => ({
    relay_id: '',
    period_s: def('period_s'),
    min_on_s: def('min_on_s'),
    min_off_s: def('min_off_s')
  })
  return {
    version: 1,
    id: '',
    name: '',
    enabled: true,
    kind: 'bang_bang',
    sensor_id: '',
    update_interval_s: def('update_interval_s'),
    heat: output(),
    cool: output(),
    visual: {
      min_temperature: def('visual_min_temperature'),
      max_temperature: def('visual_max_temperature'),
      step: def('visual_step')
    },
    safety: { sensor_timeout_s: def('sensor_timeout_s'), max_temperature: def('safety_max_temperature') },
    pid: {
      kp: def('kp'),
      ki: def('ki'),
      kd: def('kd'),
      min_integral: def('min_integral'),
      max_integral: def('max_integral'),
      starting_integral_term: def('starting_integral_term'),
      output_samples: def('output_samples'),
      derivative_samples: def('derivative_samples'),
      deadband_threshold_low: def('deadband_threshold_low'),
      deadband_threshold_high: def('deadband_threshold_high'),
      deadband_kp_multiplier: def('deadband_kp_multiplier'),
      deadband_ki_multiplier: def('deadband_ki_multiplier'),
      deadband_kd_multiplier: def('deadband_kd_multiplier'),
      deadband_output_samples: def('deadband_output_samples')
    },
    bang_bang: { below: def('hysteresis_below'), above: def('hysteresis_above') },
    mode: 'heat',
    setpoint: 21
  }
}

// Living Room runs; Floor Heating runs on a sensor with no reading, so it shows a
// fault; Guest Room shares Living Room's relay and is off, so enabling it is 409
// until it takes the relay over.
export const seedControllers: ControllerDocument[] = [
  {
    ...blankDocument(),
    id: 'floor-heating',
    name: 'Floor Heating',
    kind: 'bang_bang',
    sensor_id: 'temp_2',
    heat: { relay_id: 'relay_2', period_s: 300, min_on_s: 60, min_off_s: 300 },
    bang_bang: { below: 0.5, above: 0.5 },
    setpoint: 24
  },
  {
    ...blankDocument(),
    id: 'guest-room',
    name: 'Guest Room',
    enabled: false,
    kind: 'bang_bang',
    sensor_id: 'temp_1',
    heat: { relay_id: 'relay_1', period_s: 300, min_on_s: 10, min_off_s: 10 },
    setpoint: 19
  },
  {
    ...blankDocument(),
    id: 'living-room',
    name: 'Living Room',
    kind: 'pid',
    sensor_id: 'temp_1',
    heat: { relay_id: 'relay_1', period_s: 300, min_on_s: 10, min_off_s: 10 },
    setpoint: 22
  }
]

// --- The body ----------------------------------------------------------------
// ArduinoJson's reading of it, with its error names: blanks, then one value, and
// whatever follows the value is ignored; a NUL ends the input, and a raw control
// character inside a string is taken. Stricter than ArduinoJson only where it
// reads more than JSON (unquoted keys, `+1`, `01`), which no client sends.

// ArduinoJson's default: the eleventh level of nesting is TooDeep.
const NESTING_LIMIT = 10

function readJson(body: string): { value: unknown } | { error: string } {
  const nul = body.indexOf('\0')
  const text = nul < 0 ? body : body.slice(0, nul)
  let i = 0
  const stop = (error: string): never => {
    throw new Error(error)
  }
  const blank = () => {
    while (i < text.length && ' \t\r\n'.includes(text.charAt(i))) i++
  }
  // The character at i; running out of input mid-value is IncompleteInput.
  const next = (): string => (i < text.length ? text.charAt(i) : stop('IncompleteInput'))
  const expect = (c: string) => {
    if (next() !== c) stop('InvalidInput')
    i++
  }

  function string() {
    i++
    for (;;) {
      const c = next()
      i++
      if (c === '"') return
      if (c === '\\') {
        const escape = next()
        i++
        if (escape === 'u') {
          for (let k = 0; k < 4; k++) {
            if (!/[0-9a-fA-F]/.test(next())) stop('InvalidInput')
            i++
          }
        } else if (!'"\\/bfnrt'.includes(escape)) {
          stop('InvalidInput')
        }
      }
    }
  }

  function number() {
    const start = i
    while (i < text.length && /[0-9+\-.eE]/.test(text.charAt(i))) i++
    if (!/^-?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?$/.test(text.slice(start, i))) {
      stop(i >= text.length ? 'IncompleteInput' : 'InvalidInput')
    }
  }

  function value(depth: number) {
    const c = next()
    if (c === '{' || c === '[') {
      if (depth >= NESTING_LIMIT) stop('TooDeep')
      const close = c === '{' ? '}' : ']'
      i++
      blank()
      if (next() === close) {
        i++
        return
      }
      for (;;) {
        if (c === '{') {
          if (next() !== '"') stop('InvalidInput')
          string()
          blank()
          expect(':')
          blank()
        }
        value(depth + 1)
        blank()
        const separator = next()
        i++
        if (separator === close) return
        if (separator !== ',') stop('InvalidInput')
        blank()
      }
    }
    if (c === '"') return string()
    if (c === '-' || (c >= '0' && c <= '9')) return number()
    const word = c === 't' ? 'true' : c === 'f' ? 'false' : c === 'n' ? 'null' : stop('InvalidInput')
    for (const letter of word) expect(letter)
  }

  try {
    blank()
    if (i >= text.length) return { error: 'EmptyInput' }
    value(0)
  } catch (e) {
    // Only stop() throws in there.
    return { error: (e as Error).message }
  }
  // Escaped for JSON.parse, which refuses what ArduinoJson took.
  const json = text
    .slice(0, i)
    .replace(/"(?:[^"\\]|\\.)*"/g, (literal) =>
      literal.replace(/[\u0001-\u001f]/g, (c) => `\\u${c.charCodeAt(0).toString(16).padStart(4, '0')}`)
    )
  return { value: JSON.parse(json) }
}

// --- The codec ---------------------------------------------------------------
// climate_hub's deserialize over a blank document: the same refusals in the same
// order, a wrong-typed value keeps the default, every number is clamped.

type Json = Record<string, unknown>

function objectOf(value: unknown): Json | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? (value as Json) : null
}

function numberOr(value: unknown, current: number): number {
  return typeof value === 'number' ? value : current
}

function textOf(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

// The device counts bytes, as ArduinoJson hands the string over.
function tooLong(id: string): boolean {
  return new TextEncoder().encode(id).length > ENTITY_ID_MAX_LENGTH
}

function decodeOutput(raw: unknown, out: ControllerDocument['heat']) {
  const obj = objectOf(raw)
  if (!obj) return
  if (obj.relay_id !== undefined && obj.relay_id !== null) out.relay_id = textOf(obj.relay_id)
  out.period_s = clampParam('period_s', numberOr(obj.period_s, out.period_s))
  out.min_on_s = clampParam('min_on_s', numberOr(obj.min_on_s, out.min_on_s))
  out.min_off_s = clampParam('min_off_s', numberOr(obj.min_off_s, out.min_off_s))
}

const KINDS = ['pid', 'bang_bang'] as const
const MODES = ['off', 'heat', 'cool', 'heat_cool'] as const
const PID_KEYS = Object.keys(blankDocument().pid) as Array<keyof ControllerDocument['pid']>

export function decodeDocument(raw: unknown): { doc: ControllerDocument } | { error: string } {
  const root = objectOf(raw)
  if (!root) return { error: 'document is not an object' }
  const doc = blankDocument()
  if (typeof root.version === 'number') doc.version = Math.trunc(root.version)
  if (root.id !== undefined && root.id !== null) doc.id = textOf(root.id)

  if (typeof root.name !== 'string' || root.name === '') return { error: 'name is required' }
  doc.name = root.name
  if (typeof root.enabled === 'boolean') doc.enabled = root.enabled
  if (root.kind !== undefined && root.kind !== null) {
    const kind = KINDS.find((k) => k === root.kind)
    if (!kind) return { error: "kind must be 'pid' or 'bang_bang'" }
    doc.kind = kind
  }
  if (typeof root.sensor_id !== 'string' || root.sensor_id === '') return { error: 'sensor_id is required' }
  if (tooLong(root.sensor_id)) return { error: `sensor_id is longer than ${ENTITY_ID_MAX_LENGTH} characters` }
  doc.sensor_id = root.sensor_id
  doc.update_interval_s = clampParam('update_interval_s', numberOr(root.update_interval_s, doc.update_interval_s))

  decodeOutput(root.heat, doc.heat)
  decodeOutput(root.cool, doc.cool)
  if (!doc.heat.relay_id && !doc.cool.relay_id) {
    return { error: 'at least one of heat.relay_id / cool.relay_id is required' }
  }
  if (tooLong(doc.heat.relay_id)) return { error: `heat.relay_id is longer than ${ENTITY_ID_MAX_LENGTH} characters` }
  if (tooLong(doc.cool.relay_id)) return { error: `cool.relay_id is longer than ${ENTITY_ID_MAX_LENGTH} characters` }
  if (doc.heat.relay_id && doc.heat.relay_id === doc.cool.relay_id) {
    return { error: 'heat and cool cannot share one relay' }
  }

  const visual = objectOf(root.visual)
  if (visual) {
    const v = doc.visual
    v.min_temperature = clampParam('visual_min_temperature', numberOr(visual.min_temperature, v.min_temperature))
    v.max_temperature = clampParam('visual_max_temperature', numberOr(visual.max_temperature, v.max_temperature))
    v.step = clampParam('visual_step', numberOr(visual.step, v.step))
  }
  if (doc.visual.max_temperature <= doc.visual.min_temperature) {
    return { error: 'visual.max_temperature must be above visual.min_temperature' }
  }

  const safety = objectOf(root.safety)
  if (safety) {
    const s = doc.safety
    s.sensor_timeout_s = clampParam('sensor_timeout_s', numberOr(safety.sensor_timeout_s, s.sensor_timeout_s))
    s.max_temperature = clampParam('safety_max_temperature', numberOr(safety.max_temperature, s.max_temperature))
  }

  const pid = objectOf(root.pid)
  if (pid) {
    for (const key of PID_KEYS) doc.pid[key] = clampParam(key, numberOr(pid[key], doc.pid[key]))
  }
  if (doc.pid.max_integral < doc.pid.min_integral) {
    return { error: 'pid.max_integral must not be below pid.min_integral' }
  }

  const band = objectOf(root.bang_bang)
  if (band) {
    doc.bang_bang.below = clampParam('hysteresis_below', numberOr(band.below, doc.bang_bang.below))
    doc.bang_bang.above = clampParam('hysteresis_above', numberOr(band.above, doc.bang_bang.above))
  }

  if (root.mode !== undefined && root.mode !== null) {
    const mode = MODES.find((m) => m === root.mode)
    if (!mode) return { error: 'mode must be one of off/heat/cool/heat_cool' }
    doc.mode = mode
  }
  if (doc.mode === 'heat' && !doc.heat.relay_id) return { error: "mode 'heat' needs heat.relay_id" }
  if (doc.mode === 'cool' && !doc.cool.relay_id) return { error: "mode 'cool' needs cool.relay_id" }
  if (doc.mode === 'heat_cool' && !(doc.heat.relay_id && doc.cool.relay_id)) {
    return { error: "mode 'heat_cool' needs both relays" }
  }

  doc.setpoint = numberOr(root.setpoint, doc.setpoint)
  if (Number.isNaN(doc.setpoint)) return { error: 'setpoint must be a number' }
  doc.setpoint = Math.min(Math.max(doc.setpoint, doc.visual.min_temperature), doc.visual.max_temperature)
  return { doc }
}

// --- Stateful core -----------------------------------------------------------

export interface MockResult {
  status: number
  body: unknown
  /** Headers the device sets besides Content-Type (the Allow of a 405). */
  headers?: Record<string, string>
}

/** What a client asks of a thermostat's climate entity; either key may be left out. */
export interface ClimateControlCall {
  /** Taken only when the thermostat's relays allow it (`off` always), else ignored. */
  mode?: ClimateHubMode
  /** Clamped into the thermostat's visual range; NaN is ignored. */
  target?: number
}

export interface ClimateMockStore {
  /**
   * Dispatch one API call. `endpoint` is the path AFTER the api base, e.g.
   * '/list', '/get'. `method` must match the route, as on the device. `body` is
   * the raw request body (read by /save only).
   */
  handle(method: string, endpoint: string, search: URLSearchParams, body: string): MockResult
  /**
   * A mode or target set through a running thermostat's climate entity, as Home
   * Assistant or the web server sets it. The thermostat keeps running: its PID
   * integral and control clock carry on and the next control pass comes at once.
   * A new mode resets the bang-bang latch, as on the device. False when `id` names
   * no running thermostat, which has no entity to call.
   */
  control(id: string, call: ClimateControlCall): boolean
}

export interface ClimateMockStoreOptions {
  /** The firmware's `max_controllers`. Default 8. */
  maxControllers?: number
  /** Clock the room model runs on, in ms. Default Date.now. */
  now?: () => number
  /**
   * Names of the device's own climates, from its YAML. A thermostat may not take
   * one, by name or by the entity id both would get: 409, as on the device.
   */
  otherClimates?: string[]
}

// GET routes read, POST routes change something.
const ROUTES = new Map<string, boolean>([
  ['list', false],
  ['get', false],
  ['status', false],
  ['entities', false],
  ['schema', false],
  ['ping', false],
  ['save', true],
  ['delete', true],
  ['enable', true],
  ['setpoint', true]
])

// The room model: every 2 s a relay at full duty adds 0.06 °C and the room loses
// 0.5 % of its lead over ambient — slow enough for the default PID to settle, and
// fast enough that a status card's trace moves within a minute.
const STEP_MS = 2000
const HEAT_PER_STEP = 0.06
const LOSS_PER_STEP = 0.005
// A page left open overnight should not replay the night on its next poll.
const MAX_CATCH_UP_MS = 2 * 3600 * 1000
const SAMPLE_EVERY_S = 10

interface Runtime {
  boundAt: number
  /** Since when the sensor has had its chance to give a first reading. */
  waitingSince: number
  lastControl: number | null
  /** A control call came in: the next pass runs whatever the interval says. */
  due: boolean
  /** A new mode: the bang-bang latch starts over on the next pass. */
  resetLatch: boolean
  prevError: number | null
  integral: number
  action: ClimateHubAction
  fault: ClimateHubFault
  heatDuty: number
  coolDuty: number
  terms: PidTerms
}

function fail(status: number, error: string): MockResult {
  return { status, body: { success: false, error } }
}

function ok(message: string, extra: Record<string, unknown> = {}): MockResult {
  return { status: 200, body: { success: true, message, ...extra } }
}

function round(value: number, digits: number): number {
  return Number(value.toFixed(digits))
}

function clamp(value: number, lo: number, hi: number): number {
  return Math.min(Math.max(value, lo), hi)
}

// The device's id parameter: present and a slug, "Room" is not an id.
function idParam(search: URLSearchParams): string | MockResult {
  const raw = search.get('id')
  if (raw === null) return fail(400, 'Missing id parameter')
  if (slugify(raw) !== raw) return fail(400, 'Invalid id parameter')
  return raw
}

// A number the whole way through, as strtof must consume it; nan and inf are not targets.
const NUMBER = /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/

/** Fresh, isolated mock state (seed is deep-copied, so instances never share state). */
export function createClimateMockStore(options: ClimateMockStoreOptions = {}): ClimateMockStore {
  const maxControllers = options.maxControllers ?? MAX_CONTROLLERS
  const now = options.now ?? (() => Date.now())
  // Under ids no slug can be, so no thermostat is ever taken for one of them.
  const yamlClimates = (options.otherClimates ?? []).map((name, n) => ({ id: `yaml/${n}`, name }))
  const docs: ControllerDocument[] = structuredClone(seedControllers)
  const running = new Map<string, Runtime>()
  // ClimateHub::waiting_: why each enabled thermostat that does not run did not start, by id.
  const waitReasons = new Map<string, string>()
  const rooms = new Map<string, { temp: number | null; ambient: number }>()
  for (const [id, room] of Object.entries(seedRooms)) rooms.set(id, { temp: room.start, ambient: room.ambient })
  const startedAt = now()
  let simulatedTo = startedAt

  const find = (id: string) => docs.find((d) => d.id === id)
  const switchName = (objectId: string) => seedSwitches.find((s) => s.object_id === objectId)?.name ?? objectId
  const relaysOf = (doc: ControllerDocument) => [doc.heat.relay_id, doc.cool.relay_id].filter((r) => r !== '')
  const readingOf = (sensorId: string) => rooms.get(sensorId)?.temp ?? null
  const heatAllowed = (doc: ControllerDocument) =>
    !!doc.heat.relay_id && (doc.mode === 'heat' || doc.mode === 'heat_cool')
  const coolAllowed = (doc: ControllerDocument) =>
    !!doc.cool.relay_id && (doc.mode === 'cool' || doc.mode === 'heat_cool')
  // The modes the entity advertises: off, and what its relays can do.
  const modeSupported = (doc: ControllerDocument, mode: ClimateHubMode) =>
    mode === 'off' ||
    (mode === 'heat' && !!doc.heat.relay_id) ||
    (mode === 'cool' && !!doc.cool.relay_id) ||
    (mode === 'heat_cool' && !!doc.heat.relay_id && !!doc.cool.relay_id)

  function holderOf(relayId: string, except: string): ControllerDocument | undefined {
    return docs.find((d) => d.id !== except && running.has(d.id) && relaysOf(d).includes(relayId))
  }

  // ControllerRuntime::start(). `prev` is what a Save replaces: the wait for a first
  // reading and the duties carry over, the PID too while its law and sensor stand, and
  // a relay a bang-bang keeps closed keeps its latch.
  function bind(doc: ControllerDocument, prev?: { doc: ControllerDocument; rt: Runtime }) {
    const t = simulatedTo
    const waiting = prev && prev.doc.sensor_id === doc.sensor_id ? prev.rt : null
    const pid = waiting && prev?.doc.kind === 'pid' && doc.kind === 'pid' ? waiting : null
    const closed = (dir: 'heat' | 'cool') =>
      !!prev &&
      prev.doc[dir].relay_id === doc[dir].relay_id &&
      relayOn(prev.doc, prev.rt, dir === 'heat' ? prev.rt.heatDuty : prev.rt.coolDuty, prev.doc[dir].period_s, t)
    const integral = pid
      ? clamp(pid.integral, doc.pid.min_integral, doc.pid.max_integral)
      : doc.pid.starting_integral_term
    const rt: Runtime = {
      boundAt: t,
      waitingSince: waiting ? waiting.waitingSince : t,
      lastControl: pid ? pid.lastControl : null,
      due: true,
      resetLatch: false,
      prevError: pid ? pid.prevError : null,
      integral,
      action: closed('heat') && heatAllowed(doc) ? 'heating' : closed('cool') && coolAllowed(doc) ? 'cooling' : 'idle',
      fault: 'none',
      heatDuty: prev ? prev.rt.heatDuty : 0,
      coolDuty: prev ? prev.rt.coolDuty : 0,
      terms: pid
        ? { ...pid.terms, integral: round(integral, 3) }
        : { error: null, proportional: null, integral: null, derivative: null, in_deadband: false }
    }
    rt.fault = faultOf(doc, rt, t)
    rt.action = standingAction(doc, rt)
    running.set(doc.id, rt)
    waitReasons.delete(doc.id)
  }

  // Silence counts from the start while the sensor has given no reading.
  function faultOf(doc: ControllerDocument, rt: Runtime, t: number): ClimateHubFault {
    const temp = readingOf(doc.sensor_id)
    if (temp === null) return t - rt.waitingSince > doc.safety.sensor_timeout_s * 1000 ? 'sensor_stale' : 'none'
    return temp > doc.safety.max_temperature ? 'overtemp' : 'none'
  }

  // What the entity shows until the next pass: off only on a fault or in mode off.
  function standingAction(doc: ControllerDocument, rt: Runtime): ClimateHubAction {
    if (rt.fault !== 'none' || doc.mode === 'off') return 'off'
    if (readingOf(doc.sensor_id) === null) return 'idle'
    if (doc.kind === 'bang_bang') return rt.resetLatch || rt.action === 'off' ? 'idle' : rt.action
    if (heatAllowed(doc) && rt.heatDuty > 0) return 'heating'
    if (coolAllowed(doc) && rt.coolDuty > 0) return 'cooling'
    return 'idle'
  }

  // A sensor that is there but cannot feed a thermostat (400), else undefined.
  function unitRefusal(sensorId: string): MockResult | undefined {
    const sensor = seedSensors.find((s) => s.object_id === sensorId)
    if (!sensor || sensor.unit === CELSIUS) return undefined
    const unit = sensor.unit === '' ? 'no unit' : sensor.unit
    return fail(400, `"${sensor.name}" reports ${unit}, not ${CELSIUS}`)
  }

  function heldRefusal(relay: string, holder: ControllerDocument): MockResult {
    return fail(409, `"${switchName(relay)}" is already driven by "${holder.name}"`)
  }

  // ClimateHub::check_entities_: what a take-over asks before it stops the holder, an
  // entity the device does not have or a sensor not in °C (400).
  function entityRefusal(doc: ControllerDocument): MockResult | undefined {
    if (!seedSensors.some((s) => s.object_id === doc.sensor_id)) {
      return fail(400, `No sensor "${doc.sensor_id}" on this device`)
    }
    const unit = unitRefusal(doc.sensor_id)
    if (unit) return unit
    const missing = relaysOf(doc).find((relay) => !seedSwitches.some((s) => s.object_id === relay))
    return missing === undefined ? undefined : fail(400, `No switch "${missing}" on this device`)
  }

  // ClimateHub::check_savable_: what an enabled Save is refused for. A missing
  // sensor or relay is not among it: the thermostat is stored and waits.
  function saveRefusal(doc: ControllerDocument): MockResult | undefined {
    const unit = unitRefusal(doc.sensor_id)
    if (unit) return unit
    for (const relay of relaysOf(doc)) {
      const holder = holderOf(relay, doc.id)
      if (holder) return heldRefusal(relay, holder)
    }
    return undefined
  }

  // Why an enabled `doc` does not start, worded and ordered as ClimateHub::start_ finds it; ''
  // when it does. A Save or an enable has refused the unit and a held relay already; a boot has not.
  function startError(doc: ControllerDocument): string {
    const sensor = seedSensors.find((s) => s.object_id === doc.sensor_id)
    if (!sensor) return `sensor '${doc.sensor_id}' not found`
    if (sensor.unit !== CELSIUS) {
      return `sensor '${doc.sensor_id}' reports ${sensor.unit === '' ? 'no unit' : sensor.unit}, not ${CELSIUS}`
    }
    for (const relay of relaysOf(doc)) {
      const holder = holderOf(relay, doc.id)
      if (holder) return `relay '${relay}' is held by '${holder.id}'`
      if (!seedSwitches.some((s) => s.object_id === relay)) return `relay '${relay}' not found`
    }
    return ''
  }

  // Runs `doc`, or keeps why not and returns it as the warning; '' when it runs.
  function start(doc: ControllerDocument, prev?: { doc: ControllerDocument; rt: Runtime }): string {
    const error = startError(doc)
    if (!error) {
      bind(doc, prev)
      return ''
    }
    const warning = `not started: ${error}`
    waitReasons.set(doc.id, warning)
    return warning
  }

  // ClimateHub::waiting_reason: '' for a thermostat that runs or is disabled.
  function waitingOf(doc: ControllerDocument): string {
    return doc.enabled && !running.has(doc.id) ? (waitReasons.get(doc.id) ?? '') : ''
  }

  function runControl(doc: ControllerDocument, rt: Runtime, t: number) {
    const temp = readingOf(doc.sensor_id)
    const faulted = rt.fault !== 'none'
    rt.fault = faultOf(doc, rt, t)
    // The fault zeroed the duties: the next pass is now, not an update_interval_s later.
    if (faulted && rt.fault === 'none') rt.due = true
    // Waiting for a first reading is no fault, but nothing to act on either.
    if (temp === null || rt.fault !== 'none' || doc.mode === 'off') {
      rt.action = standingAction(doc, rt)
      rt.heatDuty = 0
      rt.coolDuty = 0
      return
    }
    if (!rt.due && rt.lastControl !== null && t - rt.lastControl < doc.update_interval_s * 1000) return
    const dt = rt.lastControl === null ? 0 : (t - rt.lastControl) / 1000
    rt.lastControl = t
    rt.due = false

    if (doc.kind === 'pid') {
      const pid = doc.pid
      const e = doc.setpoint - temp
      const inDeadband = pid.deadband_threshold_low < -e && -e < pid.deadband_threshold_high
      const p = pid.kp * e * (inDeadband ? pid.deadband_kp_multiplier : 1)
      const ki = pid.ki * (inDeadband ? pid.deadband_ki_multiplier : 1)
      rt.integral = clamp(rt.integral + e * dt * ki, pid.min_integral, pid.max_integral)
      const kd = pid.kd * (inDeadband ? pid.deadband_kd_multiplier : 1)
      const d = dt > 0 && rt.prevError !== null ? (kd * (e - rt.prevError)) / dt : 0
      rt.prevError = e
      const out = p + rt.integral + d
      rt.heatDuty = heatAllowed(doc) ? clamp(out, 0, 1) : 0
      rt.coolDuty = coolAllowed(doc) ? clamp(-out, 0, 1) : 0
      rt.action = rt.heatDuty > 0 ? 'heating' : rt.coolDuty > 0 ? 'cooling' : 'idle'
      rt.terms = {
        error: round(e, 3),
        proportional: round(p, 3),
        integral: round(rt.integral, 3),
        derivative: round(d, 3),
        in_deadband: inDeadband
      }
      return
    }

    // Bang-bang: switch at the band's ends, hold the last action in between.
    const low = doc.setpoint - doc.bang_bang.below
    const high = doc.setpoint + doc.bang_bang.above
    let action: ClimateHubAction = rt.resetLatch ? 'idle' : rt.action
    rt.resetLatch = false
    if (temp < low) action = heatAllowed(doc) ? 'heating' : 'idle'
    else if (temp > high) action = coolAllowed(doc) ? 'cooling' : 'idle'
    else if (doc.mode === 'heat_cool' && doc.heat.relay_id && doc.cool.relay_id) action = 'idle'
    else if (action === 'off') action = 'idle'
    rt.action = action
    rt.heatDuty = rt.action === 'heating' ? 1 : 0
    rt.coolDuty = rt.action === 'cooling' ? 1 : 0
  }

  function advance(to: number) {
    simulatedTo = Math.max(simulatedTo, to - MAX_CATCH_UP_MS)
    while (to - simulatedTo >= STEP_MS) {
      simulatedTo += STEP_MS
      for (const doc of docs) {
        const rt = running.get(doc.id)
        if (rt) runControl(doc, rt, simulatedTo)
      }
      for (const [sensorId, room] of rooms) {
        if (room.temp === null) continue
        let drive = 0
        for (const doc of docs) {
          const rt = running.get(doc.id)
          if (rt && doc.sensor_id === sensorId) drive += rt.heatDuty - rt.coolDuty
        }
        room.temp += HEAT_PER_STEP * drive - (room.temp - room.ambient) * LOSS_PER_STEP
      }
    }
  }

  // A PID output is a slow PWM: the relay is closed for `duty` of every period.
  function relayOn(doc: ControllerDocument, rt: Runtime, duty: number, periodS: number, t: number): boolean {
    if (duty <= 0) return false
    if (doc.kind === 'bang_bang' || duty >= 1) return true
    const period = periodS * 1000
    return (t - rt.boundAt) % period < duty * period
  }

  // ControllerRuntime::control(): a supported mode, a target held in range, and a
  // control pass at once. Nothing else about the running thermostat changes.
  function applyControl(doc: ControllerDocument, rt: Runtime, call: ClimateControlCall) {
    const mode = MODES.find((m) => m === call.mode)
    if (mode && modeSupported(doc, mode) && mode !== doc.mode) {
      doc.mode = mode
      rt.resetLatch = true
    }
    if (typeof call.target === 'number' && !Number.isNaN(call.target)) {
      doc.setpoint = clamp(call.target, doc.visual.min_temperature, doc.visual.max_temperature)
    }
    // Published with the mode it replaced, the action would say "off" in HEAT until the next pass.
    rt.action = standingAction(doc, rt)
    rt.due = true
  }

  function statusOf(doc: ControllerDocument, t: number): ControllerStatus {
    const rt = running.get(doc.id)
    const reading = readingOf(doc.sensor_id)
    const status: ControllerStatus = {
      id: doc.id,
      running: !!rt,
      waiting: waitingOf(doc),
      action: rt ? rt.action : 'off',
      fault: rt ? rt.fault : 'none',
      current_temperature: reading === null ? null : round(reading, 2),
      sensor_age_s: rt && reading !== null ? Math.floor((t - startedAt) / 1000) % SAMPLE_EVERY_S : null,
      setpoint: doc.setpoint,
      min_temperature: doc.visual.min_temperature,
      max_temperature: doc.visual.max_temperature,
      step: doc.visual.step,
      heat_duty: rt ? round(rt.heatDuty, 3) : 0,
      cool_duty: rt ? round(rt.coolDuty, 3) : 0,
      heat_relay_on: !!rt && relayOn(doc, rt, rt.heatDuty, doc.heat.period_s, t),
      cool_relay_on: !!rt && relayOn(doc, rt, rt.coolDuty, doc.cool.period_s, t)
    }
    if (doc.kind === 'bang_bang') {
      status.switch_low = round(doc.setpoint - doc.bang_bang.below, 3)
      status.switch_high = round(doc.setpoint + doc.bang_bang.above, 3)
    }
    if (rt && doc.kind === 'pid') status.pid = { ...rt.terms }
    return status
  }

  function summaryOf(doc: ControllerDocument): ControllerSummary {
    return {
      id: doc.id,
      name: doc.name,
      enabled: doc.enabled,
      kind: doc.kind,
      mode: doc.mode,
      sensor_id: doc.sensor_id,
      heat_relay_id: doc.heat.relay_id,
      cool_relay_id: doc.cool.relay_id,
      running: running.has(doc.id),
      waiting: waitingOf(doc)
    }
  }

  function save(body: string): MockResult {
    if (new TextEncoder().encode(body).length > CONFIG_MAX_BYTES) return fail(413, 'Request body over 8 KiB')
    if (!body) return fail(400, 'Empty request body')
    const parsed = readJson(body)
    if ('error' in parsed) return fail(400, `JSON parse error: ${parsed.error}`)
    const decoded = decodeDocument(parsed.value)
    if ('error' in decoded) return fail(400, decoded.error)
    const doc = decoded.doc
    const badName = nameError(doc.name, doc.id, [])
    if (badName) return fail(400, badName)
    doc.name = trimName(doc.name)

    // From here on the device is on its loop task, where the documents live.
    const updating = doc.id !== ''
    if (updating && !find(doc.id)) return fail(404, 'Thermostat not found')
    if (!updating && docs.length >= maxControllers) {
      return fail(507, `This device allows ${maxControllers} thermostats; delete one to add another`)
    }
    const taken = nameError(doc.name, doc.id, [...docs, ...yamlClimates])
    if (taken) return fail(409, taken)
    if (!updating) doc.id = uniqueId(slugify(doc.name), docs.map((d) => d.id))
    doc.version = 1
    if (doc.enabled) {
      const refusal = saveRefusal(doc)
      if (refusal) return refusal
    }

    const before = find(doc.id)
    const rt = running.get(doc.id)
    running.delete(doc.id)
    const i = docs.findIndex((d) => d.id === doc.id)
    if (i >= 0) docs[i] = doc
    else docs.push(doc)
    docs.sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0))
    const message = updating ? 'Thermostat updated' : 'Thermostat created'
    if (!doc.enabled) {
      waitReasons.delete(doc.id)
      return ok(message, { id: doc.id })
    }
    // Stored all the same, and a running one stopped: it waits for what it names.
    const warning = start(doc, before && rt ? { doc: before, rt } : undefined)
    return warning ? ok(`${message}; ${warning}`, { id: doc.id, warning }) : ok(message, { id: doc.id })
  }

  function enable(search: URLSearchParams): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const value = search.get('value')
    if (value === null) return fail(400, 'Missing value parameter')
    if (value !== 'true' && value !== 'false') return fail(400, 'Invalid value parameter')
    const takeOverRaw = search.get('take_over')
    if (takeOverRaw !== null && takeOverRaw !== 'true' && takeOverRaw !== 'false') {
      return fail(400, 'Invalid take_over parameter')
    }
    const doc = find(id)
    if (!doc) return fail(404, 'Thermostat not found')

    if (value === 'false') {
      doc.enabled = false
      running.delete(id)
      waitReasons.delete(id)
      return ok('Thermostat disabled', { persisted: true })
    }
    if (running.has(id)) return ok('Thermostat enabled', { persisted: true })
    // Refused as a Save would be, but for a held relay the call may take over.
    const unit = unitRefusal(doc.sensor_id)
    if (unit) return unit
    const holders: ControllerDocument[] = []
    for (const relay of relaysOf(doc)) {
      const holder = holderOf(relay, id)
      if (!holder) continue
      if (takeOverRaw !== 'true') return heldRefusal(relay, holder)
      if (!holders.includes(holder)) holders.push(holder)
    }
    // A take-over stops the holder, so only for a thermostat that runs in its place.
    const refusal = holders.length ? entityRefusal(doc) : undefined
    if (refusal) return refusal
    for (const holder of holders) {
      holder.enabled = false
      running.delete(holder.id)
      waitReasons.delete(holder.id)
    }
    doc.enabled = true
    // A sensor or relay that is not there is waited for, as on a Save.
    const warning = start(doc)
    let message = 'Thermostat enabled'
    if (holders.length) message += `; ${holders.map((h) => `"${h.name}"`).join(' and ')} stopped`
    return warning ? ok(`${message}; ${warning}`, { persisted: true, warning }) : ok(message, { persisted: true })
  }

  function setpoint(search: URLSearchParams): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const raw = search.get('value')
    if (raw === null) return fail(400, 'Missing value parameter')
    if (!NUMBER.test(raw) || !Number.isFinite(Number(raw))) return fail(400, 'Invalid value parameter')
    const doc = find(id)
    if (!doc) return fail(404, 'Thermostat not found')
    // A running thermostat takes it the way Home Assistant's target reaches it.
    const rt = running.get(id)
    if (rt) applyControl(doc, rt, { target: Number(raw) })
    else doc.setpoint = clamp(Number(raw), doc.visual.min_temperature, doc.visual.max_temperature)
    return ok('Setpoint updated')
  }

  function handle(method: string, endpoint: string, search: URLSearchParams, body: string): MockResult {
    const t = now()
    advance(t)
    const name = endpoint.startsWith('/') ? endpoint.slice(1) : endpoint
    const mutating = ROUTES.get(name)
    if (mutating === undefined) return fail(404, 'Unknown endpoint')
    const allow = mutating ? 'POST' : 'GET'
    if (method.toUpperCase() !== allow) return { ...fail(405, 'Method not allowed'), headers: { Allow: allow } }

    switch (name) {
      case 'ping':
        return { status: 200, body: { status: 'ok' } }

      case 'schema':
        return { status: 200, body: schemaFor(maxControllers) }

      case 'entities':
        return {
          status: 200,
          body: {
            success: true,
            sensors: seedSensors.filter((s) => s.unit === CELSIUS).map((s) => ({ ...s })),
            switches: seedSwitches.map((s) => ({ ...s, claimed_by: holderOf(s.object_id, '')?.id ?? '' }))
          }
        }

      case 'list':
        return {
          status: 200,
          body: { success: true, count: docs.length, max_controllers: maxControllers, controllers: docs.map(summaryOf) }
        }

      case 'get': {
        const id = idParam(search)
        if (typeof id !== 'string') return id
        const doc = find(id)
        return doc ? { status: 200, body: structuredClone(doc) } : fail(404, 'Thermostat not found')
      }

      case 'status': {
        let only: string | null = null
        if (search.has('id')) {
          const id = idParam(search)
          if (typeof id !== 'string') return id
          if (!find(id)) return fail(404, 'Thermostat not found')
          only = id
        }
        const controllers = docs.filter((d) => only === null || d.id === only).map((d) => statusOf(d, t))
        return { status: 200, body: { success: true, controllers } }
      }

      case 'save':
        return save(body)

      case 'delete': {
        const id = idParam(search)
        if (typeof id !== 'string') return id
        const i = docs.findIndex((d) => d.id === id)
        if (i < 0) return fail(404, 'Thermostat not found')
        running.delete(id)
        waitReasons.delete(id)
        docs.splice(i, 1)
        return ok('Thermostat deleted')
      }

      case 'enable':
        return enable(search)

      case 'setpoint':
        return setpoint(search)

      default:
        return fail(404, 'Unknown endpoint')
    }
  }

  function control(id: string, call: ClimateControlCall): boolean {
    advance(now())
    const doc = find(id)
    const rt = running.get(id)
    if (!doc || !rt) return false
    applyControl(doc, rt, call)
    return true
  }

  // Seeds that are enabled start in id order, as at boot, or wait and keep why.
  for (const doc of docs) if (doc.enabled) start(doc)

  return { handle, control }
}

export interface ClimateMockOptions extends ClimateMockStoreOptions {
  /** URL prefix the SDK calls, WITHOUT the trailing endpoint. Default '/climate-editor/api'. */
  apiBase?: string
}

// --- Transport: FetchImpl (programmatic / unit tests) ------------------------
/**
 * A `FetchImpl` backed by a fresh mock store — inject into createClimateApi() to
 * exercise the SDK with no dev server:
 *   const api = createClimateApi({ base: '/climate-editor/api', fetchImpl: createMockFetch() })
 */
export function createMockFetch(options: ClimateMockOptions = {}): FetchImpl {
  const apiBase = options.apiBase ?? '/climate-editor/api'
  const store = createClimateMockStore(options)
  return async (url, init) => {
    const u = new URL(url, 'http://localhost')
    const endpoint = u.pathname.startsWith(apiBase) ? u.pathname.slice(apiBase.length) : u.pathname
    const body = typeof init?.body === 'string' ? init.body : ''
    const { status, body: payload, headers } = store.handle(init?.method ?? 'GET', endpoint, u.searchParams, body)
    return new Response(JSON.stringify(payload), {
      status,
      headers: { 'Content-Type': 'application/json', ...headers }
    })
  }
}
