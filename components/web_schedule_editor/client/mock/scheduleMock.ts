// In-process mock for the web_schedule_editor HTTP API — the dev/QA/test double
// for the contract in ../types.ts + ../scheduleApi.ts. Lives WITH the SDK so the
// mock, the types, and the client stay one unit and cannot drift apart.
//
// This file is the transport-agnostic CORE and stays dependency-free (client/ has
// no package.json / node_modules): seed data + a stateful dispatcher + a FetchImpl
// adapter. The Vite dev-server glue lives in each consumer's vite.config.ts, which
// hands createScheduleMockStore() the device's relays and thermostats (`targets`) and
// what a switch point does to them (`apply`).
//
// It answers the way the device does, in the device's order: an unknown route is
// 404, the wrong method 405 (before any id is looked at), query parameters 400, and
// only then the lookup (404) and the change itself. Only /save and /import read a
// body, so only they can be 413; the body is read the way ArduinoJson reads it, and
// the document as the schedules component reads it (../week.ts). A Save or an import
// is refused, in order, for the document, an unknown id (404), a newer firmware's
// file (409), the limit (507), the name (409) and a target another enabled schedule
// drives (409); an enable for the last unless it asks to take the target over.
// A target or a preset the device does not have is no refusal: the schedule is
// stored, with a `warning`, and /list says why it does not run.
//
// The clock is `now()`, read on every request and by tick(). A schedule that comes to
// drive its target (the first valid clock, a Save or an enable, its target or preset
// coming back, the schedule in front of it going) gets the value in force; one that
// drives it gets each point the clock passes, only the latest of several. The clock
// only moves forward here: a step back reads as most of a week ahead. A relay that
// `targets()` says a thermostat holds is left alone, and gets the value in force once
// it is let go. `apply()` is told each value, nothing else is.
// tests/components/web_schedule_editor/contract.json lists the requests it must
// answer as the device does.
import type {
  Day,
  PointRef,
  RelayTargetInfo,
  ScheduleDocument,
  ScheduleSchema,
  SchedulesResponse,
  ScheduleStatus,
  ScheduleSummary,
  ScheduleTarget,
  ScheduleValue,
  TargetsResponse,
  ThermostatTargetInfo
} from '../types'
import {
  DAYS,
  DEFAULT_MAX_SCHEDULES,
  MAX_POINTS_PER_DAY,
  NAME_MAX_LENGTH,
  NEWER_FILE,
  RELAY_VALUES,
  SCHEDULE_MAX_BYTES,
  SCHEDULE_NOT_FOUND,
  SCHEDULE_VERSION,
  THERMOSTAT_VALUES
} from '../types'
import type { FetchImpl } from '../scheduleApi'
import {
  decodeSchedule,
  formatHHMM,
  inForce,
  MINUTES_PER_WEEK,
  nameError,
  nameKey,
  nextAfter,
  notRunningWarning,
  parseHHMM,
  RESERVED_IDS,
  slugify,
  uniqueId,
  weekMinute
} from '../week'

/** The device's local time: a day and the minutes since its midnight. */
export interface MockClock {
  day: Day
  /** 0 to 1439. */
  minute: number
  /** 0 to 59; 0 when left out. */
  second?: number
}

/** What the device has to drive, as /targets lists it, but for who schedules it: the mock knows that. */
export interface DeviceTargets {
  relays: Array<Omit<RelayTargetInfo, 'scheduled_by'>>
  thermostats: Array<Omit<ThermostatTargetInfo, 'scheduled_by'>>
}

// --- Seed data ---------------------------------------------------------------
// Typed against the SDK contract, so these double as canonical example payloads
// AND cannot silently drift from ../types.ts (a shape change fails tsc here).
// The names are the thermostat mock's, whose Living Room and Floor Heating run on
// relays 1 and 2.

export const seedTargets: DeviceTargets = {
  relays: [
    { object_id: 'relay_1', name: 'Relay 1', held_by: 'Living Room' },
    { object_id: 'relay_2', name: 'Relay 2', held_by: 'Floor Heating' },
    { object_id: 'relay_3', name: 'Relay 3', held_by: '' },
    { object_id: 'relay_4', name: 'Relay 4', held_by: '' },
    { object_id: 'relay_5', name: 'Relay 5', held_by: '' },
    { object_id: 'relay_6', name: 'Relay 6', held_by: '' }
  ],
  thermostats: [
    {
      id: 'floor-heating',
      name: 'Floor Heating',
      enabled: true,
      running: true,
      mode: 'heat',
      min_temperature: 5,
      max_temperature: 45,
      step: 0.5,
      presets: []
    },
    {
      id: 'guest-room',
      name: 'Guest Room',
      enabled: false,
      running: false,
      mode: 'heat',
      min_temperature: 5,
      max_temperature: 45,
      step: 0.5,
      presets: []
    },
    {
      id: 'living-room',
      name: 'Living Room',
      enabled: true,
      running: true,
      mode: 'heat',
      min_temperature: 5,
      max_temperature: 45,
      step: 0.5,
      presets: [
        { key: 'comfort', name: 'Comfort', setpoint: 22, mode: 'keep' },
        { key: 'eco', name: 'Eco', setpoint: 19, mode: 'keep' },
        { key: 'away', name: 'Away', setpoint: 12, mode: 'heat' },
        { key: 'night', name: 'Night', setpoint: 20, mode: 'keep' }
      ]
    }
  ]
}

const workday = (): ScheduleDocument['week']['mon'] => [
  { at: '06:30', value: { type: 'preset', preset: 'comfort' } },
  { at: '08:30', value: { type: 'preset', preset: 'eco' } },
  { at: '17:00', value: { type: 'preset', preset: 'comfort' } },
  { at: '23:00', value: { type: 'preset', preset: 'night' } }
]
const weekend = (): ScheduleDocument['week']['mon'] => [
  { at: '08:00', value: { type: 'preset', preset: 'comfort' } },
  { at: '23:30', value: { type: 'preset', preset: 'night' } }
]
const evenings = (): ScheduleDocument['week']['mon'] => [
  { at: '19:00', value: { type: 'on' } },
  { at: '23:30', value: { type: 'off' } }
]

// Living Room follows a working week; the porch light comes on every evening; Guest Room
// warms up for the weekend, but is off.
export const seedSchedules: ScheduleDocument[] = [
  {
    version: SCHEDULE_VERSION,
    id: 'guest-room-weekends',
    name: 'Guest room weekends',
    enabled: false,
    target: { type: 'thermostat', id: 'guest-room' },
    week: {
      mon: [],
      tue: [],
      wed: [],
      thu: [],
      fri: [{ at: '18:00', value: { type: 'target', target: 21 } }],
      sat: [],
      sun: [{ at: '18:00', value: { type: 'off' } }]
    }
  },
  {
    version: SCHEDULE_VERSION,
    id: 'living-room-week',
    name: 'Living room week',
    enabled: true,
    target: { type: 'thermostat', id: 'living-room' },
    week: {
      mon: workday(),
      tue: workday(),
      wed: workday(),
      thu: workday(),
      fri: workday(),
      sat: weekend(),
      sun: weekend()
    }
  },
  {
    version: SCHEDULE_VERSION,
    id: 'porch-light',
    name: 'Porch light',
    enabled: true,
    target: { type: 'relay', id: 'relay_3' },
    week: {
      mon: evenings(),
      tue: evenings(),
      wed: evenings(),
      thu: evenings(),
      fri: evenings(),
      sat: evenings(),
      sun: evenings()
    }
  }
]

export function schemaFor(maxSchedules: number): ScheduleSchema {
  return {
    version: SCHEDULE_VERSION,
    max_schedules: maxSchedules,
    max_points_per_day: MAX_POINTS_PER_DAY,
    name_max_length: NAME_MAX_LENGTH,
    days: [...DAYS],
    values: { relay: [...RELAY_VALUES], thermostat: [...THERMOSTAT_VALUES] }
  }
}

// --- The body ----------------------------------------------------------------
// ArduinoJson's reading of it, with its error names: blanks, then one value, and
// whatever follows the value is ignored; a NUL ends the input, and a raw control
// character inside a string is taken. Stricter than ArduinoJson only where it
// reads more than JSON (unquoted keys, `+1`, `01`), which no client sends. The
// thermostat mock reads its bodies the same way.

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

// --- Stateful core -----------------------------------------------------------

export interface MockResult {
  status: number
  body: unknown
  /** Headers the device sets besides Content-Type (the Allow of a 405). */
  headers?: Record<string, string>
}

export interface ScheduleMockStore {
  /**
   * Dispatch one API call. `endpoint` is the path AFTER the api base, e.g. '/list',
   * '/get'. `method` must match the route, as on the device. `body` is the raw request
   * body (read by /save and /import only). Every call looks at the clock first, as tick().
   */
  handle(method: string, endpoint: string, search: URLSearchParams, body: string): MockResult
  /**
   * The device's one-second tick: reads the clock and the targets, applies the points the
   * clock passed since the last look, starts what now drives its target, and gives a relay
   * that was let go the value it waited with.
   */
  tick(): void
}

export interface ScheduleMockStoreOptions {
  /** The firmware's `max_schedules`. Default 16. */
  maxSchedules?: number
  /** The device's local time, null while it has none. Default: this machine's local time. */
  now?: () => MockClock | null
  /**
   * The relays and the thermostats on the device, read whenever the mock needs them, so one
   * that comes or goes, a preset added or removed, a relay held or let go, shows at once.
   * Default seedTargets.
   */
  targets?: () => DeviceTargets
  /** What a schedule does to its target: called with each value it applies. */
  apply?: (target: ScheduleTarget, value: ScheduleValue) => void
  /** False for a firmware without climate_hub: no thermostat to drive, /targets lists none. Default true. */
  climateHub?: boolean
  /**
   * The schedules on the device at boot, in place of seedSchedules. One whose `version` is
   * above SCHEDULE_VERSION stands for a file a newer firmware wrote; two enabled on one target
   * stand for files written by hand, where the first by id drives it.
   */
  seed?: ScheduleDocument[]
}

// GET routes read, POST routes change something.
const ROUTES = new Map<string, boolean>([
  ['list', false],
  ['get', false],
  ['targets', false],
  ['schema', false],
  ['ping', false],
  ['save', true],
  ['import', true],
  ['delete', true],
  ['enable', true]
])

interface Entry {
  doc: ScheduleDocument
  /** Its relay was held when a value came: the value in force goes there once it is let go. */
  pending: boolean
  /** It drove its target (running or held) at the last look. */
  driving: boolean
}

function fail(status: number, error: string): MockResult {
  return { status, body: { success: false, error } }
}

function ok(message: string, extra: Record<string, unknown> = {}): MockResult {
  return { status: 200, body: { success: true, message, ...extra } }
}

// The device's id parameter: present and a slug, "Porch light" is not an id.
function idParam(search: URLSearchParams): string | MockResult {
  const raw = search.get('id')
  if (raw === null) return fail(400, 'Missing id parameter')
  if (slugify(raw) !== raw) return fail(400, 'Invalid id parameter')
  return raw
}

// "true" or "false" and nothing else: "1" or "yes" is more likely a typo than a choice.
function boolParam(search: URLSearchParams, name: string): boolean | null | MockResult {
  const raw = search.get(name)
  if (raw === null) return null
  if (raw !== 'true' && raw !== 'false') return fail(400, `Invalid ${name} parameter`)
  return raw === 'true'
}

function localClock(): MockClock {
  const d = new Date()
  return { day: DAYS[(d.getDay() + 6) % 7] ?? 'mon', minute: d.getHours() * 60 + d.getMinutes(), second: d.getSeconds() }
}

const drives = (state: ScheduleStatus['state']) => state === 'running' || state === 'held'
const keyOf = (doc: ScheduleDocument) => `${doc.target.type}:${doc.target.id}`
const newer = (doc: ScheduleDocument) => doc.version > SCHEDULE_VERSION
const quoted = (names: string[]) => names.map((n) => `"${n}"`).join(' and ')

/** Fresh, isolated mock state (seed is deep-copied, so instances never share state). */
export function createScheduleMockStore(options: ScheduleMockStoreOptions = {}): ScheduleMockStore {
  const maxSchedules = options.maxSchedules ?? DEFAULT_MAX_SCHEDULES
  const now = options.now ?? localClock
  const targetsOf = options.targets ?? (() => seedTargets)
  const climateHub = options.climateHub ?? true
  const entries: Entry[] = []
  // The week minute the clock was at when last looked at; null until it was valid once.
  let mark: number | null = null

  const byId = (a: Entry, b: Entry) => (a.doc.id < b.doc.id ? -1 : a.doc.id > b.doc.id ? 1 : 0)
  const find = (id: string) => entries.find((e) => e.doc.id === id)
  const relayOf = (id: string) => targetsOf().relays.find((r) => r.object_id === id)
  const thermostatOf = (id: string) => (climateHub ? targetsOf().thermostats.find((t) => t.id === id) : undefined)

  // How a message names the target: its name, or its id when the device does not have it.
  function targetName(doc: ScheduleDocument): string {
    const found = doc.target.type === 'relay' ? relayOf(doc.target.id) : thermostatOf(doc.target.id)
    return found?.name ?? doc.target.id
  }

  // The enabled schedule that drives the target `key` names: the first by id, whether the
  // target is there or not.
  function driverOf(key: string): Entry | undefined {
    return entries.find((e) => e.doc.enabled && keyOf(e.doc) === key)
  }

  const isLoser = (e: Entry) => e.doc.enabled && driverOf(keyOf(e.doc)) !== e
  const losers = () => new Set(entries.filter(isLoser).map((e) => e.doc.id))

  // The ones that waited behind another schedule before a change and drive their target
  // after it, by id; `self` is the one the change was about.
  function promoted(before: Set<string>, self?: Entry): string[] {
    return entries.filter((e) => e !== self && before.has(e.doc.id) && !isLoser(e)).map((e) => e.doc.id)
  }

  function startedNote(ids: string[]): string {
    return ids.length ? `; ${quoted(ids.map((id) => find(id)?.doc.name ?? id))} started` : ''
  }

  // Why the target or a preset of `doc` is not on the device, or null. The first preset
  // missing in the week's order: Monday first, then by time.
  function missingOf(doc: ScheduleDocument): { state: 'missing_target' | 'missing_preset'; reason: string } | null {
    const id = doc.target.id
    if (doc.target.type === 'relay') {
      return relayOf(id) ? null : { state: 'missing_target', reason: `Relay "${id}" is not on this device` }
    }
    if (!climateHub) return { state: 'missing_target', reason: 'This device has no thermostats' }
    const thermostat = thermostatOf(id)
    if (!thermostat) return { state: 'missing_target', reason: `Thermostat "${id}" is not on this device` }
    for (const day of DAYS) {
      for (const point of doc.week[day]) {
        const value = point.value
        if (value.type === 'preset' && !thermostat.presets.some((p) => p.key === value.preset)) {
          return { state: 'missing_preset', reason: `Thermostat "${thermostat.name}" has no preset "${value.preset}"` }
        }
      }
    }
    return null
  }

  function warningOf(doc: ScheduleDocument): string | undefined {
    const missing = doc.enabled ? missingOf(doc) : null
    return missing ? notRunningWarning(missing.reason) : undefined
  }

  function statusOf(e: Entry, clock: MockClock | null): ScheduleStatus {
    const doc = e.doc
    if (!doc.enabled) return { state: 'disabled', reason: '' }
    if (!clock) return { state: 'waiting_for_clock', reason: 'Waiting for the clock' }
    const missing = missingOf(doc)
    if (missing) return missing
    const driver = driverOf(keyOf(doc))
    if (driver && driver !== e) return { state: 'conflict', reason: `Schedule "${driver.doc.name}" drives this target` }
    if (doc.target.type === 'relay') {
      const relay = relayOf(doc.target.id)
      if (relay && relay.held_by) {
        return { state: 'held', reason: `Thermostat "${relay.held_by}" drives ${relay.name}`, held_by: relay.held_by }
      }
      return { state: 'running', reason: '' }
    }
    const thermostat = thermostatOf(doc.target.id)
    if (thermostat && !thermostat.enabled) {
      return {
        state: 'running',
        reason: '',
        note: `Thermostat "${thermostat.name}" is stopped; it starts in the scheduled values`
      }
    }
    return { state: 'running', reason: '' }
  }

  const pointAt = (doc: ScheduleDocument, clock: MockClock) => inForce(doc.week, clock.day, clock.minute)

  // The engine's apply_: a held relay is left alone and the value waits for it.
  function apply(e: Entry, value: ScheduleValue, state: ScheduleStatus['state']) {
    if (state === 'held') {
      e.pending = true
      return
    }
    e.pending = false
    options.apply?.({ ...e.doc.target }, structuredClone(value))
  }

  // The engine's start_: the value in force, when it drives its target and the clock is valid.
  function start(e: Entry, clock: MockClock | null) {
    const state = statusOf(e, clock).state
    e.driving = drives(state)
    if (!e.driving || !clock) return
    const point = pointAt(e.doc, clock)
    if (point) apply(e, point.value, state)
  }

  // What changed since the last look: one that drives its target now and did not starts, one
  // whose relay was let go gets the value in force.
  function settle(clock: MockClock | null) {
    for (const e of entries) {
      const state = statusOf(e, clock).state
      if (drives(state) && !e.driving) {
        start(e, clock)
        continue
      }
      if (state === 'running' && e.pending && clock) {
        const point = pointAt(e.doc, clock)
        if (point) apply(e, point.value, state)
      }
      if (!drives(state)) e.pending = false
      e.driving = drives(state)
    }
  }

  // The points the clock passed since the last look go to the schedules that drove their
  // target then and still do: only the latest one each, as after a stalled loop.
  function tick() {
    const clock = now()
    if (clock) {
      const minute = weekMinute(clock.day, clock.minute)
      if (mark !== null && minute !== mark) {
        const span = (minute - mark + MINUTES_PER_WEEK) % MINUTES_PER_WEEK
        for (const e of entries) {
          if (!e.driving) continue
          const state = statusOf(e, clock).state
          const point = drives(state) ? pointAt(e.doc, clock) : null
          if (!point) continue
          const since = (minute - weekMinute(point.day, parseHHMM(point.at) ?? 0) + MINUTES_PER_WEEK) % MINUTES_PER_WEEK
          if (since < span) apply(e, point.value, state)
        }
      }
      mark = minute
    }
    settle(clock)
  }

  // A conflict on the target `doc` names: the enabled schedule that drives it, unless that is `doc`.
  function conflictOf(doc: ScheduleDocument): MockResult | undefined {
    const driver = driverOf(keyOf(doc))
    if (!driver || driver.doc.id === doc.id) return undefined
    return fail(409, `"${driver.doc.name}" already drives ${targetName(doc)}; switch it off first or take over`)
  }

  function readBody(body: string): { value: unknown } | MockResult {
    if (new TextEncoder().encode(body).length > SCHEDULE_MAX_BYTES) return fail(413, 'Request body over 16 KiB')
    if (!body) return fail(400, 'Empty request body')
    const parsed = readJson(body)
    if ('error' in parsed) return fail(400, `JSON parse error: ${parsed.error}`)
    return { value: parsed.value }
  }

  // What /save and /import refuse once the document is sound and the id settled, in the
  // device's order: the limit, the name, then the target of an enabled one.
  function refusalOf(doc: ScheduleDocument, stored: Entry | undefined): MockResult | undefined {
    if (!stored && entries.length >= maxSchedules) {
      return fail(507, `This device allows ${maxSchedules} schedules; delete one to add another`)
    }
    const clash = nameError(doc.name, doc.id, entries.map((e) => e.doc))
    if (clash) return fail(409, clash)
    return undefined
  }

  // The change itself: `doc` takes `stored`'s place, or joins the list, and an enabled one
  // gets the value in force, every Save, a rename too.
  function store(doc: ScheduleDocument, stored: Entry | undefined, message: string, clock: MockClock | null): MockResult {
    const before = losers()
    doc.version = SCHEDULE_VERSION
    let entry = stored
    if (entry) {
      entry.doc = doc
      entry.pending = false
    } else {
      entry = { doc, pending: false, driving: false }
      entries.push(entry)
      entries.sort(byId)
    }
    if (doc.enabled) start(entry, clock)
    else entry.driving = false
    const started = promoted(before, entry)
    settle(clock)
    const warning = warningOf(doc)
    return ok(message + startedNote(started) + (warning ? `; ${warning}` : ''), {
      id: doc.id,
      ...(warning ? { warning } : {}),
      ...(started.length ? { started } : {})
    })
  }

  function save(body: string, clock: MockClock | null): MockResult {
    const read = readBody(body)
    if (!('value' in read)) return read
    const decoded = decodeSchedule(read.value)
    if ('error' in decoded) return fail(400, decoded.error)
    const doc = decoded.doc
    // From here on the device is on its loop task, where the schedules live.
    const updating = doc.id !== ''
    const stored = updating ? find(doc.id) : undefined
    if (updating && !stored) return fail(404, SCHEDULE_NOT_FOUND)
    if (stored && newer(stored.doc)) return fail(409, NEWER_FILE)
    const refusal = refusalOf(doc, stored)
    if (refusal) return refusal
    // The id is the device's to give, from the name.
    if (!stored) doc.id = uniqueId(slugify(doc.name), entries.map((e) => e.doc.id))
    const conflict = doc.enabled ? conflictOf(doc) : undefined
    if (conflict) return conflict
    return store(doc, stored, updating ? 'Schedule updated' : 'Schedule created', clock)
  }

  // ScheduleEngine::restore: a Save under the body's id, created when no schedule has it.
  function importDocument(body: string, clock: MockClock | null): MockResult {
    const read = readBody(body)
    if (!('value' in read)) return read
    const decoded = decodeSchedule(read.value, { requireId: true })
    if ('error' in decoded) return fail(400, decoded.error)
    const doc = decoded.doc
    // A boot refuses such a file, so the schedule would be gone after the next one.
    if (RESERVED_IDS.includes(doc.id)) return fail(400, `id '${doc.id}' is reserved`)
    const stored = find(doc.id)
    if (stored && newer(stored.doc)) return fail(409, NEWER_FILE)
    const refusal = refusalOf(doc, stored)
    if (refusal) return refusal
    const conflict = doc.enabled ? conflictOf(doc) : undefined
    if (conflict) return conflict
    return store(doc, stored, stored ? 'Schedule replaced' : 'Schedule created', clock)
  }

  function remove(search: URLSearchParams, clock: MockClock | null): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const i = entries.findIndex((e) => e.doc.id === id)
    if (i < 0) return fail(404, SCHEDULE_NOT_FOUND)
    const before = losers()
    // The target stays as the schedule left it.
    entries.splice(i, 1)
    const started = promoted(before)
    settle(clock)
    return ok('Schedule deleted' + startedNote(started), { persisted: true, ...(started.length ? { started } : {}) })
  }

  function enable(search: URLSearchParams, clock: MockClock | null): MockResult {
    const id = idParam(search)
    if (typeof id !== 'string') return id
    const value = boolParam(search, 'value')
    if (value === null) return fail(400, 'Missing value parameter')
    if (typeof value !== 'boolean') return value
    const takeOver = boolParam(search, 'take_over')
    if (takeOver !== null && typeof takeOver !== 'boolean') return takeOver
    const e = find(id)
    if (!e) return fail(404, SCHEDULE_NOT_FOUND)
    const before = losers()

    if (!value) {
      // Written only when the flag changes, and never into a newer firmware's file.
      const persisted = !(e.doc.enabled && newer(e.doc))
      e.doc.enabled = false
      e.pending = false
      e.driving = false
      const started = promoted(before, e)
      settle(clock)
      return ok('Schedule disabled' + startedNote(started), { persisted, ...(started.length ? { started } : {}) })
    }

    const key = keyOf(e.doc)
    const driver = driverOf(key)
    if (driver === e) {
      const warning = warningOf(e.doc)
      return ok('Schedule enabled' + (warning ? `; ${warning}` : ''), { persisted: true, ...(warning ? { warning } : {}) })
    }
    if (driver && !takeOver) {
      return fail(409, `"${driver.doc.name}" already drives ${targetName(e.doc)}; switch it off first or take over`)
    }
    // A take-over stores every other enabled one on the target disabled.
    const stopped = entries.filter((o) => o !== e && o.doc.enabled && keyOf(o.doc) === key)
    let persisted = true
    for (const other of stopped) {
      // A newer firmware's file cannot record the take-over, so the others' files keep their flag too.
      persisted = persisted && !newer(other.doc) && !newer(e.doc)
      other.doc.enabled = false
      other.pending = false
      other.driving = false
    }
    if (!e.doc.enabled) persisted = persisted && !newer(e.doc)
    e.doc.enabled = true
    start(e, clock)
    const started = promoted(before, e)
    settle(clock)
    const warning = warningOf(e.doc)
    let message = 'Schedule enabled'
    if (stopped.length) message += `; ${quoted(stopped.map((o) => o.doc.name))} stopped`
    message += startedNote(started)
    if (warning) message += `; ${warning}`
    return ok(message, {
      persisted,
      ...(warning ? { warning } : {}),
      ...(stopped.length ? { stopped: stopped.map((o) => o.doc.id) } : {}),
      ...(started.length ? { started } : {})
    })
  }

  function summaryOf(e: Entry, clock: MockClock | null): ScheduleSummary {
    const doc = e.doc
    const point = (ref: PointRef | null) => (ref ? structuredClone(ref) : null)
    return {
      id: doc.id,
      name: doc.name,
      enabled: doc.enabled,
      target: { type: doc.target.type, id: doc.target.id, name: targetName(doc) },
      status: statusOf(e, clock),
      now: clock ? point(pointAt(doc, clock)) : null,
      next: clock ? point(nextAfter(doc.week, clock.day, clock.minute)) : null
    }
  }

  function targets(): TargetsResponse {
    const device = targetsOf()
    const scheduledBy = (key: string) => driverOf(key)?.doc.id ?? ''
    return {
      success: true,
      relays: device.relays.map((r) => ({
        object_id: r.object_id,
        name: r.name,
        held_by: r.held_by,
        scheduled_by: scheduledBy(`relay:${r.object_id}`)
      })),
      thermostats: climateHub
        ? device.thermostats.map((t) => ({
            id: t.id,
            name: t.name,
            enabled: t.enabled,
            running: t.running,
            mode: t.mode,
            min_temperature: t.min_temperature,
            max_temperature: t.max_temperature,
            step: t.step,
            scheduled_by: scheduledBy(`thermostat:${t.id}`),
            presets: t.presets.map((p) => ({ ...p }))
          }))
        : []
    }
  }

  function route(name: string, search: URLSearchParams, body: string, clock: MockClock | null): MockResult {
    switch (name) {
      case 'ping':
        return { status: 200, body: { status: 'ok' } }

      case 'schema':
        return { status: 200, body: schemaFor(maxSchedules) }

      case 'targets':
        return { status: 200, body: targets() }

      case 'list': {
        const time = clock ? `${formatHHMM(clock.minute)}:${String(clock.second ?? 0).padStart(2, '0')}` : ''
        const list: SchedulesResponse = {
          success: true,
          count: entries.length,
          max_schedules: maxSchedules,
          clock: clock ? { day: clock.day, time } : null,
          schedules: entries.map((e) => summaryOf(e, clock))
        }
        return { status: 200, body: list }
      }

      case 'get': {
        const id = idParam(search)
        if (typeof id !== 'string') return id
        const e = find(id)
        return e ? { status: 200, body: structuredClone(e.doc) } : fail(404, SCHEDULE_NOT_FOUND)
      }

      case 'save':
        return save(body, clock)

      case 'import':
        return importDocument(body, clock)

      case 'delete':
        return remove(search, clock)

      case 'enable':
        return enable(search, clock)

      default:
        return fail(404, 'Unknown endpoint')
    }
  }

  function handle(method: string, endpoint: string, search: URLSearchParams, body: string): MockResult {
    tick()
    const name = endpoint.startsWith('/') ? endpoint.slice(1) : endpoint
    const mutating = ROUTES.get(name)
    if (mutating === undefined) return fail(404, 'Unknown endpoint')
    const allow = mutating ? 'POST' : 'GET'
    if (method.toUpperCase() !== allow) return { ...fail(405, 'Method not allowed'), headers: { Allow: allow } }
    return route(name, search, body, now())
  }

  // The boot: each file read as the device reads one, by id; nothing applied until the first tick.
  for (const raw of structuredClone(options.seed ?? seedSchedules)) {
    const decoded = decodeSchedule(raw, { requireId: true })
    if ('error' in decoded) throw new Error(`seed ${raw.id}: ${decoded.error}`)
    if (RESERVED_IDS.includes(decoded.doc.id) || find(decoded.doc.id)) continue
    entries.push({ doc: decoded.doc, pending: false, driving: false })
  }
  entries.sort(byId)
  // A name an earlier id has already: the later one is renamed "<name> 2", as the device does.
  for (const [i, e] of entries.entries()) {
    const taken = entries.slice(0, i).map((o) => nameKey(o.doc.name))
    if (!taken.includes(nameKey(e.doc.name))) continue
    let n = 2
    while (taken.includes(nameKey(`${e.doc.name} ${n}`))) n++
    e.doc.name = `${e.doc.name} ${n}`
  }

  return { handle, tick }
}

export interface ScheduleMockOptions extends ScheduleMockStoreOptions {
  /** URL prefix the SDK calls, WITHOUT the trailing endpoint. Default '/schedule-editor/api'. */
  apiBase?: string
}

// --- Transport: FetchImpl (programmatic / unit tests) ------------------------
/**
 * A `FetchImpl` backed by a fresh mock store — inject into createScheduleApi() to
 * exercise the SDK with no dev server:
 *   const api = createScheduleApi({ base: '/schedule-editor/api', fetchImpl: createMockFetch() })
 */
export function createMockFetch(options: ScheduleMockOptions = {}): FetchImpl {
  const apiBase = options.apiBase ?? '/schedule-editor/api'
  const store = createScheduleMockStore(options)
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
