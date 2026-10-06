// Wire contract for the thermostat editor API. This file is the source of truth:
// the C++ handler in web_climate_editor.cpp and every consumer follow it, and a
// shape change is one edit here plus the matching C++, in the same commit.

/**
 * Control law. Persisted, so these strings are a storage format too — which is
 * why `bang_bang` keeps ESPHome's platform name here while the dashboard labels
 * it "Hysteresis", the word a person configuring a boiler actually knows.
 */
export type ControlKind = 'pid' | 'bang_bang'

/** Requested operating mode. */
export type ClimateHubMode = 'off' | 'heat' | 'cool' | 'heat_cool'

/** What a preset does to the mode: `keep` leaves the thermostat's as it is. */
export type PresetMode = 'keep' | ClimateHubMode

/** Home Assistant's built-in presets. A preset by one of these names, in any case, is that preset there. */
export type StandardPresetName = 'eco' | 'away' | 'boost' | 'comfort' | 'home' | 'sleep' | 'activity'

/**
 * What the relays do right now: `heating` or `cooling` while that relay is closed, so a
 * relay held open by its minimum off time is not heating yet, and one held closed by its
 * minimum on time still is. A PID also stays `heating` or `cooling` between two pulses once
 * the first has closed the relay. `off` in mode off, on a fault but `relay_contested`, or
 * when the thermostat is not running.
 */
export type ClimateHubAction = 'off' | 'idle' | 'heating' | 'cooling'

/**
 * What holds a relay away from where the thermostat wants it: `min_off` before it may
 * close, `min_on` before it may open. `none` otherwise, including while a relay switched
 * from elsewhere waits to be put back: that wait is the switch's, not the thermostat's.
 */
export type RelayWait = 'none' | 'min_on' | 'min_off'

/**
 * What is wrong with a running controller. Never persisted. `sensor_stale` and
 * `overtemp` stop it controlling; `relay_contested` only reports that something else
 * keeps moving one of its relays, and it goes on. A sensor or relay the device does
 * not have is no fault: the thermostat waits, and says why in `waiting`.
 */
export type ClimateHubFault = 'none' | 'sensor_stale' | 'overtemp' | 'relay_contested'

/** One driven direction. An empty relay_id means the direction is unused. */
export interface OutputConfig {
  relay_id: string
  period_s: number
  min_on_s: number
  min_off_s: number
}

export interface VisualConfig {
  min_temperature: number
  max_temperature: number
  step: number
}

export interface SafetyConfig {
  sensor_timeout_s: number
  max_temperature: number
}

export interface PidConfig {
  kp: number
  ki: number
  kd: number
  min_integral: number
  max_integral: number
  starting_integral_term: number
  output_samples: number
  derivative_samples: number
  deadband_threshold_low: number
  deadband_threshold_high: number
  deadband_kp_multiplier: number
  deadband_ki_multiplier: number
  deadband_kd_multiplier: number
  deadband_output_samples: number
}

/** The band around the target: switch on at `setpoint - below`, off at `+ above`. */
export interface BangBangConfig {
  below: number
  above: number
}

/** A target, and a mode or `keep`, that a thermostat takes in one step. */
export interface PresetConfig {
  /**
   * The slug of the name the preset was created with, `-2` and on when another preset has it.
   * A rename keeps it, so a rule naming the preset still finds it. The device gives it: a form
   * sends back the key it got, and a new preset none.
   */
  key: string
  /**
   * The thermostat name rules, `none` reserved in any case, and unique among the thermostat's
   * presets ignoring case and runs of spaces. One of STANDARD_PRESETS, in any case, is that
   * built-in preset in Home Assistant; any other name is shown as it is.
   */
  name: string
  /** Held inside the thermostat's visual range. */
  setpoint: number
  /** One the thermostat's relays can serve, as for its own mode. */
  mode: PresetMode
}

/** A preset as POST /save takes it: a new one has no key, and `mode` defaults to `keep`. */
export type PresetInput = Pick<PresetConfig, 'name' | 'setpoint'> & Partial<Pick<PresetConfig, 'key' | 'mode'>>

/** The stored document for one controller. */
export interface ControllerDocument {
  /**
   * CONFIG_VERSION, or the higher number of a file a newer firmware wrote: that thermostat runs,
   * but its Save is 409, since it would drop what this firmware does not know.
   */
  version: number
  /**
   * Moves on only when a calibration writes new gains; a Save or any other write never moves it,
   * and a new thermostat has 0. A Save that sends one other than the
   * device's is 409 (STALE_DOCUMENT): the form was read before, and would write the old values back.
   */
  revision: number
  /** Immutable slug and the document's file name; a rename never changes it. */
  id: string
  /**
   * The climate entity's name, which is also how web_server, the dashboard and
   * Home Assistant tell it apart: a rename is a new entity in Home Assistant.
   */
  name: string
  enabled: boolean
  kind: ControlKind
  sensor_id: string
  update_interval_s: number
  heat: OutputConfig
  cool: OutputConfig
  visual: VisualConfig
  safety: SafetyConfig
  pid: PidConfig
  bang_bang: BangBangConfig
  mode: ClimateHubMode
  /** One target for both algorithms; bang-bang derives its band from it. */
  setpoint: number
  /** At most PRESET_MAX_COUNT, in the order Home Assistant lists the custom ones. */
  presets: PresetConfig[]
  /**
   * The key of the preset picked last, "" for none. A target or a mode set by hand keeps it.
   * It is the thermostat's state, not the form's: /save ignores it.
   */
  active_preset: string
}

/** A document before the device has given it an id, and its presets their keys. */
export type ControllerDraft = Omit<ControllerDocument, 'id' | 'version' | 'revision' | 'presets' | 'active_preset'> &
  Partial<Pick<ControllerDocument, 'version' | 'revision' | 'active_preset'>> & { presets: PresetInput[] }

/** `T` with every key optional, in nested objects too. */
export type DeepPartial<T> = { [K in keyof T]?: T[K] extends object ? DeepPartial<T[K]> : T[K] }

/** A direction that names its relay; its other keys take their defaults. */
export type DrivenOutputInput = Pick<OutputConfig, 'relay_id'> & DeepPartial<OutputConfig>

/**
 * What POST /save takes. An absent or empty `id` creates a controller, an existing
 * one replaces that controller's document. Only `name` and `sensor_id` are required,
 * and `heat.relay_id` or `cool.relay_id` must name a relay; any other key left out
 * takes its default, and every number is clamped to the schema's range. `presets`
 * left out, or null, is none: a Save without them removes them.
 */
export type ControllerSaveInput = DeepPartial<
  Omit<ControllerDocument, 'id' | 'name' | 'sensor_id' | 'heat' | 'cool' | 'presets'>
> &
  Pick<ControllerDocument, 'name' | 'sensor_id'> & { id?: string; presets?: PresetInput[] | null } & (
    | { heat: DrivenOutputInput; cool?: DeepPartial<OutputConfig> }
    | { heat?: DeepPartial<OutputConfig>; cool: DrivenOutputInput }
  )

/**
 * What POST /import takes: a document as GET /get answers it, or as a backup's file holds it,
 * under the slug `id` it is to have. A thermostat with that id is replaced, otherwise one is
 * created with it. Left-out keys take their defaults as on /save; the presets' keys and
 * `active_preset` are kept, since rules name them, and a preset without a key gets one from
 * its name. `version` is not read: the device writes its own.
 */
export type ControllerImportInput = ControllerSaveInput & { id: string }

/** One row of GET /list. The climate entity it drives carries the same `name`. */
export interface ControllerSummary {
  id: string
  name: string
  enabled: boolean
  kind: ControlKind
  mode: ClimateHubMode
  sensor_id: string
  heat_relay_id: string
  cool_relay_id: string
  /** Enabled and bound to its sensor and relays. Enabled but not running, it waits: see `waiting`. */
  running: boolean
  /**
   * Why an enabled thermostat does not run, the latest reason in a SaveResponse.warning's words
   * ("not started: sensor 'attic' not found"); "" when it runs or is disabled.
   */
  waiting: string
  /** The key of the preset picked last, "" for none; kept while the thermostat is stopped. */
  active_preset: string
  /** That preset's name, "" for none. */
  active_preset_name: string
}

/** Terms are `null` rather than NaN — JSON has no NaN and ArduinoJson emits null. */
export interface PidTerms {
  error: number | null
  proportional: number | null
  integral: number | null
  derivative: number | null
  in_deadband: boolean
}

/** Where a calibration is: running, or ended with gains (`succeeded`) or without (`failed`). */
export type AutotuneState = 'running' | 'succeeded' | 'failed'

/**
 * Why a calibration ended without gains: `cancelled`; a target or a mode that changed, from
 * anywhere (`target_changed`, `mode_changed`); a Save (`saved`); the thermostat stopped
 * (`stopped`) or another one took its relay over (`taken_over`); a fault (`sensor_stale`,
 * `overtemp`, `relay_contested`); 24 hours in all (`timeout`); 6 hours without a relay switch
 * (`no_switch`); readings that crossed the target more than 65 times, a noisy probe at the target
 * (`noisy`). '' while it runs and after a success.
 */
export type AutotuneReason =
  | ''
  | 'cancelled'
  | 'target_changed'
  | 'mode_changed'
  | 'saved'
  | 'stopped'
  | 'taken_over'
  | 'sensor_stale'
  | 'overtemp'
  | 'relay_contested'
  | 'timeout'
  | 'no_switch'
  | 'noisy'

/** The tuning rule that turns Ku and Pu into gains; `zn_pi` when a start names none. */
export type AutotuneRule = 'zn_pi' | 'zn_pid' | 'pessen' | 'some_overshoot' | 'no_overshoot'

/** The relay a calibration swings: one direction per run. */
export type AutotuneDirection = 'heat' | 'cool'

/**
 * What warns about a result; none ever makes a run longer. `asymmetric`: the room rose and fell at
 * very different rates (the shortest half-period under 0.66 of the longest). `uneven`: the swings
 * differed (the smallest under 0.66 of the largest), something else moved the room. `clamped`: a
 * gain had to be held inside its range.
 */
export type AutotuneFlag = 'asymmetric' | 'uneven' | 'clamped'

export interface PidGains {
  kp: number
  ki: number
  kd: number
}

/** The far end of one swing: the room's coldest or warmest in a relay phase, and when. */
export interface AutotuneExtreme {
  /** Seconds since the start. */
  at_s: number
  temperature: number
}

/** A thermostat's last calibration since boot, kept until the next one, a delete or a reboot. */
export interface AutotuneStatus {
  state: AutotuneState
  reason: AutotuneReason
  direction: AutotuneDirection
  rule: AutotuneRule
  /**
   * The target the run swings the room around: the thermostat's when it started, kept after a
   * later target ended it or moved on. `aim` and `extremes` are around it.
   */
  setpoint: number
  /** Whether the run has its relay closed now; null when it does not run, or has no reading yet. */
  phase: 'on' | 'off' | null
  /** The reading that switches the relay next, AUTOTUNE_NOISEBAND from the target; null as `phase`. */
  aim: number | null
  /** Relay switches so far; a run finds its gains at the AUTOTUNE_SWINGS-th. */
  swings: number
  /** Since the start, up to the end once it ended. */
  elapsed_s: number
  /** One per relay phase that ended, in order, the first one's too: the swing to chart. */
  extremes: AutotuneExtreme[]
  /** The ultimate gain and period (seconds) it measured; null until it succeeded. */
  ku: number | null
  pu: number | null
  flags: AutotuneFlag[]
  /** The gains in force when it started. */
  old: PidGains
  /** What it wrote in their place, null unless it succeeded. */
  new: PidGains | null
  /**
   * False when the gains run but did not reach the file, so a reboot loses them; true again once
   * the thermostat is written, by the device's retry or any later change.
   */
  persisted: boolean
}

export interface ControllerStatus {
  id: string
  running: boolean
  /** As ControllerSummary.waiting: `fault` is a running thermostat's alone. */
  waiting: string
  action: ClimateHubAction
  fault: ClimateHubFault
  /** The bound sensor's reading, running or not. null, not NaN — JSON has no NaN. */
  current_temperature: number | null
  sensor_age_s: number | null
  setpoint: number
  /** As ControllerSummary.active_preset and active_preset_name. */
  active_preset: string
  active_preset_name: string
  /** The visual range, carried here so a list card can draw a stepper off /status alone. */
  min_temperature: number
  max_temperature: number
  step: number
  /** The derived switching points. Absent unless the controller is bang-bang. */
  switch_low?: number
  switch_high?: number
  heat_duty: number
  cool_duty: number
  heat_relay_on: boolean
  cool_relay_on: boolean
  /** Which minimum holds each relay, to show "waiting (min off)" beside the action. */
  heat_relay_wait: RelayWait
  cool_relay_wait: RelayWait
  /** Present only while a PID controller runs. */
  pid?: PidTerms
  /** Present once it has been calibrated since boot, running or not; see AutotuneStatus. */
  autotune?: AutotuneStatus
}

/** One tunable number, as the device describes it. Drives the form directly. */
export interface ParamDesc {
  key: string
  label: string
  unit: string
  group: string
  def: number
  min: number
  max: number
  step: number
  integer: boolean
  /** The algorithm this knob belongs to, or "" when both use it. */
  kind: ControlKind | ''
  /** One sentence the form shows on hover. */
  hint: string
}

/** What a preset may be. Its name follows `name_max_length`; its target, the thermostat's visual range. */
export interface PresetSchema {
  /** Presets per thermostat; a Save with more is refused. */
  max_count: number
  /** `keep` first, then the modes; a preset's mode must be one its thermostat's relays serve. */
  modes: PresetMode[]
  /** Home Assistant's built-in presets, in the order STANDARD_PRESETS has them. */
  standard: StandardPresetName[]
}

export interface ClimateSchema {
  kinds: ControlKind[]
  modes: ClimateHubMode[]
  faults: ClimateHubFault[]
  /** How many thermostats the firmware has room for; a create past it is 507. */
  max_controllers: number
  /** Longest name the device accepts, in characters (printable ASCII only). */
  name_max_length: number
  presets: PresetSchema
  /** Parameters by group, in the order the form shows them. */
  params: Record<string, ParamDesc[]>
}

export interface BindableSensor {
  object_id: string
  name: string
  unit: string
}

export interface BindableSwitch {
  object_id: string
  name: string
  /**
   * Id of the running controller holding this relay, or "" when none runs on it. An enabled
   * controller that waits still reserves the relays it names (see ControllerSummary): saving
   * or enabling another on one is a 409 too.
   */
  claimed_by: string
}

export interface ClimateEntitiesResponse {
  success: true
  sensors: BindableSensor[]
  switches: BindableSwitch[]
}

export interface ControllersResponse {
  success: true
  count: number
  max_controllers: number
  controllers: ControllerSummary[]
}

export interface StatusResponse {
  success: true
  controllers: ControllerStatus[]
}

export interface SuccessResponse {
  success: true
  message: string
}

/**
 * POST /save and POST /import: the id of the controller created, updated or replaced. The
 * controllers that waited for a relay it let go start, and `message` names them before the
 * warning.
 */
export interface SaveResponse extends SuccessResponse {
  id: string
  /**
   * Present when it was saved enabled but does not run, and why: its sensor or a
   * relay is not on the device ("not started: sensor 'attic' not found") or no
   * climate entity was free. It waits; `message` repeats this after a `;`.
   */
  warning?: string
}

/**
 * POST /delete: `persisted` is false when it is gone but its file is not, so a reboot brings it
 * back. The controllers that waited for its relays start, and `message` names them.
 */
export interface DeleteResponse extends SuccessResponse {
  persisted: boolean
}

/**
 * POST /enable: `persisted` is false when the change is live but the flag did not reach flash.
 * `message` names who a take-over stopped and who started on a relay the change freed, before
 * the warning.
 */
export interface EnableResponse extends SuccessResponse {
  persisted: boolean
  /** Present when it was enabled but does not run, worded and repeated as SaveResponse.warning. */
  warning?: string
}

/**
 * POST /preset: `persisted` is false when the pick changed the target, the mode or the label of a
 * thermostat whose file a newer firmware wrote: a reboot undoes it.
 */
export interface PresetResponse extends SuccessResponse {
  persisted: boolean
}

export interface ErrorResponse {
  success: false
  error: string
}

/** A save body larger than this is refused with 413 before it is parsed. */
export const CONFIG_MAX_BYTES = 8192

/** The file format this contract describes; a document with a higher `version` came from a newer firmware. */
export const CONFIG_VERSION = 3

/** The 409 error of a Save over a document whose `version` is above CONFIG_VERSION, in the device's words. */
export const NEWER_FILE = 'A newer firmware wrote this thermostat; update the firmware to change it'

/**
 * The 409 error of a Save whose `revision` is not the device's, in the device's words: the form
 * was read before a calibration wrote new gains.
 */
export const STALE_DOCUMENT = 'The device changed this thermostat since it was read; reload it'

/** The tuning rules, in the order the dashboard offers them; the first is the default. */
export const AUTOTUNE_RULES: ReadonlyArray<AutotuneRule> = ['zn_pi', 'zn_pid', 'pessen', 'some_overshoot', 'no_overshoot']

/** A calibration closes the relay this far under the target and opens it this far over (heating). */
export const AUTOTUNE_NOISEBAND = 0.25

/** The relay switch at which a calibration has measured enough. */
export const AUTOTUNE_SWINGS = 6

/** Presets per thermostat; the same number /schema serves. */
export const PRESET_MAX_COUNT = 8

/** Home Assistant's built-in presets, as /schema serves them. */
export const STANDARD_PRESETS: ReadonlyArray<StandardPresetName> = [
  'eco',
  'away',
  'boost',
  'comfort',
  'home',
  'sleep',
  'activity'
]

/** Longest thermostat name, in characters; the same number /schema serves. */
export const NAME_MAX_LENGTH = 48

/** Longest `sensor_id` or `relay_id`: no object id is longer. */
export const ENTITY_ID_MAX_LENGTH = 120
