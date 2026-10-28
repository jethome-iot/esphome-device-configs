// Wire contract for the schedule editor API. This file is the source of truth:
// the C++ handler in web_schedule_editor.cpp and every consumer follow it, and a
// shape change is one edit here plus the matching C++, in the same commit.

/** The file format this contract describes; a document with a higher `version` came from a newer firmware. */
export const SCHEDULE_VERSION = 1

/** A day of the week as files, answers and /schema name it. */
export type Day = 'mon' | 'tue' | 'wed' | 'thu' | 'fri' | 'sat' | 'sun'

/** The days Monday first: the order a file lists them in, and /schema too. */
export const DAYS: ReadonlyArray<Day> = ['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun']

/**
 * The palette a mode's colour is one of, in /schema's order. The firmware only checks the key;
 * the dashboard maps each to a colour that reads in both themes.
 */
export const COLOR_KEYS = [
  'red',
  'orange',
  'amber',
  'yellow',
  'lime',
  'green',
  'teal',
  'cyan',
  'blue',
  'indigo',
  'violet',
  'pink',
  'grey'
] as const

export type ColorKey = (typeof COLOR_KEYS)[number]

/** What an action drives. */
export type TargetType = 'relay' | 'thermostat'

/** A relay closes; a thermostat goes back on, in the mode it had before it went off. */
export interface OnValue {
  type: 'on'
}

/** A relay opens, a thermostat goes to mode off. */
export interface OffValue {
  type: 'off'
}

/**
 * A thermostat takes one of its presets, by key. One that is off is turned on first unless the
 * preset brings a mode of its own.
 */
export interface PresetValue {
  type: 'preset'
  /** The preset's key (a slug), as GET /targets lists them. */
  preset: string
}

/** A thermostat takes this target, turned on first when it is off. The thermostat holds it in its range. */
export interface TargetValue {
  type: 'target'
  target: number
}

/** What an action sets. A relay takes on and off, a thermostat any of the four. */
export type ScheduleValue = OnValue | OffValue | PresetValue | TargetValue

export type ValueType = ScheduleValue['type']
export type RelayValueType = 'on' | 'off'
export type ThermostatValueType = ValueType

/** A relay switched on or off, by its object id. */
export interface RelayAction {
  type: 'relay'
  id: string
  value: OnValue | OffValue
}

/** A thermostat's value, by the thermostat's id. */
export interface ThermostatAction {
  type: 'thermostat'
  id: string
  value: ScheduleValue
}

/** The rest of the list waits this long: whole seconds, MIN_WAIT_SECONDS to MAX_WAIT_SECONDS. */
export interface WaitAction {
  type: 'wait'
  seconds: number
}

/** One step of a mode's list. */
export type ModeAction = RelayAction | ThermostatAction | WaitAction

export type ActionType = ModeAction['type']

/** A named list of actions a switch point runs, from its first action, in order. */
export interface Mode {
  /** [a-z0-9-], 1 to 24, unique in the schedule; points name the mode by it, so a rename keeps it. */
  id: string
  /** 1 to 24 characters, unique in the schedule ignoring ASCII case and runs of spaces. */
  name: string
  /** Absent: the editor shows the first palette colour no other mode has (modeColors()). */
  color?: ColorKey
  /** 0 to 12; a mode with none does nothing. */
  actions: ModeAction[]
}

/** From `at` on, until the next point of the week, `mode` is in force; its list ran at `at`. */
export interface SwitchPoint {
  /** "HH:MM", 00:00 to 23:59, the device's local time. */
  at: string
  /** A mode's id. */
  mode: string
}

/**
 * The points of each day. A stored or a served document has all seven, Monday first, each
 * sorted by time; a body may leave a day out, which reads as none. A point's mode holds until
 * the next point, across midnight and from Sunday into Monday.
 */
export type Week = Record<Day, SwitchPoint[]>

/** A relay or a thermostat an action names. */
export interface ScheduleTarget {
  type: TargetType
  /** A relay's object id, or a thermostat's id. */
  id: string
}

/** The stored document for one schedule, as GET /get answers it. */
export interface ScheduleDocument {
  /**
   * SCHEDULE_VERSION, or the higher number of a file a newer firmware wrote: that schedule runs,
   * but its Save and an import over it are 409, since they would drop what this firmware does not know.
   */
  version: number
  /**
   * 0 for a new schedule; moves on by one at every change of the document: a Save, an enable or
   * a disable that changes the flag, a take-over that disables it, an /import over it, a rename
   * at boot. A file without one reads as 0.
   */
  revision: number
  /** Immutable slug and the document's file name; a rename never changes it. */
  id: string
  /** 1 to 48 characters, unique among the schedules ignoring ASCII case and runs of spaces. */
  name: string
  enabled: boolean
  /** 1 to 8. */
  modes: Mode[]
  week: Week
}

/** A mode as a body may send it: `actions` left out reads as none. */
export type ModeInput = Omit<Mode, 'actions'> & { actions?: ModeAction[] }

/**
 * What POST /save takes. An absent or empty `id` creates a schedule, an existing one replaces
 * that schedule's document and keeps its id. `enabled` left out is true, a day left out has no
 * points. `version` is not read: the device writes its own.
 */
export type ScheduleSaveInput = Omit<ScheduleDocument, 'id' | 'version' | 'revision' | 'enabled' | 'modes' | 'week'> & {
  id?: string
  version?: number
  /**
   * The one GET /get gave. On an update, a whole number other than the device's is 409
   * (STALE_DOCUMENT): another client changed the schedule since, and the Save would undo that.
   * Left out, or not a whole number, it is not checked; a create ignores it.
   */
  revision?: number
  enabled?: boolean
  modes: ModeInput[]
  week: Partial<Week>
}

/** A document before the device has given it an id. */
export type ScheduleDraft = Omit<ScheduleSaveInput, 'id'>

/**
 * What POST /import takes: a document as GET /get answers it, or as a backup's file holds it,
 * under the slug `id` it is to have. A schedule with that id is replaced, otherwise one is
 * created with it; `enabled` is kept. `revision` is not read: a replaced schedule's moves on by
 * one, a new one's is 0.
 */
export type ScheduleImportInput = ScheduleSaveInput & { id: string }

/**
 * Where a schedule is, the first that holds of: `disabled`; `waiting_for_clock` (the device has
 * no valid time yet); `conflict` (a schedule before it by id that runs drives one of the
 * relays or thermostats its modes name, and it waits whole); `running`.
 */
export type ScheduleState = 'disabled' | 'waiting_for_clock' | 'conflict' | 'running'

/** Why an action cannot run as written right now. */
export type IssueKind = 'missing_relay' | 'missing_thermostat' | 'missing_preset' | 'no_thermostats' | 'held'

/**
 * An action the device skips right now. A missing relay, thermostat or preset, or a firmware
 * without thermostats, skips it whenever the list gets there; a `held` relay belongs to a
 * running thermostat, and takes the mode in force once it is let go.
 */
export interface ActionIssue {
  /** The mode's id. */
  mode: string
  /** The action's index in the mode's `actions`, from 0. */
  action: number
  kind: IssueKind
  /**
   * `Relay "relay_7" is not on this device`, `Thermostat "attic" is not on this device`,
   * `Thermostat "Living Room" has no preset "eco"`, "This device has no thermostats",
   * `Thermostat "Floor" drives Relay 3`.
   */
  text: string
  /** The name of the thermostat that holds the relay; `held` only. */
  held_by?: string
}

export interface ScheduleStatus {
  state: ScheduleState
  /**
   * Why it does not run, a sentence: "Waiting for the clock", `Schedule "Winter" drives Living
   * Room` (the first target it shares). "" when it runs or is disabled.
   */
  reason: string
  /**
   * Present while it runs and names a thermostat that is switched off (disabled): `Thermostat
   * "Hall" is stopped; it starts in the scheduled values`.
   */
  note?: string
  /** Every action of every mode the device would skip now, mode by mode; given whatever the state. */
  issues: ActionIssue[]
}

/** A point of the week, with the day it is on. */
export interface PointRef {
  day: Day
  at: string
  /** The mode's id. */
  mode: string
}

/** The device's local time; the page draws "now" from it, not from the browser's zone. */
export interface DeviceClock {
  day: Day
  /** "HH:MM:SS". */
  time: string
}

/** One row of GET /list. It carries no modes and no week: the page GETs the schedule for those. */
export interface ScheduleSummary {
  id: string
  /** As ScheduleDocument.revision: a row whose revision moved on is a schedule another client changed. */
  revision: number
  name: string
  enabled: boolean
  /**
   * Every relay and thermostat the modes name, each once, in the order they first name them.
   * `name` is the relay's or the thermostat's, or the id when the device does not have it.
   */
  targets: Array<ScheduleTarget & { name: string }>
  status: ScheduleStatus
  /**
   * The point in force at the device's clock, so its mode and since when: the last one at or
   * before it, from an earlier day or the week before when today has none yet. Null while the
   * clock is not valid; given for a disabled schedule too, as a preview.
   */
  now: PointRef | null
  /** The first point after the clock, the same mode or not; null as `now`. */
  next: PointRef | null
}

export interface SchedulesResponse {
  success: true
  count: number
  max_schedules: number
  /** Null until the device has a valid time. */
  clock: DeviceClock | null
  /** By id. */
  schedules: ScheduleSummary[]
}

/** A thermostat's mode, as the thermostat editor's API words it. */
export type ThermostatMode = 'off' | 'heat' | 'cool' | 'heat_cool'

/** What a preset does to the mode: `keep` leaves the thermostat's as it is. */
export type PresetMode = 'keep' | ThermostatMode

export interface TargetPreset {
  key: string
  name: string
  setpoint: number
  mode: PresetMode
}

export interface RelayTargetInfo {
  object_id: string
  name: string
  /** The name of the running thermostat that drives this relay, "" when none does. */
  held_by: string
  /** The id of the schedule that runs and whose modes name it; "" when none does. */
  scheduled_by: string
}

export interface ThermostatTargetInfo {
  id: string
  name: string
  enabled: boolean
  running: boolean
  mode: ThermostatMode
  /** Its visual range: what a target value is held in. */
  min_temperature: number
  max_temperature: number
  step: number
  /** As RelayTargetInfo.scheduled_by. */
  scheduled_by: string
  presets: TargetPreset[]
}

/** GET /targets: every relay, and every thermostat with its presets ([] without climate_hub). */
export interface TargetsResponse {
  success: true
  relays: RelayTargetInfo[]
  thermostats: ThermostatTargetInfo[]
}

export interface SuccessResponse {
  success: true
  message: string
}

/**
 * POST /save and POST /import: the id of the schedule created, updated or replaced. `message`
 * names, after a `;`, the schedules that started on the targets it let go, then the warning.
 */
export interface SaveResponse extends SuccessResponse {
  id: string
  /**
   * Present when it was saved enabled with an action that names a relay, a thermostat or a
   * preset the device does not have: the first one, `skips mode "Day", action 2: relay
   * "relay_7" is not on this device` (skippedWarning()). It is stored and runs all the same,
   * that action skipped; `message` repeats this last.
   */
  warning?: string
  /** Ids of the enabled schedules that waited for a target it let go and drive theirs now. */
  started?: string[]
}

/**
 * POST /delete: `persisted` is false when it is gone but its file is not, so a reboot brings it
 * back. A schedule that waited for its targets starts, and `started` names it.
 */
export interface DeleteResponse extends SuccessResponse {
  persisted: boolean
  started?: string[]
}

/**
 * POST /enable: `persisted` is false when the change is live but did not reach flash (a newer
 * firmware's file is never written).
 */
export interface EnableResponse extends SuccessResponse {
  persisted: boolean
  /** As SaveResponse.warning. */
  warning?: string
  /** Ids of the schedules a take-over stored disabled; `message` names them. */
  stopped?: string[]
  /** As SaveResponse.started, after a disable. */
  started?: string[]
}

/** GET /schema: build-time data, never 503. */
export interface ScheduleSchema {
  version: number
  /** How many schedules the firmware has room for; a create past it is 507. */
  max_schedules: number
  max_points_per_day: number
  /** In characters (code points), not bytes. */
  name_max_length: number
  max_modes: number
  /** Per mode. */
  max_actions: number
  /** In characters. */
  mode_name_max_length: number
  mode_id_max_length: number
  min_wait_seconds: number
  max_wait_seconds: number
  days: Day[]
  colors: ColorKey[]
  values: { relay: RelayValueType[]; thermostat: ThermostatValueType[] }
}

export interface ErrorResponse {
  success: false
  error: string
}

/** Points per day; a Save with more is refused. */
export const MAX_POINTS_PER_DAY = 12

/** Longest name, in characters; the same number /schema serves. */
export const NAME_MAX_LENGTH = 48

/** Longest id: `<id>.json.tmp` stays within LittleFS's names. */
export const ID_MAX_LENGTH = 48

/** Modes per schedule. */
export const MAX_MODES = 8

/** Actions per mode. */
export const MAX_ACTIONS = 12

/** Longest mode name, in characters. */
export const MODE_NAME_MAX_LENGTH = 24

/** Longest mode id. */
export const MODE_ID_MAX_LENGTH = 24

/** A wait's bounds, in seconds: a second to a day. */
export const MIN_WAIT_SECONDS = 1
export const MAX_WAIT_SECONDS = 86400

/** A body, or a file, larger than this is refused with 413. */
export const SCHEDULE_MAX_BYTES = 16384

/** The firmware's `max_schedules` when the YAML leaves it out. */
export const DEFAULT_MAX_SCHEDULES = 16

/** The 404 error for an id the device does not have, in the device's words. */
export const SCHEDULE_NOT_FOUND = 'Schedule not found'

/** The 409 error of a Save or an import over a document whose `version` is above SCHEDULE_VERSION. */
export const NEWER_FILE = 'A newer firmware wrote this schedule; update the firmware to change it'

/**
 * The 409 error of a Save whose `revision` is not the device's, in the device's words: the form
 * was read before another Save, an enable or a disable, or an import changed the schedule.
 */
export const STALE_DOCUMENT = 'The device changed this schedule since it was read; reload it'

/** The values each target takes, as /schema lists them. */
export const RELAY_VALUES: ReadonlyArray<RelayValueType> = ['on', 'off']
export const THERMOSTAT_VALUES: ReadonlyArray<ThermostatValueType> = ['on', 'off', 'preset', 'target']
