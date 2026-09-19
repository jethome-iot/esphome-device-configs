// Wire shapes of the upstream web_server entity JSON (SSE `state_detail_all` /
// `state` events and the `/{domain}/{id}` REST routes). `id` is the entity's
// display name: ESPHome 2026.x addresses entities by name, not object_id.
//
// Field names/encodings mirror the C++ `*_json_` output in web_server.cpp. Notably: `state` is always the human display string and
// `value` the machine value, BUT some domains emit neither (button) or only one
// (light emits `state` not `value`; climate/event emit no `value`) — so both are
// optional on BaseEntity. Numbers formatted through `value_accuracy_*` arrive as
// JSON strings (see NumberEntity / ClimateEntity temps), not JS numbers.

export interface BaseEntity {
  id: string
  name?: string
  icon?: string
  // Human display string. Emitted by every domain except button (and light sets
  // it to "ON"/"OFF"). Optional because button emits base fields only.
  state?: string
  // Machine value. Optional: light/climate/event/button do not emit it.
  value?: number | boolean | string
  entity_category?: number
  is_disabled_by_default?: boolean
  // detail=all — web_server `sorting_groups` membership, when the entity has one.
  sorting_group?: string
  sorting_weight?: number
}

export interface SensorEntity extends BaseEntity {
  value: number
  uom?: string // Unit of measurement
  device_class?: string // detail=all — HA device class (power, energy, temperature, humidity, …)
}

export interface BinarySensorEntity extends BaseEntity {
  value: boolean
  device_class?: string // detail=all — HA binary_sensor device class (motion, door, …)
}

export interface TextSensorEntity extends BaseEntity {
  value: string
}

export interface SwitchEntity extends BaseEntity {
  value: boolean
  assumed_state?: boolean
}

export interface UpdateEntity extends BaseEntity {
  value: string // latest/available version (NOT current — see current_version)
  current_version?: string
  title?: string
  summary?: string
  release_url?: string
}

export interface SelectEntity extends BaseEntity {
  value: string
  option?: string[] // options list — field name is singular `option`
}

/** The display mode a number entity asks its frontend for, mirroring
 *  `number::NumberMode` in the firmware (number/number_traits.h) — the wire values
 *  `number_handler.h` emits as `mode` at detail=all. AUTO leaves the choice to the
 *  frontend; BOX asks for numeric entry; SLIDER asks for a slider. */
export const NUMBER_MODE = {
  AUTO: 0,
  BOX: 1,
  SLIDER: 2
} as const

export type NumberMode = (typeof NUMBER_MODE)[keyof typeof NUMBER_MODE]

export interface NumberEntity extends BaseEntity {
  value: string // formatted number string, or "NaN"
  min_value?: string // detail=all — formatted strings, not numbers
  max_value?: string
  step?: string
  mode?: NumberMode // detail=all
  uom?: string
}

export interface ButtonEntity extends BaseEntity {
  // Button emits base fields only (id + name/icon/category at detail=all).
  // No state, no value; press-only.
  _button?: never
}

/** Nested `color` object on a light; keys present depend on color_mode. */
export interface LightColor {
  r?: number // 0-255
  g?: number
  b?: number
  w?: number
  c?: number
}

export interface LightEntity extends BaseEntity {
  // No scalar `value`; on/off is in `state` ("ON"|"OFF").
  color_mode?: string // onoff|brightness|white|color_temp|cwww|rgb|rgbw|rgbct|rgbww
  brightness?: number // 0-255
  color?: LightColor
  white_value?: number // 0-255 (legacy top-level)
  color_temp?: number // mireds
  effect?: string
  effect_index?: number
  effect_count?: number
  effects?: string[] // detail=all — always starts with "None"
}

export interface FanEntity extends BaseEntity {
  value: boolean
  speed_level?: number // if supports_speed
  speed_count?: number
  oscillation?: boolean // if supports_oscillation
}

export interface CoverEntity extends BaseEntity {
  value: number // position 0.0-1.0; state is "OPEN"|"CLOSED"
  current_operation?: string // IDLE|OPENING|CLOSING
  position?: number // 0.0-1.0 (if position supported)
  tilt?: number // 0.0-1.0 (if tilt supported)
}

export interface LockEntity extends BaseEntity {
  value: number // LockState enum int; state is LOCKED|UNLOCKED|JAMMED|...
}

export interface ClimateEntity extends BaseEntity {
  // Temperatures arrive as formatted strings ("NA" when NaN); step is numeric.
  mode?: string
  action?: string // if action supported (also mirrored into `state`)
  current_temperature?: string
  target_temperature?: string // single-point
  target_temperature_low?: string // two-point
  target_temperature_high?: string
  min_temp?: string
  max_temp?: string
  step?: number
  fan_mode?: string
  swing_mode?: string
  preset?: string
  modes?: string[] // detail=all
  fan_modes?: string[]
  swing_modes?: string[]
  presets?: string[]
}

export interface EventEntity extends BaseEntity {
  event_type?: string // last fired event (when present)
  event_types?: string[] // detail=all
  device_class?: string // detail=all
}

// Entity domain discriminator (the domain keys the aggregator groups by).
export type EntityDomain =
  | 'sensor'
  | 'binary_sensor'
  | 'text_sensor'
  | 'switch'
  | 'update'
  | 'select'
  | 'number'
  | 'button'
  | 'light'
  | 'fan'
  | 'cover'
  | 'lock'
  | 'climate'
  | 'event'

// Union of entities rendered in the dashboard entity view.
export type Entity =
  | SensorEntity
  | BinarySensorEntity
  | TextSensorEntity
  | SwitchEntity
  | SelectEntity
  | NumberEntity
  | ButtonEntity
  | LightEntity
  | FanEntity
  | CoverEntity
  | LockEntity
  | ClimateEntity
  | EventEntity
  | UpdateEntity

/**
 * Entity state as delivered over the web_server SSE stream (state / state_detail_all).
 * The SSE transport itself lives in the consumer; the shape is part of the entity
 * contract, so it is owned here. Carries the same domain-specific fields as the
 * REST payload (the backend serialises both through `entity_json_to`), so the
 * index signature keeps it permissive without re-listing every field.
 */
export interface EntityState {
  id: string
  state?: string
  value?: number | boolean | string
  // Extended fields (detail=all)
  name?: string
  icon?: string
  uom?: string
  entity_category?: number
  assumed_state?: boolean
  // Domain hint (present on some SSE payloads / prefixed ids).
  domain?: string
  // Domain-specific fields (brightness, color, position, mode, temps, …).
  [key: string]: unknown
}
