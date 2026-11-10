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
// and only then the lookup (404) and the change itself. On the device a PUT,
// DELETE, HEAD or PATCH, to any path, is ESP-IDF's text/html 405 without Allow;
// here it is answered in JSON, as a GET or POST is. Only /save, /import and the
// gain set routes that take a name read a body, so only they can be 413; the
// body is read the way ArduinoJson reads it. A
// document is merged over the defaults and clamped to the parameter table like
// climate_hub's codec does, a name is checked like the hub checks it, a sensor
// not in °C is 400, and a relay held by a running thermostat, or named by an
// enabled one that waits, is 409 unless the enable asks to take it over. An
// enabled Save or an enable whose sensor or relay the device does not have
// stores the thermostat enabled to wait, not running, with a `warning` that
// /list and /status repeat as `waiting`. Taking another thermostat's relay over
// for one is 400; with no other on its relays, take_over=true answers the same
// 200. A relay a change frees starts the enabled thermostats that wait for it,
// in id order, and the answer names them; so does a climate entity it frees, for
// the ones that wait for an entity once those had their turn.
// Presets follow the hub: the keys are the device's to give, a Save keeps the
// active preset while its key is there and applies its new values at once, and a
// target or a mode set by hand keeps the label; /preset picks one by its key,
// running or not. /import is a Save under the id the body brings that keeps its
// presets' keys and its active preset, and creates the thermostat when no one has
// the id. A thermostat whose `version` is above CONFIG_VERSION stands for a file a
// newer firmware wrote: its Save or import is 409, and what changes it is not
// persisted.
// A calibration (/autotune) runs climate_hub's port of upstream's autotuner on the
// simulated room, which swings in minutes rather than hours; what ends it on the
// device ends it here, a fault, a Save, an import, a target or a mode included.
// A calibration swings the relay of the thermostat's mode; a `direction` given has to be that
// one. On success the gains go into the document as that relay's and its revision moves on,
// as an import over it moves it, and a Save carrying an older revision is 409.
// Each relay has its own gains. A body's bare pid.kp/ki/kd are read only for a relay whose
// own gains it leaves out, and /get answers them as a copy of the gains of the relay a turn-on
// goes to. A PID pass for the other relay than its state was built for starts it over on that
// relay's gains.
// Saved gain sets live per thermostat as its file does on the device: numbered from 1, a
// number never given twice, gone with the thermostat and with a create under its id, kept
// by an import. A calibration that succeeds saves one; an applied one moves the revision on
// as a calibration does. A set is named and dated by now(), taken as the device's clock
// once it is past 2019. The storage refusals (507, and 503 for a rewrite under a reader)
// are the device's alone.
// tests/components/web_climate_editor/contract.json lists the requests it must
// answer as the device does.
// /status reads a first-order room model per sensor, heated and cooled by the
// thermostats bound to it. Their relays keep min_on_s and min_off_s, counted from
// the boot for one no thermostat has held, a relay closes only once the thermostat's
// other relay has opened, and the action follows the relays, not the demand, as on
// the device; /status says which minimum, or the other relay, holds one. Nothing
// switches a relay from elsewhere here. control() stands in for Home Assistant
// setting a running thermostat's mode, target or preset through its climate entity.
import type {
  AutotuneDirection,
  AutotuneReason,
  AutotuneRule,
  AutotuneStatus,
  BindableSensor,
  BindableSwitch,
  ClimateHubAction,
  ClimateHubFault,
  ClimateHubMode,
  ClimateHubOnMode,
  ClimateSchema,
  ControllerDocument,
  ControllerStatus,
  ControllerSummary,
  GainSet,
  GainSetRef,
  OutputConfig,
  ParamDesc,
  PidConfig,
  PidGains,
  PidTerms,
  PresetConfig,
  PresetMode,
  RelayDirection,
  RelayWait
} from '../types'
import {
  AUTOTUNE_NOISEBAND,
  AUTOTUNE_RULES,
  CALIBRATION_RUNNING,
  CONFIG_MAX_BYTES,
  CONFIG_VERSION,
  ENTITY_ID_MAX_LENGTH,
  GAIN_SET_NAME_MAX_LENGTH,
  GAIN_SET_NOT_FOUND,
  GAIN_SET_PAGE_MAX,
  NAME_MAX_LENGTH,
  NEWER_FILE,
  NO_MEASUREMENT,
  PRESET_MAX_COUNT,
  STALE_DOCUMENT,
  STANDARD_PRESETS
} from '../types'
import type { FetchImpl } from '../climateApi'
import {
  nameError,
  presetKey,
  presetNameError,
  RESERVED_IDS,
  slugify,
  standardPreset,
  trimName,
  uniqueId
} from '../naming'

const MAX_CONTROLLERS = 8
// ClimateHub::start_'s reason when every climate entity is in use.
const NO_FREE_ENTITY = 'no free climate entity'
const MODES = ['off', 'heat', 'cool'] as const
const ON_MODES = ['heat', 'cool'] as const

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
  { object_id: 'relay_6', name: 'Relay 6' }
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
  param('ki', 'Integral gain', '', 'pid', 'pid', 0.0025, 0, 1000, 0.000001, false,
    'How fast the accumulated error closes the last gap. Too high overshoots.'),
  param('kd', 'Derivative gain', '', 'pid', 'pid', 0, 0, 10000, 0.001, false,
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
    modes: [...MODES],
    faults: ['none', 'sensor_stale', 'overtemp', 'relay_contested'],
    max_controllers: maxControllers,
    name_max_length: NAME_MAX_LENGTH,
    presets: { max_count: PRESET_MAX_COUNT, modes: ['keep', ...MODES], standard: [...STANDARD_PRESETS] },
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

function defaultGains(): PidGains {
  return { kp: def('kp'), ki: def('ki'), kd: def('kd') }
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
    version: CONFIG_VERSION,
    revision: 0,
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
      heat: defaultGains(),
      cool: defaultGains(),
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
    last_on_mode: 'heat',
    setpoint: 21,
    presets: [],
    active_preset: ''
  }
}

// Living Room runs, in its Comfort preset of three built-in ones and a custom one;
// Floor Heating runs on a sensor with no reading, so it shows a fault; Guest Room
// shares Living Room's relay and is off, so enabling it is 409 until it takes the
// relay over.
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
    setpoint: 22,
    presets: [
      { key: 'comfort', name: 'Comfort', setpoint: 22, mode: 'keep' },
      { key: 'eco', name: 'Eco', setpoint: 19, mode: 'keep' },
      { key: 'away', name: 'Away', setpoint: 12, mode: 'heat' },
      { key: 'night', name: 'Night', setpoint: 20, mode: 'keep' }
    ],
    active_preset: 'comfort'
  }
]

// --- The body ----------------------------------------------------------------
// ArduinoJson's reading of it, with its error names: blanks, then one value, and
// whatever follows the value is ignored; a NUL ends the input, and a raw control
// character inside a string is taken. Stricter than ArduinoJson only where it
// reads more than JSON (unquoted keys, `+1`, `01`), which no client sends.

// ArduinoJson's default: the eleventh level of nesting is TooDeep.
const NESTING_LIMIT = 10
// A gain set route's: an object with a name in it, so the third level is TooDeep.
const SET_NESTING_LIMIT = 2

function readJson(body: string, limit = NESTING_LIMIT): { value: unknown } | { error: string } {
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
      if (depth >= limit) stop('TooDeep')
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
type SharedPidKey = Exclude<keyof PidConfig, 'heat' | 'cool' | 'kp' | 'ki' | 'kd'>
// What both relays share; each one's gains are its own.
const PID_SHARED_KEYS = (Object.keys(blankDocument().pid) as Array<keyof PidConfig>).filter(
  (key): key is SharedPidKey => key !== 'heat' && key !== 'cool'
)
const DIRECTIONS = ['heat', 'cool'] as const

// A relay without gains of its own, as in a file from before they were per relay, takes the
// bare ones beside them; a gain neither has keeps its default. Clamped as the codec clamps.
function decodeGains(pid: Json, dir: RelayDirection, out: PidGains) {
  const from = objectOf(pid[dir]) ?? pid
  out.kp = clampParam('kp', numberOr(from.kp, out.kp))
  out.ki = clampParam('ki', numberOr(from.ki, out.ki))
  out.kd = clampParam('kd', numberOr(from.kd, out.kd))
}

// The modes the entity advertises: off, and what its relays can do.
function modeSupported(doc: ControllerDocument, mode: ClimateHubMode): boolean {
  return mode === 'off' || (mode === 'heat' && !!doc.heat.relay_id) || (mode === 'cool' && !!doc.cool.relay_id)
}

// ClimateConfig::on_mode(): the mode while it is not off, else `last` while the relays serve
// it, else heat, or cool for a cooling-only thermostat. What the device serves as last_on_mode.
function onMode(doc: ControllerDocument, last: ClimateHubOnMode | undefined): ClimateHubOnMode {
  if (doc.mode !== 'off') return doc.mode
  if (last && modeSupported(doc, last)) return last
  return doc.heat.relay_id ? 'heat' : 'cool'
}

// ClimateConfig::set_mode(): the mode it leaves is the one a turn-on goes back to.
function setMode(doc: ControllerDocument, mode: ClimateHubMode) {
  const last = onMode(doc, doc.last_on_mode)
  doc.mode = mode
  doc.last_on_mode = onMode(doc, last)
}

// ClimateConfig's own mode rule, worded for the thermostat or for one of its presets.
function modeNeeds(mode: ClimateHubMode): string {
  return mode === 'heat' ? 'heat.relay_id' : 'cool.relay_id'
}

// ClimateConfig::on_direction(): the relay a turn-on goes to, whose gains a PID runs on.
function onDirection(doc: ControllerDocument): RelayDirection {
  return onMode(doc, doc.last_on_mode) === 'cool' ? 'cool' : 'heat'
}

// The document as /get answers it: each relay's gains, and the bare ones beside them a copy of
// the relay a turn-on goes to, for a firmware from before each relay had its own.
function wireDocument(doc: ControllerDocument): ControllerDocument {
  const out = structuredClone(doc)
  const { heat, cool, kp: _kp, ki: _ki, kd: _kd, ...shared } = out.pid
  out.pid = { heat, cool, ...out.pid[onDirection(doc)], ...shared }
  return out
}

// What a seed holds as a file from before each relay had its own gains: its one set is both
// relays'. The bare keys are /get's to add.
function ownGains(doc: ControllerDocument): ControllerDocument {
  const pid = doc.pid as Partial<PidConfig> & Json
  const bare: PidGains = { kp: pid.kp ?? def('kp'), ki: pid.ki ?? def('ki'), kd: pid.kd ?? def('kd') }
  const { kp: _kp, ki: _ki, kd: _kd, ...rest } = doc.pid
  return { ...doc, pid: { ...rest, heat: { ...(pid.heat ?? bare) }, cool: { ...(pid.cool ?? bare) } } }
}

// The presets' structure: a list of objects with mode words. What the values and the
// names may be is checked once the whole list is read, as the device does.
function decodePresets(raw: unknown): { presets: PresetConfig[] } | { error: string } {
  if (raw === undefined || raw === null) return { presets: [] }
  if (!Array.isArray(raw)) return { error: 'presets must be a list' }
  if (raw.length > PRESET_MAX_COUNT) return { error: `A thermostat has at most ${PRESET_MAX_COUNT} presets` }
  const presets: PresetConfig[] = []
  for (const item of raw as unknown[]) {
    const at = `Preset ${presets.length + 1}`
    const obj = objectOf(item)
    if (!obj) return { error: `${at} must be an object` }
    let mode: PresetMode = 'keep'
    if (obj.mode !== undefined && obj.mode !== null && obj.mode !== 'keep') {
      const word = MODES.find((m) => m === obj.mode)
      if (!word) return { error: `${at}: mode must be one of keep/off/heat/cool` }
      mode = word
    }
    presets.push({ key: textOf(obj.key), name: trimName(textOf(obj.name)), setpoint: numberOr(obj.setpoint, NaN), mode })
  }
  return { presets }
}

// What a preset needs to be applied, then its name: the first rule a row breaks.
function presetsError(doc: ControllerDocument): string {
  for (const [i, preset] of doc.presets.entries()) {
    const at = `Preset ${i + 1}`
    if (Number.isNaN(preset.setpoint)) return `${at}: setpoint must be a number`
    if (preset.mode !== 'keep' && !modeSupported(doc, preset.mode)) {
      return `${at}: mode '${preset.mode}' needs ${modeNeeds(preset.mode)}`
    }
    if (preset.key === '') continue
    if (slugify(preset.key) !== preset.key) {
      return `${at}: key must be a slug: lowercase letters, digits and single dashes`
    }
    const first = doc.presets.findIndex((other) => other.key === preset.key)
    if (first < i) return `${at}: key '${preset.key}' is already used by Preset ${first + 1}`
  }
  for (let i = 0; i < doc.presets.length; i++) {
    const error = presetNameError(doc.presets, i)
    if (error) return error
  }
  return ''
}

// Older files read as this version; a newer one keeps its number, rounded up so 3.5 stays newer.
function readVersion(value: unknown): number {
  return typeof value === 'number' && value > CONFIG_VERSION ? Math.min(Math.ceil(value), 65535) : CONFIG_VERSION
}

// A count the device can hold, or none: 0.
function readRevision(value: unknown): number | null {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= 0xffffffff ? value : null
}

// ClimateConfig::assign_preset_keys: a preset without a key gets one from its name.
function assignPresetKeys(doc: ControllerDocument) {
  for (const preset of doc.presets) {
    if (preset.key === '') preset.key = presetKey(preset.name, doc.presets.map((p) => p.key))
  }
}

function findPreset(doc: ControllerDocument, key: string): PresetConfig | undefined {
  return key === '' ? undefined : doc.presets.find((p) => p.key === key)
}

// ClimateConfig::pick_preset: its target, held in range, its mode if the relays serve it,
// and the label.
function pickPreset(doc: ControllerDocument, preset: PresetConfig) {
  doc.setpoint = Math.min(Math.max(preset.setpoint, doc.visual.min_temperature), doc.visual.max_temperature)
  doc.active_preset = preset.key
  if (preset.mode !== 'keep' && modeSupported(doc, preset.mode)) setMode(doc, preset.mode)
}

/**
 * `requireId` reads the document as the device reads a file, and an import: the id first, a
 * slug.
 */
export function decodeDocument(raw: unknown, requireId = false): { doc: ControllerDocument } | { error: string } {
  const root = objectOf(raw)
  if (!root) return { error: 'document is not an object' }
  const doc = blankDocument()
  doc.version = readVersion(root.version)
  doc.revision = readRevision(root.revision) ?? 0
  if (requireId) {
    doc.id = textOf(root.id)
    if (doc.id === '') return { error: 'id is required' }
    if (slugify(doc.id) !== doc.id) return { error: 'id must be a slug: lowercase letters, digits and single dashes' }
  } else if (root.id !== undefined && root.id !== null) {
    doc.id = textOf(root.id)
  }

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
    for (const dir of DIRECTIONS) decodeGains(pid, dir, doc.pid[dir])
    for (const key of PID_SHARED_KEYS) doc.pid[key] = clampParam(key, numberOr(pid[key], doc.pid[key]))
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
    if (!mode) return { error: 'mode must be one of off/heat/cool' }
    doc.mode = mode
  }
  // State, like active_preset: one the relays no longer serve gives way. A word it does not
  // know is refused as any other is.
  let last: ClimateHubOnMode | undefined
  if (root.last_on_mode !== undefined && root.last_on_mode !== null) {
    last = ON_MODES.find((m) => m === root.last_on_mode)
    if (!last) return { error: 'last_on_mode must be one of heat/cool' }
  }
  doc.last_on_mode = onMode(doc, last)
  if (!modeSupported(doc, doc.mode)) return { error: `mode '${doc.mode}' needs ${modeNeeds(doc.mode)}` }

  doc.setpoint = numberOr(root.setpoint, doc.setpoint)
  if (Number.isNaN(doc.setpoint)) return { error: 'setpoint must be a number' }

  // After the thermostat's own rules, before its name's.
  const presets = decodePresets(root.presets)
  if ('error' in presets) return presets
  doc.presets = presets.presets
  const broken = presetsError(doc)
  if (broken) return { error: broken }

  const clamp = (value: number) => Math.min(Math.max(value, doc.visual.min_temperature), doc.visual.max_temperature)
  doc.setpoint = clamp(doc.setpoint)
  for (const preset of doc.presets) preset.setpoint = clamp(preset.setpoint)
  assignPresetKeys(doc)
  // State rather than a rule: the preset it named is gone, so none is active.
  const active = textOf(root.active_preset)
  doc.active_preset = findPreset(doc, active) ? active : ''
  return { doc }
}

// --- Calibration -------------------------------------------------------------
// climate_hub's PidAutotuner, which is upstream's relay-oscillation autotuner: the relay
// swings the room AUTOTUNE_NOISEBAND around the target until three swings each way are
// measured; Ku = 4d / (πa) from their amplitude a and the relay's half-span d, Pu from the
// time between crossings.

const RULE_FACTORS: Record<AutotuneRule, [number, number, number]> = {
  zn_pi: [0.45, 0.54, 0],
  zn_pid: [0.6, 1.2, 0.075],
  pessen: [0.7, 1.75, 0.105],
  some_overshoot: [0.333, 0.667, 0.111],
  no_overshoot: [0.2, 0.4, 0.0625]
}
// autotune.cpp's gains_for_rule: kp = f·Ku, ki = f·Ku/Pu, kd = f·Ku·Pu, each held in its range.
function gainsForRule(ku: number, pu: number, rule: AutotuneRule): { gains: PidGains; clamped: boolean } {
  const [kp, ki, kd] = RULE_FACTORS[rule]
  let clamped = false
  const gain = (key: string, raw: number) => {
    const held = clampParam(key, raw)
    if (!(held === raw)) clamped = true
    return held
  }
  return { gains: { kp: gain('kp', kp * ku), ki: gain('ki', (ki * ku) / pu), kd: gain('kd', kd * ku * pu) }, clamped }
}

const AUTOTUNE_MAX_MS = 24 * 3600 * 1000
const AUTOTUNE_STALL_MS = 6 * 3600 * 1000
// PidAutotuner::MAX_INTERVALS: more crossings are noise around the target, and end the run.
const AUTOTUNE_MAX_INTERVALS = 64
const AUTOTUNE_EVEN_RATIO = 0.66

type Side = 'init' | 'positive' | 'negative'

class Tuner {
  relay: Side = 'init'
  switches = 0
  outPositive = 1
  outNegative = 0
  crossing: Side = 'init'
  lastCrossing: number | null = null
  intervals: number[] = []
  noisy = false
  lastRelay: Side = 'init'
  phaseMin = NaN
  phaseMax = NaN
  phaseMinAt = 0
  phaseMaxAt = 0
  mins: number[] = []
  maxs: number[] = []
  extremes: Array<{ at: number; error: number }> = []
  ku = 0
  pu = 0
  finished = false

  // One direction: heating is (0, 1), cooling (-1, 0).
  constructor(cooling: boolean) {
    this.outPositive = cooling ? 0 : 1
    this.outNegative = cooling ? -1 : 0
  }

  update(setpoint: number, value: number, at: number): number {
    if (this.finished) return 0
    const error = setpoint - value
    const band = AUTOTUNE_NOISEBAND
    if (this.relay === 'init') this.relay = error > band ? 'positive' : 'negative'
    if (this.relay === 'positive' && error < -band) {
      this.relay = 'negative'
      this.switches++
    } else if (this.relay === 'negative' && error > band) {
      this.relay = 'positive'
      this.switches++
    }
    // A quarter of the band keeps sensor noise from counting as a crossing.
    const quarter = band / 4
    if (this.crossing === 'init') this.crossing = error > quarter ? 'positive' : 'negative'
    const crossed =
      (this.crossing === 'positive' && error < -quarter) || (this.crossing === 'negative' && error > quarter)
    if (crossed) {
      this.crossing = this.crossing === 'positive' ? 'negative' : 'positive'
      if (this.lastCrossing !== null && this.intervals.length >= AUTOTUNE_MAX_INTERVALS) this.noisy = true
      else if (this.lastCrossing !== null) this.intervals.push(at - this.lastCrossing)
      this.lastCrossing = at
    }
    if (this.relay !== this.lastRelay) {
      if (this.lastRelay === 'positive') {
        this.maxs.push(this.phaseMax)
        this.extremes.push({ at: this.phaseMaxAt, error: this.phaseMax })
      } else if (this.lastRelay === 'negative') {
        this.mins.push(this.phaseMin)
        this.extremes.push({ at: this.phaseMinAt, error: this.phaseMin })
      }
      this.phaseMin = this.phaseMax = error
      this.phaseMinAt = this.phaseMaxAt = at
    }
    this.lastRelay = this.relay
    if (error < this.phaseMin) [this.phaseMin, this.phaseMinAt] = [error, at]
    if (error > this.phaseMax) [this.phaseMax, this.phaseMaxAt] = [error, at]
    const output = this.relay === 'positive' ? this.outPositive : this.outNegative

    // The first pass with enough data ends it.
    const pairs = Math.min(this.mins.length, this.maxs.length)
    if (this.noisy || this.intervals.length < 2 || pairs < 3) return output
    let total = 0
    for (let i = 1; i < pairs - 1; i++) total += Math.abs(this.maxs[i]! - this.mins[i + 1]!)
    const amplitude = total / (pairs - 2) / 2
    this.ku = (4 * ((this.outPositive - this.outNegative) / 2)) / (Math.PI * amplitude)
    this.pu = ((this.intervals.reduce((a, b) => a + b, 0) / this.intervals.length) / 1000) * 2
    this.finished = true
    return output
  }

  symmetrical(): boolean {
    return this.intervals.length > 0 && Math.min(...this.intervals) / Math.max(...this.intervals) >= 0.66
  }

  // The smallest swing over the largest, the first phase left out.
  swingRatio(): number {
    const swings = this.extremes.slice(1).map((e, i) => Math.abs(e.error - this.extremes[i]!.error)).slice(1)
    return swings.length ? Math.min(...swings) / Math.max(...swings) : NaN
  }
}

interface Run {
  state: AutotuneStatus['state']
  reason: AutotuneReason
  direction: AutotuneDirection
  rule: AutotuneRule
  // The target when it started: a later one ends the run and leaves its chart where it was.
  setpoint: number
  startedAt: number
  endedAt: number
  lastSwitch: number
  tuner: Tuner
  old: PidGains
  new: PidGains | null
  flags: AutotuneStatus['flags']
  // The gain set a success went into, followed through a rename.
  saved: GainSetRef | null
}

// What the device's file gives back for a gain: ArduinoJson writes a float to six decimals,
// fewer as the whole part grows, and with an exponent past 1e7 or under 1e-5.
function asStored(value: number): number {
  const v = Math.abs(Math.fround(value))
  if (v === 0 || !Number.isFinite(v)) return value
  if (v >= 1e7 || v <= 1e-5) return Number(Math.fround(value).toPrecision(7))
  const whole = v < 1 ? 1 : Math.floor(Math.log10(v)) + 1
  return Number(Math.fround(value).toFixed(7 - whole))
}

// --- Gain sets ---------------------------------------------------------------
// gain_sets.cpp: the rules on a set's name and the name it gets when given none.

// rule_label(): how a set's name says which rule made it.
const RULE_LABEL: Record<AutotuneRule, string> = {
  zn_pi: 'ZN PI',
  zn_pid: 'ZN PID',
  pessen: 'Pessen',
  some_overshoot: 'Some overshoot',
  no_overshoot: 'No overshoot'
}
const MANUAL_LABEL = 'manual'

// validate_set_name() on the trimmed name, '' when it passes. A JS string is UTF-16, so the only
// text that is not UTF-8 is a lone surrogate; the device finds one before it counts.
function setNameError(trimmed: string): string {
  if (trimmed === '') return 'Name is required'
  let chars = 0
  let control = false
  for (const c of trimmed) {
    const cp = c.codePointAt(0)!
    if (cp >= 0xd800 && cp <= 0xdfff) return 'Name is not valid UTF-8'
    control = control || cp < 0x20 || (cp >= 0x7f && cp <= 0x9f)
    chars++
  }
  if (chars > GAIN_SET_NAME_MAX_LENGTH) return `Name is longer than ${GAIN_SET_NAME_MAX_LENGTH} characters`
  return control ? 'Name cannot contain control characters' : ''
}

// ESPTime::is_valid(): a clock before 2019 has not been set yet.
function clockAt(ms: number): Date | null {
  const date = new Date(ms)
  return date.getFullYear() >= 2019 ? date : null
}

// default_set_name(): the local time it was saved at, or without a clock its relay and number.
function defaultSetName(clock: Date | null, direction: RelayDirection, label: string, n: number): string {
  if (!clock) return `${direction === 'cool' ? 'Cool' : 'Heat'} ${label} #${n}`
  const two = (value: number) => String(value).padStart(2, '0')
  const day = `${String(clock.getFullYear()).padStart(4, '0')}-${two(clock.getMonth() + 1)}-${two(clock.getDate())}`
  return `${day} ${two(clock.getHours())}:${two(clock.getMinutes())} ${label}`
}

// What a set holds besides what its file gives it: its number, its name and its time.
type GainSetFields = Omit<GainSet, 'n' | 'name' | 'time'>

// The device writes floats; a non-finite measurement is written null.
const measured = (value: number) => (Number.isFinite(value) ? asStored(value) : null)

// --- Stateful core -----------------------------------------------------------

export interface MockResult {
  status: number
  body: unknown
  /** Headers the device sets besides Content-Type (the Allow of a 405). */
  headers?: Record<string, string>
}

/**
 * What a client asks of a thermostat's climate entity; any key may be left out. The preset
 * comes first, the mode and the target over it.
 */
export interface ClimateControlCall {
  /** Taken only when the thermostat's relays allow it (`off` always), else ignored. */
  mode?: ClimateHubMode
  /** Clamped into the thermostat's visual range; NaN is ignored. */
  target?: number
  /**
   * A preset by the name Home Assistant and the web server show: a built-in one in any case,
   * a custom one exactly. Its target, its mode unless `keep`, and the label; one the
   * thermostat does not have is ignored.
   */
  preset?: string
}

export interface ClimateMockStore {
  /**
   * Dispatch one API call. `endpoint` is the path AFTER the api base, e.g.
   * '/list', '/get'. `method` must match the route, as on the device. `body` is
   * the raw request body (read by /save, /import and the gain set routes that take a name).
   */
  handle(method: string, endpoint: string, search: URLSearchParams, body: string): MockResult
  /**
   * A mode, target or preset set through a running thermostat's climate entity, as
   * Home Assistant or the web server sets it. The thermostat keeps running: its PID
   * integral and control clock carry on and the next control pass comes at once.
   * A new mode resets the bang-bang latch, as on the device. False when `id` names
   * no running thermostat, which has no entity to call.
   */
  control(id: string, call: ClimateControlCall): boolean
}

export interface ClimateMockStoreOptions {
  /** The firmware's `max_controllers`. Default 8. */
  maxControllers?: number
  /**
   * The climate entities the thermostats run in. Default maxControllers; fewer stands for
   * a firmware whose entity table had no room for them all, where an enabled thermostat that
   * finds every one in use waits: `not started: no free climate entity`.
   */
  climateEntities?: number
  /**
   * The thermostats on the device at boot, in place of seedControllers. One whose
   * `version` is above CONFIG_VERSION stands for a file a newer firmware wrote.
   */
  seed?: ControllerDocument[]
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
  ['import', true],
  ['delete', true],
  ['enable', true],
  ['setpoint', true],
  ['preset', true],
  ['autotune', true],
  ['gains', false],
  ['gains/add', true],
  ['gains/rename', true],
  ['gains/delete', true],
  ['gains/apply', true],
  ['gains/derive', true]
])

// The room model: every 2 s a closed heating relay adds 0.06 °C, a closed cooling one
// takes it away, and the room loses 0.5 % of its lead over ambient — slow enough for the
// default PID to settle, and fast enough that a status card's trace moves within a minute.
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
  prevError: number | null
  integral: number
  /** What a bang-bang holds between its switching points. */
  latch: ClimateHubAction
  /** What the relays do, as the entity shows it. */
  action: ClimateHubAction
  fault: ClimateHubFault
  heatDuty: number
  coolDuty: number
  heatWait: RelayWait
  coolWait: RelayWait
  terms: PidTerms
  /** The relay the PID state was built for, whose gains it runs on. */
  pidDirection: RelayDirection
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

// WebClimateEditor::read_count_: present, digits only and at most ten of them, in [min, max].
function countParam(search: URLSearchParams, name: string, min: number, max: number): number | MockResult {
  const raw = search.get(name)
  if (raw === null) return fail(400, `Missing ${name} parameter`)
  const value = Number(raw)
  if (!/^[0-9]{1,10}$/.test(raw) || value < min || value > max) return fail(400, `Invalid ${name} parameter`)
  return value
}

const COUNT_MAX = 0xffffffff

// WebClimateEditor::read_set_name_: the body's name, '' for none; a rename needs a body.
function setNameParam(body: string, required: boolean): string | MockResult {
  if (new TextEncoder().encode(body).length > CONFIG_MAX_BYTES) return fail(413, 'Request body over 8 KiB')
  if (!body) return required ? fail(400, 'Empty request body') : ''
  const parsed = readJson(body, SET_NESTING_LIMIT)
  if ('error' in parsed) return fail(400, `JSON parse error: ${parsed.error}`)
  const root = objectOf(parsed.value)
  if (!root) return fail(400, 'The body must be a JSON object')
  if (root.name !== undefined && root.name !== null && typeof root.name !== 'string') {
    return fail(400, 'name must be a string')
  }
  return textOf(root.name)
}

// ClimateHub's no_relay(): a set for a relay the thermostat does not have.
function noRelay(direction: RelayDirection): MockResult {
  return fail(409, `The thermostat has no ${direction === 'cool' ? 'cooling' : 'heating'} relay`)
}

/** Fresh, isolated mock state (seed is deep-copied, so instances never share state). */
export function createClimateMockStore(options: ClimateMockStoreOptions = {}): ClimateMockStore {
  const maxControllers = options.maxControllers ?? MAX_CONTROLLERS
  const climateEntities = options.climateEntities ?? maxControllers
  const now = options.now ?? (() => Date.now())
  // Under ids no slug can be, so no thermostat is ever taken for one of them.
  const yamlClimates = (options.otherClimates ?? []).map((name, n) => ({ id: `yaml/${n}`, name }))
  const docs: ControllerDocument[] = structuredClone(options.seed ?? seedControllers).map(ownGains)
  docs.sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0))
  const running = new Map<string, Runtime>()
  // ClimateHub::waiting_: why each enabled thermostat that does not run did not start, by id.
  const waitReasons = new Map<string, string>()
  // ClimateHub::autotunes_: each thermostat's last calibration, by id, until a delete.
  const runs = new Map<string, Run>()
  // Each thermostat's gain sets file, by id: its sets in file order, and the highest number it
  // ever held, which a deleted set's number stays under.
  const gainFiles = new Map<string, { last: number; sets: GainSet[] }>()
  const rooms = new Map<string, { temp: number | null; ambient: number }>()
  for (const [id, room] of Object.entries(seedRooms)) rooms.set(id, { temp: room.start, ambient: room.ambient })
  const startedAt = now()
  let simulatedTo = startedAt
  // Each relay, by object id: whether it is closed and since when. One no thermostat has held
  // counts as opened at the boot, so its min_off runs from there.
  const relays = new Map<string, { on: boolean; since: number }>()
  const relayOf = (relayId: string) => {
    let relay = relays.get(relayId)
    if (!relay) relays.set(relayId, (relay = { on: false, since: startedAt }))
    return relay
  }

  const find = (id: string) => docs.find((d) => d.id === id)
  const switchName = (objectId: string) => seedSwitches.find((s) => s.object_id === objectId)?.name ?? objectId
  const relaysOf = (doc: ControllerDocument) => [doc.heat.relay_id, doc.cool.relay_id].filter((r) => r !== '')
  const readingOf = (sensorId: string) => rooms.get(sensorId)?.temp ?? null
  const heatAllowed = (doc: ControllerDocument) => !!doc.heat.relay_id && doc.mode === 'heat'
  const coolAllowed = (doc: ControllerDocument) => !!doc.cool.relay_id && doc.mode === 'cool'
  // Its file came from a newer firmware: this one never writes it.
  const newer = (doc: ControllerDocument) => doc.version > CONFIG_VERSION

  function holderOf(relayId: string, except: string): ControllerDocument | undefined {
    return docs.find((d) => d.id !== except && running.has(d.id) && relaysOf(d).includes(relayId))
  }

  // ClimateHub::reserver_of_: the first other thermostat by id that is enabled, waits, and
  // names one of `doc`'s relays, a relay `doc` holds already aside.
  function reserverOf(doc: ControllerDocument): { relay: string; holder: ControllerDocument } | undefined {
    const stored = find(doc.id)
    const own = stored && running.has(doc.id) ? relaysOf(stored) : []
    for (const other of docs) {
      if (other.id === doc.id || !other.enabled || running.has(other.id)) continue
      const relay = relaysOf(doc).find((r) => !own.includes(r) && relaysOf(other).includes(r))
      if (relay) return { relay, holder: other }
    }
    return undefined
  }

  // The relays the running thermostats hold.
  const heldRelays = () => new Set(docs.filter((d) => running.has(d.id)).flatMap(relaysOf))
  const entityFree = () => running.size < climateEntities
  // What a change frees is what was here before it: the relays held, and who ran.
  const snapshot = () => ({ relays: heldRelays(), running: new Set(running.keys()) })

  const quoted = (names: string[]) => names.map((n) => `"${n}"`).join(' and ')

  // ClimateHub::start_waiters_: every enabled thermostat but `skip` that names a relay held
  // before the change and free after it tries to start, in id order; then, if one that ran
  // before no longer does, the ones whose start found no free climate entity, while one is
  // free. The answer's note on who started, '' when nobody did. Only a boot leaves one waiting
  // on a held relay; the API refuses that.
  function startWaiters(before: ReturnType<typeof snapshot>, skip: string): string {
    const after = heldRelays()
    const freed = [...before.relays].filter((r) => !after.has(r))
    // ClimateHub::let_go_ first: a freed relay opens before a waiter takes it, so its min_off holds that start.
    for (const relay of freed) forceOff(relay, simulatedTo)
    const waits = (doc: ControllerDocument) => doc.enabled && doc.id !== skip && !running.has(doc.id)
    const started: string[] = []
    for (const doc of docs) {
      if (waits(doc) && relaysOf(doc).some((r) => freed.includes(r)) && !start(doc)) started.push(doc.name)
    }
    if ([...before.running].some((id) => !running.has(id))) {
      for (const doc of docs) {
        if (!entityFree()) break
        if (waits(doc) && waitReasons.get(doc.id) === `not started: ${NO_FREE_ENTITY}` && !start(doc)) {
          started.push(doc.name)
        }
      }
    }
    return started.length ? `; ${quoted(started)} started` : ''
  }

  // ControllerRuntime::start(). `prev` is what a Save replaces: the wait for a first
  // reading, the duties and the action of a direction that keeps its relay carry over, the
  // PID too while its law and sensor stand, and a bang-bang's latch while both are
  // bang-bang; a start latches on a closed relay instead.
  function bind(doc: ControllerDocument, prev?: { doc: ControllerDocument; rt: Runtime }) {
    const t = simulatedTo
    const waiting = prev && prev.doc.sensor_id === doc.sensor_id ? prev.rt : null
    const pid = waiting && prev?.doc.kind === 'pid' && doc.kind === 'pid' ? waiting : null
    const keepLatch = prev?.doc.kind === 'bang_bang' && doc.kind === 'bang_bang'
    const latched = (dir: 'heat' | 'cool') =>
      keepLatch
        ? prev!.rt.latch === (dir === 'heat' ? 'heating' : 'cooling')
        : !!doc[dir].relay_id && relayOf(doc[dir].relay_id).on
    // A relay the Save keeps carries what holds it until the next pass, in either direction now:
    // the device's claim is the relay's.
    const keeps = (dir: 'heat' | 'cool') => !!prev && prev.doc[dir].relay_id === doc[dir].relay_id
    const keptWait = (dir: 'heat' | 'cool'): RelayWait => {
      const relay = doc[dir].relay_id
      if (!prev || !relay) return 'none'
      if (prev.doc.heat.relay_id === relay) return prev.rt.heatWait
      return prev.doc.cool.relay_id === relay ? prev.rt.coolWait : 'none'
    }
    // A PID's gap between pulses was the old relay's: the one a Save moves it to has not closed.
    const keptAction = (): ClimateHubAction => {
      if (!prev) return 'off'
      const was = prev.rt.action
      return (was === 'heating' && !keeps('heat')) || (was === 'cooling' && !keeps('cool')) ? 'idle' : was
    }
    const integral = pid
      ? clamp(pid.integral, doc.pid.min_integral, doc.pid.max_integral)
      : doc.pid.starting_integral_term
    const pidDirection = pid ? pid.pidDirection : onDirection(doc)
    const rt: Runtime = {
      boundAt: t,
      waitingSince: waiting ? waiting.waitingSince : t,
      lastControl: pid ? pid.lastControl : null,
      due: true,
      prevError: pid ? pid.prevError : null,
      integral,
      latch: heatAllowed(doc) && latched('heat') ? 'heating' : coolAllowed(doc) && latched('cool') ? 'cooling' : 'idle',
      action: keptAction(),
      fault: 'none',
      heatDuty: prev ? prev.rt.heatDuty : 0,
      coolDuty: prev ? prev.rt.coolDuty : 0,
      heatWait: keptWait('heat'),
      coolWait: keptWait('cool'),
      pidDirection,
      terms: pid
        ? { ...pid.terms, integral: round(integral, 3) }
        : {
            direction: pidDirection,
            error: null,
            proportional: null,
            integral: null,
            derivative: null,
            in_deadband: false
          }
    }
    rt.fault = faultOf(doc, rt, t)
    rt.action = relayAction(doc, rt)
    running.set(doc.id, rt)
    waitReasons.delete(doc.id)
  }

  // Silence counts from the start while the sensor has given no reading.
  function faultOf(doc: ControllerDocument, rt: Runtime, t: number): ClimateHubFault {
    const temp = readingOf(doc.sensor_id)
    if (temp === null) return t - rt.waitingSince > doc.safety.sensor_timeout_s * 1000 ? 'sensor_stale' : 'none'
    return temp > doc.safety.max_temperature ? 'overtemp' : 'none'
  }

  // ControllerRuntime::relay_action_(): what the relays do, not what the thermostat wants;
  // off only on a fault or in mode off.
  function relayAction(doc: ControllerDocument, rt: Runtime): ClimateHubAction {
    if (rt.fault !== 'none' || doc.mode === 'off') return 'off'
    if (readingOf(doc.sensor_id) === null) return 'idle'
    const heating = shows(doc, rt, 'heat')
    const cooling = shows(doc, rt, 'cool')
    // Both closed only while one closed from elsewhere waits to be put back, which this mock never
    // does: the one it drives now wins.
    if (heating && (!cooling || rt.heatDuty > 0)) return 'heating'
    return cooling ? 'cooling' : 'idle'
  }

  // A relay shows its direction while it is closed; a PID's also between two pulses once the
  // first has closed it, while the mode drives that way.
  function shows(doc: ControllerDocument, rt: Runtime, dir: 'heat' | 'cool'): boolean {
    const relayId = doc[dir].relay_id
    if (!relayId) return false
    if (relayOf(relayId).on) return true
    const drives = dir === 'heat' ? heatAllowed(doc) : coolAllowed(doc)
    const duty = dir === 'heat' ? rt.heatDuty : rt.coolDuty
    return doc.kind === 'pid' && drives && duty > 0 && rt.action === (dir === 'heat' ? 'heating' : 'cooling')
  }

  // RelayClaim::request(): the relay goes where `want` says once its min_on or min_off since
  // its last move is over; until then it stays, and the answer says which holds it.
  function request(out: OutputConfig, want: boolean, t: number): RelayWait {
    const relay = relayOf(out.relay_id)
    if (relay.on === want) return 'none'
    if (t - relay.since < (relay.on ? out.min_on_s : out.min_off_s) * 1000) return relay.on ? 'min_on' : 'min_off'
    relay.on = want
    relay.since = t
    return 'none'
  }

  // RelayClaim::force_off(): open at once, whatever min_on says; an open relay does not move.
  function forceOff(relayId: string, t: number) {
    const relay = relayOf(relayId)
    if (!relay.on) return
    relay.on = false
    relay.since = t
  }

  // ControllerRuntime::tick() after its pass: the relays move as their minimums let them, the
  // opening first, and one closes only once the other has opened; the action follows them.
  function drive(doc: ControllerDocument, rt: Runtime, t: number) {
    const holdsOpen = readingOf(doc.sensor_id) === null || rt.fault !== 'none' || doc.mode === 'off'
    if (holdsOpen) {
      if (doc.heat.relay_id) forceOff(doc.heat.relay_id, t)
      if (doc.cool.relay_id) forceOff(doc.cool.relay_id, t)
      rt.heatWait = 'none'
      rt.coolWait = 'none'
      rt.action = relayAction(doc, rt)
      return
    }
    const wantHeat = !!doc.heat.relay_id && pwm(doc, rt, rt.heatDuty, doc.heat.period_s, t)
    const wantCool = !!doc.cool.relay_id && pwm(doc, rt, rt.coolDuty, doc.cool.period_s, t)
    if (doc.heat.relay_id && !wantHeat) rt.heatWait = request(doc.heat, false, t)
    if (doc.cool.relay_id && !wantCool) rt.coolWait = request(doc.cool, false, t)
    // A heater and a cooler never run against each other; one closed already stays.
    const held = (own: OutputConfig, other: OutputConfig) =>
      !relayOf(own.relay_id).on && !!other.relay_id && relayOf(other.relay_id).on
    if (wantHeat) rt.heatWait = held(doc.heat, doc.cool) ? 'other_relay' : request(doc.heat, true, t)
    if (wantCool) rt.coolWait = held(doc.cool, doc.heat) ? 'other_relay' : request(doc.cool, true, t)
    rt.action = relayAction(doc, rt)
  }

  // ClimateHub::let_go_(): a relay no running thermostat holds any more opens at once.
  function releaseUnheld() {
    const held = heldRelays()
    for (const relayId of relays.keys()) if (!held.has(relayId)) forceOff(relayId, simulatedTo)
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

  function reservedRefusal(relay: string, holder: ControllerDocument): MockResult {
    return fail(409, `"${switchName(relay)}" is reserved by "${holder.name}", which is enabled and waits to start`)
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
    const reserved = reserverOf(doc)
    return reserved ? reservedRefusal(reserved.relay, reserved.holder) : undefined
  }

  // Why an enabled `doc` does not start, worded and ordered as ClimateHub::start_ finds it; ''
  // when it does. A Save or an enable has refused the unit and a held relay already; a boot has not.
  function startError(doc: ControllerDocument): string {
    const sensor = seedSensors.find((s) => s.object_id === doc.sensor_id)
    if (!sensor) return `sensor '${doc.sensor_id}' not found`
    if (sensor.unit !== CELSIUS) {
      return `sensor '${doc.sensor_id}' reports ${sensor.unit === '' ? 'no unit' : sensor.unit}, not ${CELSIUS}`
    }
    if (!entityFree()) return NO_FREE_ENTITY
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

  // ClimateHub::append_set_: the number after the highest the file held, the clock's time, and a
  // name for one given none. Answers a copy, as the device answers what it wrote.
  function appendSet(id: string, fields: GainSetFields, name: string, label: string, t: number): GainSet {
    let file = gainFiles.get(id)
    if (!file) gainFiles.set(id, (file = { last: 0, sets: [] }))
    const n = ++file.last
    const clock = clockAt(t)
    const { direction, source, rule, ku, pu, kp, ki, kd, flags, from } = fields
    // serialize_gain_set()'s key order.
    const set: GainSet = {
      n,
      name: name || defaultSetName(clock, direction, label, n),
      direction,
      source,
      rule,
      ku,
      pu,
      kp,
      ki,
      kd,
      flags: [...flags],
      time: clock ? Math.floor(t / 1000) : null,
      from
    }
    file.sets.push(set)
    return structuredClone(set)
  }

  const findSet = (id: string, n: number) => gainFiles.get(id)?.sets.find((s) => s.n === n)

  // ClimateHub::may_apply_: what a set's gains cannot go into the thermostat for.
  function applyRefusal(doc: ControllerDocument, direction: RelayDirection): MockResult | undefined {
    if (newer(doc)) return fail(409, NEWER_FILE)
    if (!doc[direction].relay_id) return noRelay(direction)
    return activeRun(doc) ? fail(409, CALIBRATION_RUNNING) : undefined
  }

  // ClimateHub::apply_set_: the gains held in range, the revision on, and a running thermostat
  // restarted as after a Save, its PID keeping what it learnt.
  function applySet(doc: ControllerDocument, set: GainSet): { revision: number; persisted: boolean } {
    const { kp, ki, kd } = set
    doc.pid[set.direction] = { kp: clampParam('kp', kp), ki: clampParam('ki', ki), kd: clampParam('kd', kd) }
    // A form read before now would write the old gains back.
    doc.revision++
    const rt = running.get(doc.id)
    if (rt) bind(doc, { doc, rt })
    return { revision: doc.revision, persisted: true }
  }

  function listGainSets(search: URLSearchParams): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    let after = 0
    let limit = GAIN_SET_PAGE_MAX
    if (search.has('after')) {
      const value = countParam(search, 'after', 0, COUNT_MAX)
      if (typeof value !== 'number') return value
      after = value
    }
    if (search.has('limit')) {
      const value = countParam(search, 'limit', 1, GAIN_SET_PAGE_MAX)
      if (typeof value !== 'number') return value
      limit = value
    }
    if (!find(id)) return fail(404, 'Thermostat not found')
    const later = (gainFiles.get(id)?.sets ?? []).filter((s) => s.n > after)
    const sets = structuredClone(later.slice(0, limit))
    return { status: 200, body: { success: true, sets, more: later.length > limit } }
  }

  function addGainSet(search: URLSearchParams, body: string, t: number): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const rawDirection = search.get('direction')
    if (rawDirection !== null && rawDirection !== 'heat' && rawDirection !== 'cool') {
      return fail(400, 'Invalid direction parameter')
    }
    const name = setNameParam(body, false)
    if (typeof name !== 'string') return name
    // From here on the hub's: a blank name is none.
    const trimmed = trimName(name)
    const badName = trimmed && setNameError(trimmed)
    if (badName) return fail(400, badName)
    const doc = find(id)
    if (!doc) return fail(404, 'Thermostat not found')
    const direction = rawDirection ?? onDirection(doc)
    if (!doc[direction].relay_id) return noRelay(direction)
    const gains = doc.pid[direction]
    const set = appendSet(
      id,
      {
        direction,
        source: 'manual',
        rule: null,
        ku: null,
        pu: null,
        kp: asStored(gains.kp),
        ki: asStored(gains.ki),
        kd: asStored(gains.kd),
        flags: [],
        from: null
      },
      trimmed,
      MANUAL_LABEL,
      t
    )
    return ok('Gain set saved', { set })
  }

  function renameGainSet(search: URLSearchParams, body: string): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const n = countParam(search, 'set', 1, COUNT_MAX)
    if (typeof n !== 'number') return n
    const name = setNameParam(body, true)
    if (typeof name !== 'string') return name
    const trimmed = trimName(name)
    const badName = setNameError(trimmed)
    if (badName) return fail(400, badName)
    if (!find(id)) return fail(404, 'Thermostat not found')
    const set = findSet(id, n)
    if (!set) return fail(404, GAIN_SET_NOT_FOUND)
    set.name = trimmed
    const saved = runs.get(id)?.saved
    if (saved?.n === n) saved.name = trimmed
    return ok('Gain set renamed', { set: structuredClone(set) })
  }

  function deleteGainSet(search: URLSearchParams): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const n = countParam(search, 'set', 1, COUNT_MAX)
    if (typeof n !== 'number') return n
    if (!find(id)) return fail(404, 'Thermostat not found')
    const sets = gainFiles.get(id)?.sets ?? []
    const i = sets.findIndex((s) => s.n === n)
    if (i < 0) return fail(404, GAIN_SET_NOT_FOUND)
    sets.splice(i, 1)
    const run = runs.get(id)
    if (run?.saved?.n === n) run.saved = null
    return ok('Gain set deleted')
  }

  function applyGainSet(search: URLSearchParams): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const n = countParam(search, 'set', 1, COUNT_MAX)
    if (typeof n !== 'number') return n
    const doc = find(id)
    if (!doc) return fail(404, 'Thermostat not found')
    const set = findSet(id, n)
    if (!set) return fail(404, GAIN_SET_NOT_FOUND)
    return applyRefusal(doc, set.direction) ?? ok('Gain set applied', applySet(doc, set))
  }

  function deriveGainSet(search: URLSearchParams, body: string, t: number): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const n = countParam(search, 'set', 1, COUNT_MAX)
    if (typeof n !== 'number') return n
    const rawRule = search.get('rule')
    if (rawRule === null) return fail(400, 'Missing rule parameter')
    const rule = AUTOTUNE_RULES.find((r) => r === rawRule)
    if (!rule) return fail(400, 'Invalid rule parameter')
    const rawApply = search.get('apply')
    if (rawApply !== null && rawApply !== 'true' && rawApply !== 'false') return fail(400, 'Invalid apply parameter')
    const apply = rawApply === 'true'
    const name = setNameParam(body, false)
    if (typeof name !== 'string') return name
    const trimmed = trimName(name)
    const badName = trimmed && setNameError(trimmed)
    if (badName) return fail(400, badName)
    const doc = find(id)
    if (!doc) return fail(404, 'Thermostat not found')
    const parent = findSet(id, n)
    if (!parent) return fail(404, GAIN_SET_NOT_FOUND)
    if (parent.ku === null || parent.pu === null) return fail(409, NO_MEASUREMENT)
    // Nothing is added for a set that could not be applied.
    const refusal = apply ? applyRefusal(doc, parent.direction) : undefined
    if (refusal) return refusal
    const { gains, clamped } = gainsForRule(parent.ku, parent.pu, rule)
    const set = appendSet(
      id,
      {
        direction: parent.direction,
        source: 'derived',
        rule,
        ku: parent.ku,
        pu: parent.pu,
        kp: asStored(gains.kp),
        ki: asStored(gains.ki),
        kd: asStored(gains.kd),
        // The measurement's warnings hold for any rule; clamping is the new rule's.
        flags: [
          ...(['asymmetric', 'uneven'] as const).filter((flag) => parent.flags.includes(flag)),
          ...(clamped ? (['clamped'] as const) : [])
        ],
        from: parent.n
      },
      trimmed,
      RULE_LABEL[rule],
      t
    )
    if (!apply) return ok('Gain set derived', { set })
    return ok('Gain set derived and applied', { set, ...applySet(doc, set) })
  }

  // The calibration running on `doc`, if one does.
  function activeRun(doc: ControllerDocument): Run | undefined {
    const run = runs.get(doc.id)
    return run && run.state === 'running' && running.has(doc.id) ? run : undefined
  }

  // Feeds the reading to the tuner and drives the run's relay from it; the PID starts over clean
  // once it has its gains. False when no calibration runs.
  function calibrate(doc: ControllerDocument, rt: Runtime, temp: number, t: number): boolean {
    const run = activeRun(doc)
    if (!run) return false
    const limit = t - run.startedAt >= AUTOTUNE_MAX_MS ? 'timeout' : t - run.lastSwitch >= AUTOTUNE_STALL_MS ? 'no_switch' : ''
    if (limit) {
      endRun(doc, rt, limit, t)
      return false
    }
    feedRun(doc, rt, run, temp, t)
    if (run.tuner.noisy) {
      endRun(doc, rt, 'noisy', t)
      return false
    }
    if (!run.tuner.finished) return true
    const { ku, pu } = run.tuner
    const { gains: held, clamped } = gainsForRule(ku, pu, run.rule)
    const gains = { kp: asStored(held.kp), ki: asStored(held.ki), kd: asStored(held.kd) }
    // Only its relay's: the other one keeps what it had.
    doc.pid[run.direction] = { ...gains }
    // A form read before now would write the old gains back.
    doc.revision++
    run.new = gains
    run.flags = [
      ...(run.tuner.symmetrical() ? [] : (['asymmetric'] as const)),
      ...(run.tuner.swingRatio() < AUTOTUNE_EVEN_RATIO ? (['uneven'] as const) : []),
      ...(clamped ? (['clamped'] as const) : [])
    ]
    run.state = 'succeeded'
    run.endedAt = t
    restartPid(doc, rt)
    // ClimateHub::save_calibration_: the result is kept as a set too, named as one given none.
    const set = appendSet(
      doc.id,
      {
        direction: run.direction,
        source: 'calibration',
        rule: run.rule,
        ku: measured(ku),
        pu: measured(pu),
        ...gains,
        flags: [...run.flags],
        from: null
      },
      '',
      RULE_LABEL[run.rule],
      t
    )
    run.saved = { n: set.n, name: set.name }
    return false
  }

  function feedRun(doc: ControllerDocument, rt: Runtime, run: Run, temp: number, t: number) {
    const switches = run.tuner.switches
    const output = run.tuner.update(run.setpoint, temp, t)
    if (run.tuner.switches !== switches) run.lastSwitch = t
    if (run.tuner.finished) return
    const on = run.direction === 'heat' ? output > 0 : output < 0
    rt.heatDuty = run.direction === 'heat' && on ? 1 : 0
    rt.coolDuty = run.direction === 'cool' && on ? 1 : 0
    rt.action = relayAction(doc, rt)
  }

  // ControllerRuntime::end_autotune: nothing the PID held carries on, on the mode's relay.
  function restartPid(doc: ControllerDocument, rt: Runtime) {
    rt.pidDirection = onDirection(doc)
    rt.integral = doc.pid.starting_integral_term
    rt.prevError = null
    rt.lastControl = null
    rt.heatDuty = 0
    rt.coolDuty = 0
    rt.due = true
  }

  // Ends the calibration `doc` runs, if one does, for `reason`.
  function endRun(doc: ControllerDocument, rt: Runtime | undefined, reason: AutotuneReason, t: number) {
    const run = runs.get(doc.id)
    if (!run || run.state !== 'running') return
    run.state = 'failed'
    run.reason = reason
    run.endedAt = t
    if (rt) restartPid(doc, rt)
  }

  function runStatus(run: Run, t: number): AutotuneStatus {
    const live = run.state === 'running' && run.tuner.relay !== 'init'
    const positive = run.tuner.relay === 'positive'
    const found = run.state === 'succeeded'
    return {
      state: run.state,
      reason: run.reason,
      direction: run.direction,
      rule: run.rule,
      setpoint: run.setpoint,
      phase: live ? (positive === (run.direction === 'heat') ? 'on' : 'off') : null,
      aim: live ? run.setpoint + (positive ? AUTOTUNE_NOISEBAND : -AUTOTUNE_NOISEBAND) : null,
      swings: run.tuner.switches,
      elapsed_s: Math.floor(((run.state === 'running' ? t : run.endedAt) - run.startedAt) / 1000),
      extremes: run.tuner.extremes.map((e) => ({
        at_s: Math.floor((e.at - run.startedAt) / 1000),
        temperature: round(run.setpoint - e.error, 3)
      })),
      ku: found ? run.tuner.ku : null,
      pu: found ? run.tuner.pu : null,
      flags: [...run.flags],
      old: { ...run.old },
      new: run.new ? { ...run.new } : null,
      persisted: true,
      set: run.saved ? { ...run.saved } : null
    }
  }

  // ClimateHub::start_autotune and cancel_autotune, refused in the device's order.
  function autotune(search: URLSearchParams, t: number): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const value = search.get('value')
    if (value === null) return fail(400, 'Missing value parameter')
    if (value !== 'true' && value !== 'false') return fail(400, 'Invalid value parameter')
    const rawDirection = search.get('direction')
    if (rawDirection !== null && rawDirection !== 'heat' && rawDirection !== 'cool') {
      return fail(400, 'Invalid direction parameter')
    }
    const rawRule = search.get('rule')
    const rule = rawRule === null ? 'zn_pi' : AUTOTUNE_RULES.find((r) => r === rawRule)
    if (!rule) return fail(400, 'Invalid rule parameter')
    const doc = find(id)
    if (!doc) return fail(404, 'Thermostat not found')
    const rt = running.get(id)
    if (value === 'false') {
      if (!activeRun(doc)) return fail(409, 'No calibration is running')
      endRun(doc, rt, 'cancelled', t)
      return ok('Calibration cancelled')
    }
    if (doc.kind !== 'pid') return fail(409, 'Only a PID thermostat can be calibrated')
    if (!rt) return fail(409, 'The thermostat is not running')
    if (newer(doc)) return fail(409, NEWER_FILE)
    if (activeRun(doc)) return fail(409, 'A calibration is already running')
    if (doc.mode === 'off') return fail(409, 'The thermostat is off: set it to heat or cool first')
    // The mode's relay: one a client names besides has to be that one.
    const direction: AutotuneDirection = doc.mode
    if (rawDirection !== null && rawDirection !== direction) {
      return fail(
        400,
        `direction '${rawDirection}' does not match mode ${doc.mode}: a calibration swings the relay of the current mode`
      )
    }
    if (rt.fault !== 'none') return fail(409, `The thermostat reports ${rt.fault}; calibrate it once that clears`)
    const run: Run = {
      state: 'running',
      reason: '',
      direction,
      rule,
      setpoint: doc.setpoint,
      startedAt: t,
      endedAt: t,
      lastSwitch: t,
      tuner: new Tuner(direction === 'cool'),
      old: { ...doc.pid[direction] },
      new: null,
      flags: [],
      saved: null
    }
    runs.set(id, run)
    const temp = readingOf(doc.sensor_id)
    if (temp === null) {
      rt.heatDuty = 0
      rt.coolDuty = 0
    } else {
      feedRun(doc, rt, run, temp, t)
    }
    return ok('Calibration started')
  }

  function runControl(doc: ControllerDocument, rt: Runtime, t: number) {
    const temp = readingOf(doc.sensor_id)
    const faulted = rt.fault !== 'none'
    rt.fault = faultOf(doc, rt, t)
    if (rt.fault !== 'none') endRun(doc, rt, rt.fault, t)
    // The fault zeroed the duties: the next pass is now, not an update_interval_s later.
    if (faulted && rt.fault === 'none') rt.due = true
    // Waiting for a first reading is no fault, but nothing to act on either.
    if (temp === null || rt.fault !== 'none' || doc.mode === 'off') {
      rt.heatDuty = 0
      rt.coolDuty = 0
      return
    }
    if (calibrate(doc, rt, temp, t)) return
    if (!rt.due && rt.lastControl !== null && t - rt.lastControl < doc.update_interval_s * 1000) return
    // What one relay learnt means nothing to the other: a pass for it starts the PID over.
    const direction: RelayDirection = doc.mode === 'cool' ? 'cool' : 'heat'
    if (doc.kind === 'pid' && direction !== rt.pidDirection) {
      rt.pidDirection = direction
      rt.integral = doc.pid.starting_integral_term
      rt.prevError = null
      rt.lastControl = null
    }
    const dt = rt.lastControl === null ? 0 : (t - rt.lastControl) / 1000
    rt.lastControl = t
    rt.due = false

    if (doc.kind === 'pid') {
      const pid = doc.pid
      const gains = pid[rt.pidDirection]
      const e = doc.setpoint - temp
      const inDeadband = pid.deadband_threshold_low < -e && -e < pid.deadband_threshold_high
      const p = gains.kp * e * (inDeadband ? pid.deadband_kp_multiplier : 1)
      const ki = gains.ki * (inDeadband ? pid.deadband_ki_multiplier : 1)
      rt.integral = clamp(rt.integral + e * dt * ki, pid.min_integral, pid.max_integral)
      const kd = gains.kd * (inDeadband ? pid.deadband_kd_multiplier : 1)
      const d = dt > 0 && rt.prevError !== null ? (kd * (e - rt.prevError)) / dt : 0
      rt.prevError = e
      const out = p + rt.integral + d
      rt.heatDuty = heatAllowed(doc) ? clamp(out, 0, 1) : 0
      rt.coolDuty = coolAllowed(doc) ? clamp(-out, 0, 1) : 0
      rt.terms = {
        direction: rt.pidDirection,
        error: round(e, 3),
        proportional: round(p, 3),
        integral: round(rt.integral, 3),
        derivative: round(d, 3),
        in_deadband: inDeadband
      }
      return
    }

    // Bang-bang: switch at the band's ends, hold the latch in between.
    const low = doc.setpoint - doc.bang_bang.below
    const high = doc.setpoint + doc.bang_bang.above
    if (temp < low) rt.latch = heatAllowed(doc) ? 'heating' : 'idle'
    else if (temp > high) rt.latch = coolAllowed(doc) ? 'cooling' : 'idle'
    rt.heatDuty = rt.latch === 'heating' ? 1 : 0
    rt.coolDuty = rt.latch === 'cooling' ? 1 : 0
  }

  function advance(to: number) {
    simulatedTo = Math.max(simulatedTo, to - MAX_CATCH_UP_MS)
    while (to - simulatedTo >= STEP_MS) {
      simulatedTo += STEP_MS
      for (const doc of docs) {
        const rt = running.get(doc.id)
        if (!rt) continue
        runControl(doc, rt, simulatedTo)
        drive(doc, rt, simulatedTo)
      }
      for (const [sensorId, room] of rooms) {
        if (room.temp === null) continue
        let power = 0
        for (const doc of docs) {
          if (!running.has(doc.id) || doc.sensor_id !== sensorId) continue
          // The relays as they are, so a minimum that holds one shows in the room too.
          if (doc.heat.relay_id && relayOf(doc.heat.relay_id).on) power += 1
          if (doc.cool.relay_id && relayOf(doc.cool.relay_id).on) power -= 1
        }
        room.temp += HEAT_PER_STEP * power - (room.temp - room.ambient) * LOSS_PER_STEP
      }
    }
  }

  // Where the thermostat wants the relay. A PID output is a slow PWM: closed for `duty` of
  // every period.
  function pwm(doc: ControllerDocument, rt: Runtime, duty: number, periodS: number, t: number): boolean {
    if (duty <= 0) return false
    if (doc.kind === 'bang_bang' || duty >= 1) return true
    const period = periodS * 1000
    return (t - rt.boundAt) % period < duty * period
  }

  // The preset a climate call names: upstream maps a built-in name in any case to its
  // preset before it looks at the custom ones, which match exactly.
  function presetNamed(doc: ControllerDocument, name: string): PresetConfig | undefined {
    const standard = standardPreset(name)
    return doc.presets.find((p) =>
      standard ? standardPreset(p.name) === standard : standardPreset(p.name) === null && p.name === name
    )
  }

  // ControllerRuntime::control(): the preset, then a supported mode and a target held in
  // range over it, and a control pass at once. A target or a mode set by hand keeps the
  // label. Nothing else about the running thermostat changes.
  function applyControl(
    doc: ControllerDocument,
    rt: Runtime,
    call: Omit<ClimateControlCall, 'preset'> & { preset?: PresetConfig }
  ) {
    const previousMode = doc.mode
    const previousTarget = doc.setpoint
    if (call.preset) pickPreset(doc, call.preset)
    const mode = MODES.find((m) => m === call.mode)
    if (mode && mode !== doc.mode && modeSupported(doc, mode)) setMode(doc, mode)
    if (typeof call.target === 'number' && !Number.isNaN(call.target)) {
      doc.setpoint = clamp(call.target, doc.visual.min_temperature, doc.visual.max_temperature)
    }
    // Carried across a mode change, the latch would keep the heater running in COOL.
    if (doc.mode !== previousMode) rt.latch = 'idle'
    if (doc.mode !== previousMode) endRun(doc, rt, 'mode_changed', simulatedTo)
    else if (doc.setpoint !== previousTarget) endRun(doc, rt, 'target_changed', simulatedTo)
    // Published with the mode it replaced, the action would say "off" in HEAT until the next pass.
    rt.action = relayAction(doc, rt)
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
      ...activePresetOf(doc),
      min_temperature: doc.visual.min_temperature,
      max_temperature: doc.visual.max_temperature,
      step: doc.visual.step,
      heat_duty: rt ? round(rt.heatDuty, 3) : 0,
      cool_duty: rt ? round(rt.coolDuty, 3) : 0,
      heat_relay_on: !!rt && !!doc.heat.relay_id && relayOf(doc.heat.relay_id).on,
      cool_relay_on: !!rt && !!doc.cool.relay_id && relayOf(doc.cool.relay_id).on,
      heat_relay_wait: rt ? rt.heatWait : 'none',
      cool_relay_wait: rt ? rt.coolWait : 'none'
    }
    if (doc.kind === 'bang_bang') {
      status.switch_low = round(doc.setpoint - doc.bang_bang.below, 3)
      status.switch_high = round(doc.setpoint + doc.bang_bang.above, 3)
    }
    if (rt && doc.kind === 'pid') status.pid = { ...rt.terms, direction: rt.pidDirection }
    const run = runs.get(doc.id)
    if (run) status.autotune = runStatus(run, t)
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
      revision: doc.revision,
      waiting: waitingOf(doc),
      ...activePresetOf(doc)
    }
  }

  // Kept while the thermostat is stopped, as its target is.
  function activePresetOf(doc: ControllerDocument): { active_preset: string; active_preset_name: string } {
    const active = findPreset(doc, doc.active_preset)
    return { active_preset: active?.key ?? '', active_preset_name: active?.name ?? '' }
  }

  // ClimateHub::update's presets: a key the thermostat gave out stays with its preset,
  // any other is made again from the name; the active preset is the thermostat's state,
  // not the body's, and takes its new values at once.
  function keepPresetState(stored: ControllerDocument, doc: ControllerDocument) {
    for (const preset of doc.presets) if (!findPreset(stored, preset.key)) preset.key = ''
    assignPresetKeys(doc)
    const now = findPreset(doc, stored.active_preset)
    doc.active_preset = now ? stored.active_preset : ''
    if (!now) return
    const was = findPreset(stored, stored.active_preset)
    if (!was || now.setpoint !== was.setpoint || now.mode !== was.mode) pickPreset(doc, now)
  }

  // The body as /save and /import read it, up to the document's own rules, its name's last.
  // `revision` is the one the body carries, if a count, null otherwise.
  function readDocument(
    body: string,
    requireId: boolean
  ): { doc: ControllerDocument; revision: number | null } | MockResult {
    if (new TextEncoder().encode(body).length > CONFIG_MAX_BYTES) return fail(413, 'Request body over 8 KiB')
    if (!body) return fail(400, 'Empty request body')
    const parsed = readJson(body)
    if ('error' in parsed) return fail(400, `JSON parse error: ${parsed.error}`)
    const decoded = decodeDocument(parsed.value, requireId)
    if ('error' in decoded) return fail(400, decoded.error)
    const doc = decoded.doc
    const badName = nameError(doc.name, doc.id, [])
    if (badName) return fail(400, badName)
    doc.name = trimName(doc.name)
    return { doc, revision: readRevision(objectOf(parsed.value)?.revision) }
  }

  // What /save and /import refuse once the document is sound, in the device's order: the
  // limit, the name, then the unit and the relays of an enabled one.
  function refusalOf(doc: ControllerDocument, stored: ControllerDocument | undefined): MockResult | undefined {
    if (!stored && docs.length >= maxControllers) {
      return fail(507, `This device allows ${maxControllers} thermostats; delete one to add another`)
    }
    const taken = nameError(doc.name, doc.id, [...docs, ...yamlClimates])
    if (taken) return fail(409, taken)
    return doc.enabled ? saveRefusal(doc) : undefined
  }

  // The change itself: `doc` takes `stored`'s place, or joins the list, and runs if it can.
  function store(doc: ControllerDocument, stored: ControllerDocument | undefined, message: string): MockResult {
    doc.version = CONFIG_VERSION
    const before = snapshot()
    const rt = running.get(doc.id)
    if (stored) endRun(stored, rt, 'saved', simulatedTo)
    running.delete(doc.id)
    const i = docs.findIndex((d) => d.id === doc.id)
    if (i >= 0) docs[i] = doc
    else docs.push(doc)
    docs.sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0))
    if (!doc.enabled) {
      waitReasons.delete(doc.id)
      return ok(message + startWaiters(before, doc.id), { id: doc.id })
    }
    // Stored all the same, and a running one stopped: it waits for what it names.
    const warning = start(doc, stored && rt ? { doc: stored, rt } : undefined)
    // Who started on a relay it let go, then its own warning, last.
    const text = message + startWaiters(before, doc.id)
    return warning ? ok(`${text}; ${warning}`, { id: doc.id, warning }) : ok(text, { id: doc.id })
  }

  function save(body: string): MockResult {
    const read = readDocument(body, false)
    if (!('doc' in read)) return read
    const doc = read.doc

    // From here on the device is on its loop task, where the documents live.
    const updating = doc.id !== ''
    const stored = updating ? find(doc.id) : undefined
    if (updating && !stored) return fail(404, 'Thermostat not found')
    if (stored && newer(stored)) return fail(409, NEWER_FILE)
    // A form read before a calibration wrote new gains.
    if (stored && read.revision !== null && read.revision !== stored.revision) return fail(409, STALE_DOCUMENT)
    // The thermostat's state, not the body's: an update keeps the stored one's (as stored->on_mode()),
    // a create starts from its own mode.
    doc.last_on_mode = onMode(doc, stored ? onMode(stored, stored.last_on_mode) : undefined)
    if (stored) {
      keepPresetState(stored, doc)
      doc.revision = stored.revision
    } else {
      // The keys are the device's to give, as the id is.
      doc.id = uniqueId(slugify(doc.name), docs.map((d) => d.id))
      for (const preset of doc.presets) preset.key = ''
      assignPresetKeys(doc)
      doc.active_preset = ''
      doc.revision = 0
    }
    const refusal = refusalOf(doc, stored)
    if (refusal) return refusal
    // ClimateHub::create: sets left under this id are not the new thermostat's; an import keeps them.
    if (!stored) gainFiles.delete(doc.id)
    return store(doc, stored, updating ? 'Thermostat updated' : 'Thermostat created')
  }

  // ClimateHub::restore: a Save under the body's id, its presets' keys and active preset as
  // they come, since rules name them.
  function importDocument(body: string): MockResult {
    const read = readDocument(body, true)
    if (!('doc' in read)) return read
    const doc = read.doc
    // A boot refuses such a file, so the thermostat would be gone after the next one.
    if (RESERVED_IDS.includes(doc.id)) return fail(400, `id '${doc.id}' is reserved`)
    const stored = find(doc.id)
    if (stored && newer(stored)) return fail(409, NEWER_FILE)
    // Never the backup's: a form read before the import would otherwise Save over it.
    doc.revision = stored ? stored.revision + 1 : 0
    const refusal = refusalOf(doc, stored)
    if (refusal) return refusal
    return store(doc, stored, stored ? 'Thermostat replaced' : 'Thermostat created')
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

    const before = snapshot()
    if (value === 'false') {
      // Written only when the flag changes, and never into a newer firmware's file.
      const persisted = !(doc.enabled && newer(doc))
      doc.enabled = false
      endRun(doc, running.get(id), 'stopped', simulatedTo)
      running.delete(id)
      waitReasons.delete(id)
      return ok('Thermostat disabled' + startWaiters(before, id), { persisted })
    }
    if (running.has(id)) return ok('Thermostat enabled', { persisted: true })
    // Refused as a Save would be, but for a relay another thermostat holds or waits for the
    // call may take over.
    const refused = saveRefusal(doc)
    if (refused && (refused.status !== 409 || takeOverRaw !== 'true')) return refused
    // The running holders, by relay, then the enabled ones that wait for a relay, by id.
    const holders: ControllerDocument[] = []
    for (const relay of relaysOf(doc)) {
      const holder = holderOf(relay, id)
      if (holder && !holders.includes(holder)) holders.push(holder)
    }
    const waiters = docs.filter(
      (d) => d.id !== id && d.enabled && !running.has(d.id) && relaysOf(d).some((r) => relaysOf(doc).includes(r))
    )
    // A take-over stops the others, so only for a thermostat that runs in their place.
    const refusal = refused ? entityRefusal(doc) : undefined
    if (refusal) return refusal
    // Waiters free no climate entity, a running holder does.
    if (refused && !holders.length && !entityFree()) return fail(409, 'No free climate entity to run it in')
    let persisted = true
    for (const other of [...holders, ...waiters]) {
      // A newer firmware's file cannot record the take-over, so the others' files keep their flag too.
      persisted = persisted && !newer(other) && !newer(doc)
      other.enabled = false
      endRun(other, running.get(other.id), 'taken_over', simulatedTo)
      running.delete(other.id)
      waitReasons.delete(other.id)
    }
    if (!doc.enabled) persisted = persisted && !newer(doc)
    doc.enabled = true
    // A sensor or relay that is not there is waited for, as on a Save.
    const warning = start(doc)
    let message = 'Thermostat enabled'
    if (holders.length || waiters.length) message += `; ${quoted([...holders, ...waiters].map((h) => h.name))} stopped`
    // After it: the relays the holders drove alone go to whoever waits for them.
    message += startWaiters(before, id)
    return warning ? ok(`${message}; ${warning}`, { persisted, warning }) : ok(message, { persisted })
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

  // ClimateHub::apply_preset: through the entity when it runs, into the document when not.
  function applyPreset(search: URLSearchParams): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const key = search.get('key')
    if (key === null) return fail(400, 'Missing key parameter')
    if (slugify(key) !== key) return fail(400, 'Invalid key parameter')
    const doc = find(id)
    if (!doc) return fail(404, 'Thermostat not found')
    const picked = findPreset(doc, key)
    if (!picked) return fail(404, 'Preset not found')
    const before = [doc.setpoint, doc.mode, doc.active_preset].join()
    const rt = running.get(id)
    if (rt) applyControl(doc, rt, { preset: picked })
    else pickPreset(doc, picked)
    // A newer firmware's file is never written, so a pick that moved something lasts until a reboot.
    const changed = [doc.setpoint, doc.mode, doc.active_preset].join() !== before
    return ok('Preset applied', { persisted: !(changed && newer(doc)) })
  }

  function handle(method: string, endpoint: string, search: URLSearchParams, body: string): MockResult {
    const t = now()
    advance(t)
    const name = endpoint.startsWith('/') ? endpoint.slice(1) : endpoint
    const mutating = ROUTES.get(name)
    if (mutating === undefined) return fail(404, 'Unknown endpoint')
    const allow = mutating ? 'POST' : 'GET'
    if (method.toUpperCase() !== allow) return { ...fail(405, 'Method not allowed'), headers: { Allow: allow } }
    const result = route(name, search, body, t)
    // A stop, a removal or a Save onto other relays lets go of what it held.
    if (mutating) releaseUnheld()
    return result
  }

  function route(name: string, search: URLSearchParams, body: string, t: number): MockResult {
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
        return doc ? { status: 200, body: wireDocument(doc) } : fail(404, 'Thermostat not found')
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

      case 'import':
        return importDocument(body)

      case 'delete': {
        const id = idParam(search)
        if (typeof id !== 'string') return id
        const i = docs.findIndex((d) => d.id === id)
        if (i < 0) return fail(404, 'Thermostat not found')
        const before = snapshot()
        running.delete(id)
        waitReasons.delete(id)
        runs.delete(id)
        // Its sets go with it, their numbers too.
        gainFiles.delete(id)
        docs.splice(i, 1)
        return ok('Thermostat deleted' + startWaiters(before, ''), { persisted: true })
      }

      case 'enable':
        return enable(search)

      case 'setpoint':
        return setpoint(search)

      case 'preset':
        return applyPreset(search)

      case 'autotune':
        return autotune(search, t)

      case 'gains':
        return listGainSets(search)

      case 'gains/add':
        return addGainSet(search, body, t)

      case 'gains/rename':
        return renameGainSet(search, body)

      case 'gains/delete':
        return deleteGainSet(search)

      case 'gains/apply':
        return applyGainSet(search)

      case 'gains/derive':
        return deriveGainSet(search, body, t)

      default:
        return fail(404, 'Unknown endpoint')
    }
  }

  function control(id: string, call: ClimateControlCall): boolean {
    advance(now())
    const doc = find(id)
    const rt = running.get(id)
    if (!doc || !rt) return false
    applyControl(doc, rt, { ...call, preset: call.preset === undefined ? undefined : presetNamed(doc, call.preset) })
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
