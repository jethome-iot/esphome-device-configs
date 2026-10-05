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

/** What the controller is doing right now. */
export type ClimateHubAction = 'off' | 'idle' | 'heating' | 'cooling'

/**
 * Why a running controller is not controlling. Never persisted. A sensor or relay
 * the device does not have is no fault: the thermostat waits, and says why in `waiting`.
 */
export type ClimateHubFault = 'none' | 'sensor_stale' | 'overtemp'

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

/** The stored document for one controller. */
export interface ControllerDocument {
  version: number
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
}

/** A document before the device has given it an id. */
export type ControllerDraft = Omit<ControllerDocument, 'id' | 'version'> & Partial<Pick<ControllerDocument, 'version'>>

/** `T` with every key optional, in nested objects too. */
export type DeepPartial<T> = { [K in keyof T]?: T[K] extends object ? DeepPartial<T[K]> : T[K] }

/** A direction that names its relay; its other keys take their defaults. */
export type DrivenOutputInput = Pick<OutputConfig, 'relay_id'> & DeepPartial<OutputConfig>

/**
 * What POST /save takes. An absent or empty `id` creates a controller, an existing
 * one replaces that controller's document. Only `name` and `sensor_id` are required,
 * and `heat.relay_id` or `cool.relay_id` must name a relay; any other key left out
 * takes its default, and every number is clamped to the schema's range.
 */
export type ControllerSaveInput = DeepPartial<Omit<ControllerDocument, 'id' | 'name' | 'sensor_id' | 'heat' | 'cool'>> &
  Pick<ControllerDocument, 'name' | 'sensor_id'> & { id?: string } & (
    | { heat: DrivenOutputInput; cool?: DeepPartial<OutputConfig> }
    | { heat?: DeepPartial<OutputConfig>; cool: DrivenOutputInput }
  )

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
   * Why an enabled thermostat does not run, in the words of the SaveResponse.warning that said
   * so first ("not started: sensor 'attic' not found"); "" when it runs or is disabled.
   */
  waiting: string
}

/** Terms are `null` rather than NaN — JSON has no NaN and ArduinoJson emits null. */
export interface PidTerms {
  error: number | null
  proportional: number | null
  integral: number | null
  derivative: number | null
  in_deadband: boolean
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
  /** Present only while a PID controller runs. */
  pid?: PidTerms
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

export interface ClimateSchema {
  kinds: ControlKind[]
  modes: ClimateHubMode[]
  faults: ClimateHubFault[]
  /** How many thermostats the firmware has room for; a create past it is 507. */
  max_controllers: number
  /** Longest name the device accepts, in characters (printable ASCII only). */
  name_max_length: number
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
  /** Id of the running controller holding this relay, or "" when it is free. */
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

/** POST /save: the id of the controller created or updated. */
export interface SaveResponse extends SuccessResponse {
  id: string
  /**
   * Present when it was saved enabled but does not run, and why: its sensor or a
   * relay is not on the device ("not started: sensor 'attic' not found") or no
   * climate entity was free. It waits; `message` repeats this after a `;`.
   */
  warning?: string
}

/** POST /delete: `persisted` is false when it is gone but its file is not, so a reboot brings it back. */
export interface DeleteResponse extends SuccessResponse {
  persisted: boolean
}

/** POST /enable: `persisted` is false when the change is live but the flag did not reach flash. */
export interface EnableResponse extends SuccessResponse {
  persisted: boolean
  /** Present when it was enabled but does not run, worded and repeated as SaveResponse.warning. */
  warning?: string
}

export interface ErrorResponse {
  success: false
  error: string
}

/** A save body larger than this is refused with 413 before it is parsed. */
export const CONFIG_MAX_BYTES = 8192

/** Longest thermostat name, in characters; the same number /schema serves. */
export const NAME_MAX_LENGTH = 48

/** Longest `sensor_id` or `relay_id`: no object id is longer. */
export const ENTITY_ID_MAX_LENGTH = 120
