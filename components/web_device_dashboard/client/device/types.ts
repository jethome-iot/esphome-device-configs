// Wire contract of the JetHome device API under /api/device, as the dashboard knows it:
// device identity and status, network, MQTT, and per-entity settings. A firmware
// implements a subset — this one's is openapi.yaml. The app reaches these via the @da alias.

// --- Device identity / runtime status ---

/** What jethome_board_info read from the EEPROM, verbatim, plus the chip's own eFuses. Only
 *  `valid` is present when the header failed its checks. */
export interface BoardInfo {
  valid: boolean
  header_version?: number
  boardname?: string
  boardversion?: string
  /** v4 headers only; a v3 header keeps the device serial here and it goes to `serial_number`. */
  board_serial?: string
  usid?: string
  /** The chip's factory MAC as the factory wrote it, `AA:BB:CC:DD:EE:FF`. */
  cpuid?: string
  /** The chip's custom MAC as the factory wrote it, `AABBCCDDEEFF`. */
  mac?: string
  timestamp?: number
  /** 0 on a board that was never signed; otherwise `signature` is the factory signature, hex. */
  signature_version?: number
  signature?: string
  /** The `device.id` record, the product's identity; null on a board without one. */
  identity?: {
    model: string
    serial: string
    hw_revision: string
    /** Absent when there was nothing to compare, so false always means a real mismatch. */
    serial_matches_usid?: boolean
  } | null
  /** The chip's eFuses, in the two formats above. Null when the eFuse could not be read. */
  efuse?: { factory_mac: string | null; custom_mac: string | null }
}

export interface DeviceInfo {
  /** Device serial: from the signed `device.id` record, or from the EEPROM header on a
   *  board that predates it, where production put the device serial by convention. */
  serial_number?: string
  /** Effective display name — the override if set, else the compiled `friendly_name`.
   *  The same string HA and MQTT discovery announce. */
  name: string
  /** The compiled `friendly_name`, i.e. what `name` falls back to. Present only
   *  when the firmware stores a device-name override, so its presence is also what
   *  tells the dashboard the name is editable. */
  default_name?: string
  base_mac_address: string
  mac_address?: string
  /** Product model from the signed `device.id` record. */
  device_model?: string
  /** Product hardware revision from `device.id`, alongside `device_model`. */
  hw_revision?: string
  /** The CPU board's EEPROM identity; absent when the firmware has no jethome_board_info wired
   *  in. */
  board?: BoardInfo
  version?: string
  compilation_time?: string
  /** Ordered object_ids the firmware marks as important for the Overview
   *  ("featured"). Device-declared via the web_device_api `overview_group:` option
   *  (a groups reference); the backend resolves the group's members to object_ids
   *  at request time. The Overview renders them regardless of entity_category. If
   *  absent/empty the Overview shows nothing featured — there is no client-side
   *  heuristic fallback. Ids are object_ids (name slugs), the same key
   *  /api/entities uses — NOT the ESPHome config id:. */
  featured_entities?: string[]
  /** Device-declared Entities-screen cards, in the order the screen renders them.
   *  Declared via the web_device_api `cards:` option. Entities no card claims fall
   *  through to the dashboard's built-in auto-categorization; a device that
   *  declares no cards is categorized entirely by it. */
  cards?: DeviceCard[]
  /** object_ids the Entities screen must not show at all, resolved by the backend
   *  from every `hide:` predicate's `entities:` + `groups:`. Hiding is applied
   *  before any card distribution, so a hidden entity reaches neither a declared
   *  card nor the auto-categorized ones. It only affects the Entities screen —
   *  other screens (e.g. the Overview Network card) still address these entities
   *  by object_id. */
  hidden_members?: string[]
  /** Predicate half of `hide:` — one entry per declared `hide:` item that carries a
   *  `filter:`. An entity is hidden when it matches ANY entry (the entries are a
   *  union; axes WITHIN an entry are an AND, see CardFilter). Evaluated
   *  client-side, so entities created at runtime are hidden too. */
  hidden_filters?: CardFilter[]
  /** object_ids whose control must be confirmed before the dashboard issues its
   *  command, resolved by the backend from every `confirm:` predicate's `entities:`
   *  + `groups:`. It gates the command, not the display: the row renders as usual,
   *  but the first interaction only arms a `Confirm` step, and the command
   *  reports itself in a toast once the device answers.
   *
   *  Applies wherever the dashboard renders its shared control widget — a button
   *  press, a switch toggle, a select change. Two controls draw themselves and
   *  call the API directly, so a rule naming them has no effect: the Entities
   *  screen's own thermostat block, and the firmware-type dropdown in Settings. */
  confirm_members?: string[]
  /** Predicate half of `confirm:` — one entry per declared `confirm:` item that
   *  carries a `filter:`. An entity is gated when it matches ANY entry (the entries
   *  are a union; axes WITHIN an entry are an AND, see CardFilter). Evaluated
   *  client-side, so entities created at runtime are gated too. */
  confirm_filters?: CardFilter[]
  /** The device's `groups:` that declare an `icon:`, in declaration order. Their
   *  members inherit that icon for their row (see DeviceGroup). Groups without an
   *  icon are not carried — they have nothing a row can read. Absent when no group
   *  declares one. */
  groups?: DeviceGroup[]
}

// --- Device-declared Entities-screen cards ---

/** The icon vocabulary a device may name in `cards: - icon:`. Material Design Icon
 *  names — the same vocabulary ESPHome entities and Home Assistant already use, so
 *  a config author writes the name they know rather than a dashboard-private one.
 *  The dashboard draws its own glyph for each (see CARD_GLYPH); it does not ship
 *  MDI artwork, which is filled where the dashboard's icon set is stroked.
 *
 *  This array is the single source of truth for the vocabulary: web_device_api's
 *  __init__.py parses it at codegen time to validate `icon:` (an unknown name fails
 *  `esphome compile`), and the dashboard maps it with an exhaustive Record keyed by
 *  CardIcon (a name with no glyph fails `vue-tsc`). Add a name here first. */
export const CARD_ICONS = [
  'mdi:sine-wave',
  'mdi:current-ac',
  'mdi:flash',
  'mdi:angle-acute',
  'mdi:gauge',
  'mdi:counter',
  'mdi:thermometer',
  'mdi:water-percent',
  'mdi:tune',
  'mdi:cog',
  'mdi:toggle-switch',
  'mdi:electric-switch',
  'mdi:power-plug',
  'mdi:gesture-tap-button',
  'mdi:login',
  'mdi:chart-line',
  'mdi:lightbulb',
  'mdi:fan',
  'mdi:lock',
  'mdi:lan-connect',
  'mdi:wifi',
  'mdi:clock-outline',
  'mdi:harddisk',
  'mdi:bell',
  'mdi:tag',
  'mdi:information-outline',
  'mdi:alert'
] as const

export type CardIcon = (typeof CARD_ICONS)[number]

/** Membership predicate for a card (or for a `hide:` / `confirm:` entry). An entity matches when
 *  it satisfies EVERY specified axis; within one axis, being in the listed set is
 *  enough. Applied client-side against the live entity store, so a predicate card
 *  tracks entities that appear at runtime.
 *
 *  The axis order also drives row order inside a card: members are grouped by which
 *  `device_classes` entry they matched, then by `domains`, then by `categories`,
 *  and within a group they keep the order /api/entities returned them in.
 *
 *  Note `device_classes` only ever matches sensor / binary_sensor / event — those
 *  are the only domains whose REST payload carries a device_class. */
export interface CardFilter {
  /** ESPHome domains: sensor, binary_sensor, switch, number, select, button, … */
  domains?: string[]
  /** device_class names: voltage, current, power, reactive_power, energy, … */
  device_classes?: string[]
  /** entity_category: 0 = NONE, 1 = CONFIG, 2 = DIAGNOSTIC. */
  categories?: number[]
}

/** One device-declared card on the Entities screen (see DeviceInfo.cards).
 *
 *  Cards are rendered in declaration order and claim entities in that same order:
 *  the first card an entity matches takes it, and no later card — nor the built-in
 *  auto-categorization — sees it again. Rows within a card run `members` (the
 *  explicit `entities:` then whole `groups:`, de-duplicated, in declaration order)
 *  followed by whatever `filter` matched. A card that ends up with no rows is not
 *  rendered. */
export interface DeviceCard {
  /** Stable identity for the card (the dashboard's list key) — a slug of the title,
   *  suffixed if two titles slug alike. */
  key: string
  /** Card heading. */
  title: string
  /** Optional icon (see CARD_ICONS). */
  icon?: CardIcon
  /** Explicit member object_ids — name slugs, the key /api/entities uses, NOT the
   *  ESPHome config `id:`. Resolved by the backend from the card's `entities:` and
   *  `groups:` at request time, so runtime members of a referenced group (e.g.
   *  Dallas probes) are included. Empty for a pure `filter` card. */
  members: string[]
  /** Optional predicate membership, unioned with `members`. */
  filter?: CardFilter
}

/** One `groups:` group that declares an `icon:` (see DeviceInfo.groups).
 *
 *  It exists on the wire for one reason: a row whose entity carries no device_class
 *  and no `icon:` of its own — every relay and every digital input on a controller —
 *  would otherwise fall through to its domain glyph, drawing a whole card of
 *  identical pictures. The group says what those entities are.
 *
 *  Resolution for a row: device_class → the entity's own `icon:` → this group icon →
 *  unit → domain. An entity in several icon-bearing groups takes the FIRST declared
 *  one, which is what keeps a broad group (an `overview_group:` holding relays AND
 *  inputs) from overriding the specific ones — leave the broad group's icon unset
 *  and it is not on the wire at all. */
export interface DeviceGroup {
  /** The group's `name:`, for identification only — rows are matched by member id. */
  name: string
  /** The icon its members inherit (see CARD_ICONS). Validated by web_device_api at
   *  compile time; the fork's `groups` component itself accepts any MDI name. */
  icon: CardIcon
  /** Member object_ids — name slugs, the key /api/entities uses, NOT the ESPHome
   *  config `id:`. Resolved at request time, so a group's runtime members (e.g.
   *  Dallas probes) are included. */
  members: string[]
}

/** Which group of saved-but-unapplied settings is waiting for a restart. */
export type RebootReason = 'network' | 'wifi' | 'mqtt' | 'auth' | 'api'

export interface DeviceStatus {
  /** An API client is connected. Absent — not false — on a build with no API server. */
  ha_connected?: boolean
  uptime_s: number
  reset_reason: string
  connection_type: 'wifi' | 'ethernet' | 'none'
  ip_address: string | null
  rssi: number | null
  /** A saved MQTT setting or subscription slot waits for a restart, or MQTT or its
   *  subscriptions are held back and a restart retries them; `reboot_reasons` names it.
   *  Modbus and Network mode, which also apply on restart, are not tracked. */
  reboot_required: boolean
  /** Omitted when nothing is waiting. This firmware names only `mqtt`. */
  reboot_reasons?: RebootReason[]
  /** The MQTT client as it runs this boot. Absent on a build without MQTT. */
  mqtt?: MqttLiveStatus
  /** `available` = the API can be turned off here. No `connected`: see ha_connected. */
  api?: {
    available: boolean
    enabled: boolean
  }
}

/** Envelope every mutating route answers with. */
export interface MutationResponse {
  success: boolean
  message?: string
  /** Set by a route whose change can wait for a restart: POST /mqtt, /mqtt/subscriptions. */
  reboot_required?: boolean
}

/** POST /name — set the display name, or clear the override with `reset: true`. A union,
 *  because the device rejects both `{}` and `{ reset: false }`.
 *  Trimmed, printable ASCII, 1..32 chars; the compiled name clears the override. */
export type DeviceNameUpdate = { name: string; reset?: false } | { reset: true; name?: never }

/** Body required by the confirm-gated system actions (reboot/rollback/factory-reset).
 *  `confirm_token` is the last three octets of `DeviceInfo.base_mac_address`, e.g.
 *  `"DD:EE:FF"` — matched in either case. NOT `mac_address`, which on a build with Ethernet
 *  is a different MAC. A wrong or missing one is `403`, a body without
 *  `confirm: true` is `400`. It is a guard against a stray POST, not an authorization
 *  scheme: whatever reaches the port and can read /info can send it. */
export interface ConfirmPayload {
  confirm: true
  confirm_token: string
}

// --- Capabilities ---

/** The mount the Files screen browses and a factory reset wipes. What it is, not how full
 *  it is: for live usage call `info` on the file API at `Capabilities.files.url_prefix`. */
export interface StorageCapability {
  /** `"LittleFS"` on a device; whatever the storage component calls itself. */
  type: string
  base_path: string
  /** False when the partition failed to mount: the Files screen has nothing to show. */
  mounted: boolean
}

/**
 * GET /capabilities — what this firmware has, so the page knows which screens to draw and
 * which routes exist. Read on load, not polled; read again where `rollback` may have moved.
 *
 * A key is present only when the capability is, so `if (caps.files)` is the test; one that
 * has no detail to carry is `true`. A `404` on this route is a firmware from before it
 * existed, not a device without capabilities.
 */
export interface Capabilities {
  /** POST /system/reboot is served. Always true — every device can reboot. */
  reboot: true
  /** POST /system/factory-reset is served. `clears_storage` says whether it also wipes the
   *  user partition, which is what the confirmation dialog has to warn about. */
  factory_reset: { clears_storage: boolean }
  /** The slot POST /system/rollback would boot: after an update the firmware it replaced,
   *  after a rollback the newer one. Absent after a serial flash, a failed or interrupted
   *  update, a rollback the bootloader did itself, or while a switch waits for its reboot;
   *  then that route answers `503`. `version` is the ESPHome version string of that image,
   *  comparable to `DeviceInfo.version` — show it, because after one rollback the other slot
   *  is the NEWER firmware. */
  rollback?: {
    /** The partition label, e.g. `"app1"`. */
    partition: string
    version?: string
    /** The name the image was built under, the device's node name, e.g. `"jxd-r6-e1eth-lcd"`. */
    project_name?: string
  }
  /** Present with a storage component; then `factory_reset.clears_storage` is true. */
  storage?: StorageCapability
  /** Present when the firmware serves the file API the Files screen uses. */
  files?: { url_prefix: string }
  /** Present when the firmware serves the automations API the Automations screen uses. */
  automations?: { url_prefix: string }
  /** Present with a `config_json` store; `types` are the keys `/entity-settings` takes. */
  entity_settings?: { types: string[] }
  /** The CPU board's EEPROM identity is in `/info`. */
  board_info?: true
  /** GET/POST /mqtt are served. */
  mqtt?: true
  /** GET/POST /mqtt/subscriptions are served, with this many slots. */
  mqtt_subscriptions?: { max_slots: number }
}

// --- Network (live status + saved config) ---

export interface NetworkLiveStatus {
  hostname: string
  connection_type: 'wifi' | 'ethernet' | 'none'
  ip_address: string | null
  gateway: string | null
  subnet: string | null
  dns1: string | null
  dns2: string | null
  ssid: string | null
  rssi: number | null
  ethernet_connected: boolean
}

export interface ManualIPConfig {
  static_ip: string
  gateway: string
  subnet: string
  dns1: string
  dns2: string
}

export interface SavedWifiNetwork {
  ssid: string
  has_password: boolean
  priority: number
}

export interface SavedWifiNetworkInput {
  ssid: string
  password?: string
  priority: number
}

/** One access point found by a WiFi scan. */
export interface WifiScanNetwork {
  ssid: string
  rssi: number
  encryption: 'open' | 'wpa2'
  /** True when this SSID is already in the saved network list. */
  known: boolean
}

/**
 * WiFi scan result, returned by both POST (start) and GET (poll) on the scan
 * endpoint. `scanning: true` means a scan is in flight (poll again);
 * `scanning: false` carries the results of the last completed scan.
 *
 * The device only starts a fresh scan when WiFi is not the active link — scanning
 * re-associates the radio and would drop a live WiFi connection. When WiFi IS the
 * link, POST answers `scanning: false` and the results are whatever the device's
 * own last scan found, which may be an empty list.
 */
export interface WifiScanResponse {
  scanning: boolean
  networks?: WifiScanNetwork[]
}

export interface NetworkSavedConfig {
  network_type: 'wifi' | 'ethernet'
  use_dhcp: boolean
  manual_ip: ManualIPConfig
  wifi_networks: SavedWifiNetwork[]
  /** The native API server starts at boot. Absent on a build with no API server. */
  api_enabled?: boolean
}

export interface NetworkConfigUpdate {
  network_type?: 'wifi' | 'ethernet'
  use_dhcp?: boolean
  manual_ip?: ManualIPConfig
  wifi_networks?: SavedWifiNetworkInput[]
  /** Turns the native API server on/off. Applied at the next boot. */
  api_enabled?: boolean
}

// --- MQTT (mqtt_config) ---

export type MqttState = 'not_configured' | 'off' | 'connecting' | 'connected' | 'disconnected'
/** Why the client is not connected, as far as the firmware can tell. */
export type MqttError =
  | 'dns'
  | 'unreachable'
  | 'connection_lost'
  | 'protocol'
  | 'identifier_rejected'
  | 'server_unavailable'
  | 'bad_credentials'
  | 'not_authorized'
  | 'crash_guard'
/** Removing this device's retained Home Assistant entries from the broker: `running` while
 *  they go out, `pending` while one is due but the broker is not connected. */
export type MqttDiscoveryCleanup = 'none' | 'running' | 'pending'

/** GET /status `mqtt`: the client as it runs this boot. */
export interface MqttLiveStatus {
  available: true
  /** The client was started this boot. */
  enabled: boolean
  connected: boolean
  state: MqttState
  last_error: MqttError | null
}

/** What runs this boot, with the defaults filled in. */
export interface MqttRunning {
  broker: string
  port: number
  client_id: string
  topic_prefix: string
  /** `<topic_prefix>/status`: `online` / `offline`. */
  status_topic: string
  discovery: boolean
}

/** GET /mqtt: the stored settings (never the password), and what runs this boot. */
export interface MqttSettings {
  enabled: boolean
  broker: string
  port: number
  username: string
  password_set: boolean
  /** '' = `client_id_default`. */
  client_id: string
  client_id_default: string
  /** '' = `topic_prefix_default`. */
  topic_prefix: string
  topic_prefix_default: string
  /** Home Assistant discovery. */
  discovery: boolean
  state: MqttState
  last_error: MqttError | null
  /** Null while the client was not started this boot. */
  running: MqttRunning | null
  /** A save that turns MQTT on connects at once: the client has not started this boot, is not
   *  held back, and the effective topic prefix is still the one this boot started with. A save
   *  that changes that prefix waits for a reboot all the same. */
  apply_now: boolean
  /** A saved change waits for a restart, or MQTT is held back and a restart retries it. */
  reboot_required: boolean
  discovery_cleanup: MqttDiscoveryCleanup
  /** `newer_firmware`: the stored fields show the defaults and MQTT is off. `invalid: …`: the
   *  stored record breaks a rule of this firmware, runs anyway, and the next save must fix it. */
  stored_notice: 'newer_firmware' | `invalid: ${string}` | null
}

/** POST /mqtt: an absent key is unchanged; `password: ''` clears the stored one. */
export interface MqttSettingsUpdate {
  enabled?: boolean
  broker?: string
  port?: number
  username?: string
  password?: string
  client_id?: string
  topic_prefix?: string
  discovery?: boolean
}

/** POST /mqtt's 200. `message` is one of MQTT_SAVE_MESSAGES (mqttRules.ts). */
export interface MqttSaveResult extends MutationResponse {
  reboot_required: boolean
  /** This save started the client: it is connecting now, no reboot needed. */
  started: boolean
  discovery_cleanup: MqttDiscoveryCleanup
}

// --- MQTT subscription slots (mqtt_subscriptions) ---

/** The entity a slot runs as, by its domain: Number, On/Off and Text in the dashboard. */
export type MqttSlotKind = 'sensor' | 'binary_sensor' | 'text_sensor'
/** `off`: runs nothing this boot (disabled, empty, breaking a rule, no room). `suspended`: its
 *  entity runs but nothing is subscribed after repeated crashes. `waiting`: MQTT is not
 *  connected or no message came yet. `error`: the last message could not be read. */
export type MqttSlotState = 'off' | 'waiting' | 'ok' | 'error' | 'suspended'

/** A slot's settings. `unit` and `decimals` are a sensor's, the payloads a binary_sensor's;
 *  for the other kinds they hold their defaults. An empty slot has `topic: ''`. */
export interface MqttSlotFields {
  enabled: boolean
  /** The entity's name: at most 32 bytes, no `/`, trimmed. */
  name: string
  /** One exact topic: at most 128 bytes, no `+` or `#`. */
  topic: string
  kind: MqttSlotKind
  /** Up to 6 keys separated by `.`; a number picks a list element. '' reads the whole message. */
  json_path: string
  /** One of `MqttSubscriptions.units`, or ''. */
  unit: string
  /** 0–4. */
  decimals: number
  payload_on: string
  payload_off: string
}

/** What a slot runs as this boot. */
export interface MqttSlotEntity {
  domain: MqttSlotKind
  /** The name web_server serves it under: join entities on it, not on the saved name. */
  name: string
}

export interface MqttSlotStatus {
  state: MqttSlotState
  /** As the device shows it: `21.5 °C`, `On`, the text; '' while unknown. */
  value: string
  /** The first 64 bytes of the last message, stray bytes and control characters as `?`. */
  raw: string
  /** Why the slot does not run (`invalid: …`, `no room in the entity table`) or why the last
   *  message could not be read (`not JSON`, `key 'a.b' not found`, `not a number`, `neither ON
   *  nor OFF`, `not UTF-8`, `message over 2 KiB`). */
  error: string | null
  /** Seconds since the last message; null before one. */
  age_s: number | null
}

/** One slot of GET /mqtt/subscriptions: the saved fields, and what runs. */
export interface MqttSlot extends MqttSlotFields {
  /** 1-based. */
  slot: number
  /** The saved slot differs from what runs: it changes at the next restart. */
  pending: boolean
  entity: MqttSlotEntity | null
  status: MqttSlotStatus
}

/** `newer_firmware`: no slot runs and saves are refused. `unreadable`: the file cannot be read;
 *  the next save sets it aside. The renamed form: set aside at boot. `unavailable`: the storage
 *  is not mounted. */
export type MqttSlotFileError =
  | 'newer_firmware'
  | 'unreadable'
  | 'unreadable: renamed to subscriptions.json.bad'
  | 'unavailable'

/** GET /mqtt/subscriptions. */
export interface MqttSubscriptions {
  max_slots: number
  /** A saved slot differs from what runs, or the slots are suspended and a restart retries them. */
  reboot_required: boolean
  /** After two crashes in a row while connected, nothing is subscribed this boot. */
  suspended: boolean
  file_error: MqttSlotFileError | null
  /** The units a sensor slot may take. */
  units: string[]
  /** Always `max_slots` entries, slot 1 first. */
  slots: MqttSlot[]
}

/** POST /mqtt/subscriptions: one slot, whole. Left-out optional fields take their defaults. */
export type MqttSlotSave = Pick<MqttSlotFields, 'enabled' | 'name' | 'topic' | 'kind'> &
  Partial<Pick<MqttSlotFields, 'json_path' | 'unit' | 'decimals' | 'payload_on' | 'payload_off'>> & {
    slot: number
  }

/** POST /mqtt/subscriptions: empty one slot. */
export interface MqttSlotClear {
  slot: number
  action: 'clear'
}

/** POST /mqtt/subscriptions's 200. `message` is one of SLOT_SAVE_MESSAGES (mqttRules.ts). */
export interface MqttSlotSaveResult extends MutationResponse {
  reboot_required: boolean
}

// --- Auth (web_auth) ---

export interface AuthStatus {
  username: string
  /** Characters in the stored password; the password itself is never answered. */
  password_length: number
  /** Still the pair the firmware was built with. */
  is_default: boolean
}

// --- Entity settings (per-entity user config) ---

export interface EntitySettingsFieldOption {
  value: string
  label: string
}

export interface EntitySettingsFieldDef {
  type: 'string' | 'boolean' | 'enum' | 'number'
  label: string
  /** Short help text; the dashboard shows it as a tooltip beside the label. */
  description?: string
  options?: EntitySettingsFieldOption[]
  default?: unknown
  min?: number
  max?: number
  unit?: string
  display_unit?: string
  display_factor?: number
}

export interface EntitySettingsMetaResponse {
  /** Fields every type shares; the device emits none today. */
  common?: Record<string, EntitySettingsFieldDef>
  settings: Record<string, Record<string, EntitySettingsFieldDef>>
}

export interface EntitySettingsRecord {
  source_name: string
  [field: string]: unknown
}

export interface EntitySettingsGetResponse {
  type: string
  records: EntitySettingsRecord[]
}

// --- Entity index (object_id <-> name) ---

/** One settable entity. `source_name` is the object_id the entity-settings records
 *  are keyed by; `name` is the display name the web_server REST and SSE use. */
export interface EntityIndexEntry {
  source_name: string
  name: string
}

/** GET /entities — settable entities per settings type (`switch`, `binary_sensor`, ...). */
export type EntityIndexResponse = Record<string, EntityIndexEntry[]>
