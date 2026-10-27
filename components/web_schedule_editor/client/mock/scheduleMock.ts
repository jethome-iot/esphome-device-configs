// In-process mock for the web_schedule_editor HTTP API — the dev/QA/test double
// for the contract in ../types.ts + ../scheduleApi.ts. Lives WITH the SDK so the
// mock, the types, and the client stay one unit and cannot drift apart.
//
// This file is the transport-agnostic CORE and stays dependency-free (client/ has
// no package.json / node_modules): seed data + a stateful dispatcher + a FetchImpl
// adapter. The Vite dev-server glue lives in each consumer's vite.config.ts, which
// hands createScheduleMockStore() the device's relays and thermostats (`targets`) and
// what an action does to them (`apply`).
//
// It answers the way the device does, in the device's order: an unknown route is
// 404, the wrong method 405 (before any id is looked at), query parameters 400, and
// only then the lookup (404) and the change itself. Only /save and /import read a
// body, so only they can be 413; the body is read the way ArduinoJson reads it, and
// the document as the schedules component reads it (../week.ts). A Save or an import
// is refused, in order, for the document, an unknown id (404), a newer firmware's
// file (409), a Save's `revision` other than the stored one (409), the limit (507),
// the name (409) and a target another enabled schedule drives (409); an enable for
// the last unless it asks to take the targets over. Every change of a document moves
// its `revision` on, as the device moves it. A relay, a thermostat or a preset the
// device does not have is no refusal: the schedule is stored, with a `warning`, the
// action is skipped and /list names it among the `issues`.
//
// The clock is `now()`, read on every request and by tick(); `millis()` times the
// waits. A schedule that comes to drive its targets (the first valid clock, a Save or
// an enable, the schedule in front of it going) runs the list of the mode in force;
// one that drives them runs the list of each point the clock passes, only the latest
// of several. A new point stops a list in a wait. The clock only moves forward here:
// a step back reads as most of a week ahead, and no step is a jump. A relay that
// `targets()` says a thermostat holds is skipped, and takes the last action on it the
// list passed once it is let go; a thermostat or a preset that comes back takes the
// actions on it the list passed. `apply()` is told each action, nothing else is.
// tests/components/web_schedule_editor/contract.json lists the requests it must
// answer as the device does.
import type {
  ActionIssue,
  Day,
  ModeAction,
  RelayAction,
  RelayTargetInfo,
  ScheduleDocument,
  ScheduleSchema,
  SchedulesResponse,
  ScheduleStatus,
  ScheduleSummary,
  ScheduleTarget,
  ScheduleValue,
  TargetsResponse,
  ThermostatAction,
  ThermostatTargetInfo
} from '../types'
import {
  COLOR_KEYS,
  DAYS,
  DEFAULT_MAX_SCHEDULES,
  MAX_ACTIONS,
  MAX_MODES,
  MAX_POINTS_PER_DAY,
  MAX_WAIT_SECONDS,
  MIN_WAIT_SECONDS,
  MODE_ID_MAX_LENGTH,
  MODE_NAME_MAX_LENGTH,
  NAME_MAX_LENGTH,
  NEWER_FILE,
  RELAY_VALUES,
  SCHEDULE_MAX_BYTES,
  SCHEDULE_NOT_FOUND,
  SCHEDULE_VERSION,
  STALE_DOCUMENT,
  THERMOSTAT_VALUES
} from '../types'
import type { FetchImpl } from '../scheduleApi'
import {
  conflictError,
  conflictReason,
  decodeSchedule,
  formatHHMM,
  inForce,
  MINUTES_PER_WEEK,
  nameError,
  nameKey,
  namesTarget,
  nextAfter,
  parseHHMM,
  readRevision,
  RESERVED_IDS,
  scheduleTargets,
  skippedWarning,
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

const at = (time: string, mode: string) => ({ at: time, mode })
const everyDay = (workday: () => ScheduleDocument['week']['mon'], weekend = workday): ScheduleDocument['week'] => ({
  mon: workday(),
  tue: workday(),
  wed: workday(),
  thu: workday(),
  fri: workday(),
  sat: weekend(),
  sun: weekend()
})

// Home warms Living Room for the day, starts the boiler pump (relay 1, which Living Room
// holds) and two minutes on the valve (relay 5); the porch light comes on every evening;
// Guest Room warms up for the weekend, but is off.
export const seedSchedules: ScheduleDocument[] = [
  {
    version: SCHEDULE_VERSION,
    revision: 0,
    id: 'guest-room-weekends',
    name: 'Guest room weekends',
    enabled: false,
    modes: [
      {
        id: 'warm',
        name: 'Warm',
        color: 'orange',
        actions: [{ type: 'thermostat', id: 'guest-room', value: { type: 'target', target: 21 } }]
      },
      { id: 'off', name: 'Off', color: 'grey', actions: [{ type: 'thermostat', id: 'guest-room', value: { type: 'off' } }] }
    ],
    week: { mon: [], tue: [], wed: [], thu: [], fri: [at('18:00', 'warm')], sat: [], sun: [at('18:00', 'off')] }
  },
  {
    version: SCHEDULE_VERSION,
    revision: 0,
    id: 'home-weekdays',
    name: 'Home weekdays',
    enabled: true,
    modes: [
      {
        id: 'day',
        name: 'Day',
        color: 'amber',
        actions: [
          { type: 'thermostat', id: 'living-room', value: { type: 'preset', preset: 'comfort' } },
          { type: 'relay', id: 'relay_1', value: { type: 'on' } },
          { type: 'wait', seconds: 120 },
          { type: 'relay', id: 'relay_5', value: { type: 'on' } }
        ]
      },
      {
        id: 'night',
        name: 'Night',
        color: 'indigo',
        actions: [
          { type: 'thermostat', id: 'living-room', value: { type: 'preset', preset: 'eco' } },
          { type: 'relay', id: 'relay_5', value: { type: 'off' } },
          { type: 'relay', id: 'relay_1', value: { type: 'off' } }
        ]
      }
    ],
    week: everyDay(
      () => [at('06:30', 'day'), at('23:00', 'night')],
      () => [at('08:00', 'day'), at('23:30', 'night')]
    )
  },
  {
    version: SCHEDULE_VERSION,
    revision: 0,
    id: 'porch-light',
    name: 'Porch light',
    enabled: true,
    modes: [
      { id: 'on', name: 'On', color: 'green', actions: [{ type: 'relay', id: 'relay_3', value: { type: 'on' } }] },
      { id: 'off', name: 'Off', color: 'grey', actions: [{ type: 'relay', id: 'relay_3', value: { type: 'off' } }] }
    ],
    week: everyDay(() => [at('19:00', 'on'), at('23:30', 'off')])
  }
]

export function schemaFor(maxSchedules: number): ScheduleSchema {
  return {
    version: SCHEDULE_VERSION,
    max_schedules: maxSchedules,
    max_points_per_day: MAX_POINTS_PER_DAY,
    name_max_length: NAME_MAX_LENGTH,
    max_modes: MAX_MODES,
    max_actions: MAX_ACTIONS,
    mode_name_max_length: MODE_NAME_MAX_LENGTH,
    mode_id_max_length: MODE_ID_MAX_LENGTH,
    min_wait_seconds: MIN_WAIT_SECONDS,
    max_wait_seconds: MAX_WAIT_SECONDS,
    days: [...DAYS],
    colors: [...COLOR_KEYS],
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
   * The device's one-second tick: goes on with the lists whose wait is over, gives a relay
   * that was let go and a thermostat or a preset that came back what the list passed, runs
   * the lists the clock calls for: every schedule's at its first valid reading, each point's
   * that it passed since the last look.
   */
  tick(): void
}

export interface ScheduleMockStoreOptions {
  /** The firmware's `max_schedules`. Default 16. */
  maxSchedules?: number
  /** The device's local time, null while it has none. Default: this machine's local time. */
  now?: () => MockClock | null
  /** Milliseconds on a clock that only moves on, for the waits. Default Date.now. */
  millis?: () => number
  /**
   * The relays and the thermostats on the device, read whenever the mock needs them, so one
   * that comes or goes, a preset added or removed, a relay held or let go, shows at once.
   * Default seedTargets.
   */
  targets?: () => DeviceTargets
  /** What an action does to its relay or thermostat: called with each action it applies. */
  apply?: (target: ScheduleTarget, value: ScheduleValue) => void
  /** False for a firmware without climate_hub: no thermostat to drive, /targets lists none. Default true. */
  climateHub?: boolean
  /**
   * The schedules on the device at boot, in place of seedSchedules. One whose `version` is
   * above SCHEDULE_VERSION stands for a file a newer firmware wrote; two enabled sharing a
   * target stand for files written by hand, where the first by id drives it.
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

/** Where a schedule's list is: its mode (-1 for none), its next action, the end of its wait. */
interface Run {
  mode: number
  next: number
  waitUntil: number | null
  /** Thermostats whose actions it passed did not reach them: they take them once back. */
  lost: Set<string>
}

interface Entry {
  doc: ScheduleDocument
  /** Enabled and the first enabled by id on every target its modes name. */
  driving: boolean
  run: Run
}

const idle = (): Run => ({ mode: -1, next: 0, waitUntil: null, lost: new Set() })

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

const newer = (doc: ScheduleDocument) => doc.version > SCHEDULE_VERSION
const quoted = (names: string[]) => names.map((n) => `"${n}"`).join(' and ')
const sameTarget = (a: ScheduleTarget, b: ScheduleTarget) => a.type === b.type && a.id === b.id

/** Fresh, isolated mock state (seed is deep-copied, so instances never share state). */
export function createScheduleMockStore(options: ScheduleMockStoreOptions = {}): ScheduleMockStore {
  const maxSchedules = options.maxSchedules ?? DEFAULT_MAX_SCHEDULES
  const now = options.now ?? localClock
  const millis = options.millis ?? (() => Date.now())
  const targetsOf = options.targets ?? (() => seedTargets)
  const climateHub = options.climateHub ?? true
  const entries: Entry[] = []
  // The week minute the clock was at when last looked at; null until it was valid once.
  let mark: number | null = null
  // What the device's relays and thermostats were at the last look: who held each relay, and
  // each thermostat's preset keys ('' for one not there).
  const heldSeen = new Map<string, string>()
  const presetsSeen = new Map<string, string>()

  const byId = (a: Entry, b: Entry) => (a.doc.id < b.doc.id ? -1 : a.doc.id > b.doc.id ? 1 : 0)
  const find = (id: string) => entries.find((e) => e.doc.id === id)
  const relayOf = (id: string) => targetsOf().relays.find((r) => r.object_id === id)
  const thermostatOf = (id: string) => (climateHub ? targetsOf().thermostats.find((t) => t.id === id) : undefined)

  // How a message names a target: its name, or its id when the device does not have it.
  function targetName(target: ScheduleTarget): string {
    const found = target.type === 'relay' ? relayOf(target.id) : thermostatOf(target.id)
    return found?.name ?? target.id
  }

  // The enabled schedule that drives `target`: the first by id that names it, there or not.
  function holderOf(target: ScheduleTarget): Entry | undefined {
    return entries.find((e) => e.doc.enabled && namesTarget(e.doc, target))
  }

  const drives = (e: Entry) => e.doc.enabled && scheduleTargets(e.doc).every((t) => holderOf(t) === e)

  // Why the device would skip each action now, mode by mode.
  function issuesOf(doc: ScheduleDocument): ActionIssue[] {
    const issues: ActionIssue[] = []
    for (const mode of doc.modes) {
      for (const [index, action] of mode.actions.entries()) {
        const issue = (kind: ActionIssue['kind'], text: string, holder?: string) =>
          issues.push({ mode: mode.id, action: index, kind, text, ...(holder ? { held_by: holder } : {}) })
        if (action.type === 'relay') {
          const relay = relayOf(action.id)
          if (!relay) issue('missing_relay', `Relay "${action.id}" is not on this device`)
          else if (relay.held_by) issue('held', `Thermostat "${relay.held_by}" drives ${relay.name}`, relay.held_by)
        } else if (action.type === 'thermostat') {
          const thermostat = thermostatOf(action.id)
          const value = action.value
          if (!climateHub) issue('no_thermostats', 'This device has no thermostats')
          else if (!thermostat) issue('missing_thermostat', `Thermostat "${action.id}" is not on this device`)
          else if (value.type === 'preset' && !thermostat.presets.some((p) => p.key === value.preset)) {
            issue('missing_preset', `Thermostat "${thermostat.name}" has no preset "${value.preset}"`)
          }
        }
      }
    }
    return issues
  }

  // The first action an enabled schedule skips for good; a held relay only waits.
  function warningOf(doc: ScheduleDocument): string | undefined {
    if (!doc.enabled) return undefined
    const issue = issuesOf(doc).find((i) => i.kind !== 'held')
    const mode = issue && doc.modes.find((m) => m.id === issue.mode)
    return issue && mode ? skippedWarning(mode.name, issue.action, issue.text) : undefined
  }

  function statusOf(e: Entry, clock: MockClock | null): ScheduleStatus {
    const doc = e.doc
    const issues = issuesOf(doc)
    if (!doc.enabled) return { state: 'disabled', reason: '', issues }
    if (!clock) return { state: 'waiting_for_clock', reason: 'Waiting for the clock', issues }
    for (const target of scheduleTargets(doc)) {
      const holder = holderOf(target)
      if (holder && holder !== e) {
        return { state: 'conflict', reason: conflictReason(holder.doc.name, targetName(target)), issues }
      }
    }
    const stopped = scheduleTargets(doc)
      .map((t) => (t.type === 'thermostat' ? thermostatOf(t.id) : undefined))
      .find((t) => t && !t.enabled)
    if (stopped) {
      return {
        state: 'running',
        reason: '',
        note: `Thermostat "${stopped.name}" is stopped; it starts in the scheduled values`,
        issues
      }
    }
    return { state: 'running', reason: '', issues }
  }

  // --- The lists ---

  // The engine's apply: a relay not there or held is left alone; a thermostat or a preset not
  // there is remembered, to take the action once it is back.
  function applyAction(e: Entry, action: RelayAction | ThermostatAction) {
    if (action.type === 'relay') {
      const relay = relayOf(action.id)
      if (!relay || relay.held_by) return
    } else {
      const thermostat = thermostatOf(action.id)
      const value = action.value
      if (!thermostat || (value.type === 'preset' && !thermostat.presets.some((p) => p.key === value.preset))) {
        e.run.lost.add(action.id)
        return
      }
    }
    options.apply?.({ type: action.type, id: action.id }, structuredClone(action.value))
  }

  // The actions up to the next wait, or to the end.
  function advance(e: Entry) {
    const run = e.run
    while (e.run === run && run.mode >= 0 && e.driving) {
      const action = e.doc.modes[run.mode]?.actions[run.next]
      if (!action) return
      run.next++
      if (action.type === 'wait') {
        run.waitUntil = millis() + action.seconds * 1000
        return
      }
      applyAction(e, action)
    }
  }

  // A new list stops the one before, in a wait or not.
  function runMode(e: Entry, mode: number) {
    e.run = { mode, next: 0, waitUntil: null, lost: new Set() }
    advance(e)
  }

  function startInForce(e: Entry, clock: MockClock | null) {
    if (!clock || !e.driving) return
    const point = inForce(e.doc.week, clock.day, clock.minute)
    const mode = point ? e.doc.modes.findIndex((m) => m.id === point.mode) : -1
    if (mode >= 0) runMode(e, mode)
  }

  // The actions the list has passed, in order.
  function passed(e: Entry): ModeAction[] {
    return e.run.mode < 0 ? [] : (e.doc.modes[e.run.mode]?.actions.slice(0, e.run.next) ?? [])
  }

  // Which schedules drive their targets now; one that no longer does stops where it is.
  // Returns those that did not before.
  function refresh(): Entry[] {
    const started: Entry[] = []
    for (const e of entries) {
      const was = e.driving
      e.driving = drives(e)
      if (was && !e.driving) e.run = idle()
      if (e.driving && !was) started.push(e)
    }
    return started
  }

  // After a change, as the engine's settle_: `self` runs its mode's list when saved enabled or
  // when it starts to drive, then every other schedule the change let drive. Their ids.
  function settle(self: Entry | undefined, save: boolean, clock: MockClock | null): string[] {
    const started = refresh()
    if (self && ((save && self.doc.enabled) || started.includes(self))) startInForce(self, clock)
    const others = started.filter((e) => e !== self)
    for (const e of others) startInForce(e, clock)
    return others.map((e) => e.doc.id)
  }

  // A relay let go takes the last action on it the list passed; one a thermostat or a preset
  // came back for gets every action on it the list passed.
  function watchDevice(first: boolean) {
    const device = targetsOf()
    for (const relay of device.relays) {
      const before = heldSeen.get(relay.object_id) ?? ''
      heldSeen.set(relay.object_id, relay.held_by)
      if (first || !before || relay.held_by) continue
      for (const e of entries) {
        if (!e.driving) continue
        const last = passed(e)
          .filter((a): a is RelayAction => a.type === 'relay' && a.id === relay.object_id)
          .pop()
        if (last) applyAction(e, last)
      }
    }
    if (!climateHub) return
    const ids = new Set([...presetsSeen.keys(), ...device.thermostats.map((t) => t.id)])
    for (const id of ids) {
      const thermostat = device.thermostats.find((t) => t.id === id)
      const presets = thermostat ? `[${thermostat.presets.map((p) => p.key).join(',')}]` : ''
      const before = presetsSeen.get(id)
      presetsSeen.set(id, presets)
      if (first || before === presets) continue
      for (const e of entries) {
        if (!e.driving) continue
        const actions = passed(e).filter((a): a is ThermostatAction => a.type === 'thermostat' && a.id === id)
        if (!actions.length) continue
        const ready =
          !!thermostat &&
          actions.every((a) => {
            const value = a.value
            return value.type !== 'preset' || thermostat.presets.some((p) => p.key === value.preset)
          })
        if (!ready) {
          e.run.lost.add(id)
        } else if (e.run.lost.delete(id)) {
          for (const action of actions) applyAction(e, action)
        }
      }
    }
  }

  // The waits that are over, then what the device changed, then the clock: the first valid
  // reading runs every list, a point passed since the last look runs its own, only the latest.
  function tick() {
    for (let more = true; more; ) {
      more = false
      for (const e of entries) {
        if (e.run.waitUntil !== null && millis() >= e.run.waitUntil) {
          e.run.waitUntil = null
          advance(e)
          more = true
        }
      }
    }
    watchDevice(false)
    const clock = now()
    if (!clock) return
    const minute = weekMinute(clock.day, clock.minute)
    if (mark === null) {
      mark = minute
      for (const e of entries) startInForce(e, clock)
      return
    }
    if (minute === mark) return
    const span = (minute - mark + MINUTES_PER_WEEK) % MINUTES_PER_WEEK
    mark = minute
    for (const e of entries) {
      if (!e.driving) continue
      const point = inForce(e.doc.week, clock.day, clock.minute)
      if (!point) continue
      const since = (minute - weekMinute(point.day, parseHHMM(point.at) ?? 0) + MINUTES_PER_WEEK) % MINUTES_PER_WEEK
      const mode = e.doc.modes.findIndex((m) => m.id === point.mode)
      if (since < span && mode >= 0) runMode(e, mode)
    }
  }

  // --- The routes ---

  // A conflict on a target `doc` names: the enabled schedule that drives the first one another drives.
  function conflictOf(doc: ScheduleDocument): MockResult | undefined {
    for (const target of scheduleTargets(doc)) {
      const holder = holderOf(target)
      if (holder && holder.doc.id !== doc.id) return fail(409, conflictError(holder.doc.name, targetName(target)))
    }
    return undefined
  }

  function readBody(body: string): { value: unknown } | MockResult {
    if (new TextEncoder().encode(body).length > SCHEDULE_MAX_BYTES) return fail(413, 'Request body over 16 KiB')
    if (!body) return fail(400, 'Empty request body')
    const parsed = readJson(body)
    if ('error' in parsed) return fail(400, `JSON parse error: ${parsed.error}`)
    return { value: parsed.value }
  }

  // What /save and /import refuse once the document is sound and the id settled, in the
  // device's order: the limit, then the name.
  function refusalOf(doc: ScheduleDocument, stored: Entry | undefined): MockResult | undefined {
    if (!stored && entries.length >= maxSchedules) {
      return fail(507, `This device allows ${maxSchedules} schedules; delete one to add another`)
    }
    const clash = nameError(doc.name, doc.id, entries.map((e) => e.doc))
    if (clash) return fail(409, clash)
    return undefined
  }

  // The change itself: `doc` takes `stored`'s place, its revision moved on and its list
  // stopped, or joins the list at revision 0; an enabled one runs the list of the mode in
  // force, every Save, a rename too.
  function store(doc: ScheduleDocument, stored: Entry | undefined, message: string, clock: MockClock | null): MockResult {
    doc.version = SCHEDULE_VERSION
    doc.revision = stored ? stored.doc.revision + 1 : 0
    let entry = stored
    if (entry) {
      entry.doc = doc
      entry.run = idle()
    } else {
      entry = { doc, driving: false, run: idle() }
      entries.push(entry)
      entries.sort(byId)
    }
    const started = settle(entry, true, clock)
    const warning = warningOf(doc)
    return ok(message + startedNote(started) + (warning ? `; ${warning}` : ''), {
      id: doc.id,
      ...(warning ? { warning } : {}),
      ...(started.length ? { started } : {})
    })
  }

  function startedNote(ids: string[]): string {
    return ids.length ? `; ${quoted(ids.map((id) => find(id)?.doc.name ?? id))} started` : ''
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
    // The revision the form was read at, when the body has one as a whole number; a body that
    // decoded is an object.
    const revision = readRevision((read.value as { revision?: unknown }).revision)
    if (stored && revision !== null && revision !== stored.doc.revision) return fail(409, STALE_DOCUMENT)
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
    // Its list stops; the targets stay as it left them.
    entries.splice(i, 1)
    const started = settle(undefined, false, clock)
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

    if (!value) {
      // Written only when the flag changes, and never into a newer firmware's file.
      const persisted = !(e.doc.enabled && newer(e.doc))
      if (e.doc.enabled) e.doc.revision++
      e.doc.enabled = false
      const started = settle(e, false, clock)
      return ok('Schedule disabled' + startedNote(started), { persisted, ...(started.length ? { started } : {}) })
    }

    const conflict = conflictOf(e.doc)
    if (conflict && !takeOver) return conflict
    // A take-over stores every other enabled one that shares a target disabled.
    const mine = scheduleTargets(e.doc)
    const stopped = conflict
      ? entries.filter((o) => o !== e && o.doc.enabled && mine.some((t) => scheduleTargets(o.doc).some((u) => sameTarget(t, u))))
      : []
    let persisted = true
    for (const other of stopped) {
      // A newer firmware's file cannot record the take-over, so the others' files keep their flag too.
      persisted = persisted && !newer(other.doc) && !newer(e.doc)
      other.doc.enabled = false
      other.doc.revision++
    }
    if (!e.doc.enabled) {
      persisted = persisted && !newer(e.doc)
      e.doc.revision++
    }
    e.doc.enabled = true
    const started = settle(e, false, clock)
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
    return {
      id: doc.id,
      revision: doc.revision,
      name: doc.name,
      enabled: doc.enabled,
      targets: scheduleTargets(doc).map((t) => ({ type: t.type, id: t.id, name: targetName(t) })),
      status: statusOf(e, clock),
      now: clock ? inForce(doc.week, clock.day, clock.minute) : null,
      next: clock ? nextAfter(doc.week, clock.day, clock.minute) : null
    }
  }

  function targets(): TargetsResponse {
    const device = targetsOf()
    const scheduledBy = (target: ScheduleTarget) => holderOf(target)?.doc.id ?? ''
    return {
      success: true,
      relays: device.relays.map((r) => ({
        object_id: r.object_id,
        name: r.name,
        held_by: r.held_by,
        scheduled_by: scheduledBy({ type: 'relay', id: r.object_id })
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
            scheduled_by: scheduledBy({ type: 'thermostat', id: t.id }),
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

  // The boot: each file read as the device reads one, by id; nothing runs until the first tick.
  for (const raw of structuredClone(options.seed ?? seedSchedules)) {
    const decoded = decodeSchedule(raw, { requireId: true })
    if ('error' in decoded) throw new Error(`seed ${raw.id}: ${decoded.error}`)
    if (RESERVED_IDS.includes(decoded.doc.id) || find(decoded.doc.id)) continue
    entries.push({ doc: decoded.doc, driving: false, run: idle() })
  }
  entries.sort(byId)
  // A name an earlier id has already: the later one is renamed "<name> 2", as the device does.
  for (const [i, e] of entries.entries()) {
    const taken = entries.slice(0, i).map((o) => nameKey(o.doc.name))
    if (!taken.includes(nameKey(e.doc.name))) continue
    let n = 2
    while (taken.includes(nameKey(`${e.doc.name} ${n}`))) n++
    e.doc.name = `${e.doc.name} ${n}`
    e.doc.revision++
  }
  refresh()
  watchDevice(true)

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
