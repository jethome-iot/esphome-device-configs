// Wire contract for the schedule editor API. This file is the source of truth:
// the C++ handler in web_schedule_editor.cpp and every consumer follow it, and a
// shape change is one edit here plus the matching C++, in the same commit.

/** The file format this contract describes; a document with a higher `version` came from a newer firmware. */
export const SCHEDULE_VERSION = 1

/** A day of the week as files, answers and /schema name it. */
export type Day = 'mon' | 'tue' | 'wed' | 'thu' | 'fri' | 'sat' | 'sun'

/** The days Monday first: the order a file lists them in, and /schema too. */
export const DAYS: ReadonlyArray<Day> = ['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun']

/** What a schedule drives. */
export type TargetType = 'relay' | 'thermostat'

/** A relay closes; a thermostat has no `on` value (turned on by a preset or a target instead). */
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

/** What a switch point sets. A relay takes on and off, a thermostat a preset, off or a target. */
export type ScheduleValue = OnValue | OffValue | PresetValue | TargetValue

export type ValueType = ScheduleValue['type']
export type RelayValueType = 'on' | 'off'
export type ThermostatValueType = 'preset' | 'off' | 'target'

/** From `at` on, until the next point of the week, the target has `value`. */
export interface SwitchPoint {
  /** "HH:MM", 00:00 to 23:59, the device's local time. */
  at: string
  value: ScheduleValue
}

/**
 * The points of each day. A stored or a served document has all seven, Monday first, each
 * sorted by time; a body may leave a day out, which reads as none. A point's value holds until
 * the next point, across midnight and from Sunday into Monday.
 */
export type Week = Record<Day, SwitchPoint[]>

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
  /** Immutable slug and the document's file name; a rename never changes it. */
  id: string
  /** 1 to 48 characters, unique among the schedules ignoring ASCII case and runs of spaces. */
  name: string
  enabled: boolean
  target: ScheduleTarget
  week: Week
}

/**
 * What POST /save takes. An absent or empty `id` creates a schedule, an existing one replaces
 * that schedule's document and keeps its id. `enabled` left out is true, a day left out has no
 * points. `version` is not read: the device writes its own.
 */
export type ScheduleSaveInput = Omit<ScheduleDocument, 'id' | 'version' | 'enabled' | 'week'> & {
  id?: string
  version?: number
  enabled?: boolean
  week: Partial<Week>
}

/** A document before the device has given it an id. */
export type ScheduleDraft = Omit<ScheduleSaveInput, 'id'>

/**
 * What POST /import takes: a document as GET /get answers it, or as a backup's file holds it,
 * under the slug `id` it is to have. A schedule with that id is replaced, otherwise one is
 * created with it; `enabled` is kept.
 */
export type ScheduleImportInput = ScheduleSaveInput & { id: string }

/**
 * Where a schedule is, the first that holds of: `disabled`; `waiting_for_clock` (the device has
 * no valid time yet); `missing_target` (no such relay or thermostat, or no thermostats on this
 * firmware); `missing_preset` (a point names a preset its thermostat does not have);
 * `conflict` (another enabled schedule, first by id, drives the same target); `held` (its relay
 * is driven by a running thermostat, so the value waits until that lets go); `running`.
 */
export type ScheduleState =
  | 'disabled'
  | 'waiting_for_clock'
  | 'missing_target'
  | 'missing_preset'
  | 'conflict'
  | 'held'
  | 'running'

export interface ScheduleStatus {
  state: ScheduleState
  /**
   * Why it does not run, a sentence: "Waiting for the clock", `Relay "relay_7" is not on this
   * device`, `Thermostat "attic" is not on this device`, "This device has no thermostats",
   * `Thermostat "Living Room" has no preset "eco"`, `Schedule "Winter" drives this target`,
   * `Thermostat "Floor" drives Relay 3`. "" when it runs or is disabled.
   */
  reason: string
  /** The name of the thermostat that holds the relay; present in `held` only. */
  held_by?: string
  /**
   * Present while it runs on a thermostat that is switched off (disabled): `Thermostat "Hall" is
   * stopped; it starts in the scheduled values`.
   */
  note?: string
}

/** A point of the week, with the day it is on. */
export interface PointRef {
  day: Day
  at: string
  value: ScheduleValue
}

/** The device's local time; the page draws "now" from it, not from the browser's zone. */
export interface DeviceClock {
  day: Day
  /** "HH:MM:SS". */
  time: string
}

/** One row of GET /list. It carries no week: the page GETs the schedule for that. */
export interface ScheduleSummary {
  id: string
  name: string
  enabled: boolean
  /** `name` is the relay's or the thermostat's, or the id when the device does not have it. */
  target: ScheduleTarget & { name: string }
  status: ScheduleStatus
  /**
   * The point in force at the device's clock: the last one at or before it, from an earlier day
   * or the week before when today has none yet. Null while the clock is not valid; given for a
   * disabled schedule too, as a preview.
   */
  now: PointRef | null
  /** The first point after the clock, the same value or not; null as `now`. */
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
  /** The id of the enabled schedule that drives it, "" when none does. */
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
 * names, after a `;`, the schedules that started on the target it let go, then the warning.
 */
export interface SaveResponse extends SuccessResponse {
  id: string
  /**
   * Present when it was saved enabled but its target or a preset is not on the device: "not
   * running: " and the status reason with its first letter lowercased (`not running: thermostat
   * "attic" is not on this device`). It is stored all the same; `message` repeats this last.
   */
  warning?: string
  /** Ids of the enabled schedules that waited for this target and drive it now. */
  started?: string[]
}

/**
 * POST /delete: `persisted` is false when it is gone but its file is not, so a reboot brings it
 * back. The schedule that waited for its target starts, and `started` names it.
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
  days: Day[]
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

/** A body, or a file, larger than this is refused with 413. */
export const SCHEDULE_MAX_BYTES = 16384

/** The firmware's `max_schedules` when the YAML leaves it out. */
export const DEFAULT_MAX_SCHEDULES = 16

/** The 404 error for an id the device does not have, in the device's words. */
export const SCHEDULE_NOT_FOUND = 'Schedule not found'

/** The 409 error of a Save or an import over a document whose `version` is above SCHEDULE_VERSION. */
export const NEWER_FILE = 'A newer firmware wrote this schedule; update the firmware to change it'

/** The values each target takes, as /schema lists them. */
export const RELAY_VALUES: ReadonlyArray<RelayValueType> = ['on', 'off']
export const THERMOSTAT_VALUES: ReadonlyArray<ThermostatValueType> = ['preset', 'off', 'target']
