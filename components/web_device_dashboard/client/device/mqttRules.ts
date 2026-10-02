// The firmware's MQTT settings rules (mqtt_config's validate() and parse_patch(), and the
// messages a save answers with), with its messages verbatim, so a form can refuse what the
// device would before the round trip. The device stays the authority: keep the two in step.
import type { MqttDiscoveryCleanup, MqttReservedNames, MqttSettingsUpdate, MqttSlotFields, MqttSlotKind } from './types'

/** Size limits in UTF-8 bytes. broker and client_id take ASCII only, so their bytes are characters. */
export const MQTT_LIMITS = { broker: 128, username: 64, password: 128, client_id: 64, topic_prefix: 64 } as const

/** validate()'s messages, in its order: the first rule that fails is the answer. */
export const MQTT_MESSAGES = {
  brokerRequired: "'broker' is required to turn MQTT on",
  brokerTooLong: "'broker' is over 128 characters",
  brokerUrl: "'broker' takes a host name, not a URL: drop the 'mqtt://'",
  brokerPort: "'broker' cannot carry a port: put it in 'port'",
  brokerChars: "'broker' must be a host name or an IPv4 address",
  portRange: "'port' must be between 1 and 65535",
  usernameTooLong: "'username' is over 64 bytes",
  usernameText: "'username' must be text without control characters",
  passwordTooLong: "'password' is over 128 bytes",
  passwordNul: "'password' cannot contain a NUL character",
  passwordNeedsUsername: "'password' needs a 'username': MQTT 3.1.1 sends none without one",
  clientIdTooLong: "'client_id' is over 64 characters",
  clientIdChars: "'client_id' must be printable ASCII without spaces",
  topicPrefixTooLong: "'topic_prefix' is over 64 bytes",
  topicPrefixWildcard: "'topic_prefix' cannot contain '+' or '#'",
  topicPrefixText: "'topic_prefix' must be text without control characters",
  topicPrefixDollar: "'topic_prefix' cannot start with '$'",
  topicPrefixSlash: "'topic_prefix' cannot end with '/'"
} as const

type MqttStringKey = 'broker' | 'username' | 'password' | 'client_id' | 'topic_prefix'

/** parse_patch()'s messages: a key it does not know, or a value of the wrong JSON type. */
export const MQTT_PATCH_MESSAGES = {
  unknownKey: (key: string): string => `'${key}' is not an MQTT setting`,
  notBoolean: (key: 'enabled' | 'discovery'): string => `'${key}' must be true or false`,
  notString: (key: MqttStringKey): string => `'${key}' must be a string`,
  portNotWhole: "'port' must be a whole number"
} as const

/** A 200's `message`, picked by saveMessage(). */
export const MQTT_SAVE_MESSAGES = {
  started: 'Saved; MQTT is connecting',
  cleaningThenReboot: "Saved; removing this device's Home Assistant entries now, the rest applies after a reboot",
  rebootEntriesStay:
    "Saved; applies after a reboot. The broker is not reachable, so this device's Home Assistant entries stay on it unless it comes back before then",
  reboot: 'Saved; applies after a reboot',
  cleaning: "Saved; removing this device's Home Assistant entries",
  entriesStayUntilOn: "Saved; this device's Home Assistant entries stay on the broker until MQTT is turned on again",
  cleaningPending: "Saved; this device's Home Assistant entries go once the broker is reachable",
  saved: 'Saved',
  unchanged: 'Nothing changed'
} as const

/** The 500 when the settings could not be stored. */
export const MQTT_STORE_FAILED = 'Storing the MQTT settings failed; the old ones stay in force'

const BOOLEAN_KEYS: ReadonlySet<string> = new Set(['enabled', 'discovery'])
const STRING_KEYS: ReadonlySet<string> = new Set(['broker', 'username', 'password', 'client_id', 'topic_prefix'])

const M = MQTT_MESSAGES
const encoder = new TextEncoder()

export function utf8Bytes(s: string): number {
  return encoder.encode(s).length
}

// Valid UTF-8 (no lone surrogate) and no control character: U+0000–U+001F, U+007F–U+009F.
function isText(s: string): boolean {
  for (const ch of s) {
    const cp = ch.codePointAt(0) ?? 0
    if (cp <= 0x1f || (cp >= 0x7f && cp <= 0x9f) || (cp >= 0xd800 && cp <= 0xdfff)) return false
  }
  return true
}

/** `ipv6` is what the build was compiled with; the JXD builds have IPv6 off. */
export function checkBroker(broker: string, enabled: boolean, ipv6 = false): string | null {
  if (broker === '') return enabled ? M.brokerRequired : null
  if (utf8Bytes(broker) > MQTT_LIMITS.broker) return M.brokerTooLong
  if (broker.includes('://')) return M.brokerUrl
  // With IPv6 an address has two colons or more, so only a lone one is a port.
  const colons = broker.split(':').length - 1
  if (ipv6 ? colons === 1 : colons > 0) return M.brokerPort
  if (!(ipv6 ? /^[A-Za-z0-9._:-]+$/ : /^[A-Za-z0-9._-]+$/).test(broker)) return M.brokerChars
  return null
}

/** Takes what the body carried, so a string or a fraction gets parse_patch()'s message. */
export function checkPort(port: unknown): string | null {
  // An integer past int64 is not one to the device either.
  if (typeof port !== 'number' || !Number.isInteger(port) || port >= 2 ** 63 || port < -(2 ** 63)) {
    return MQTT_PATCH_MESSAGES.portNotWhole
  }
  return port < 1 || port > 65535 ? M.portRange : null
}

export function checkUsername(username: string): string | null {
  if (utf8Bytes(username) > MQTT_LIMITS.username) return M.usernameTooLong
  return isText(username) ? null : M.usernameText
}

export function checkPassword(password: string): string | null {
  if (utf8Bytes(password) > MQTT_LIMITS.password) return M.passwordTooLong
  return password.includes('\0') ? M.passwordNul : null
}

/** `passwordWillBeSet`: the save leaves a password stored, typed now or kept from before. */
export function checkCredentialsPair(username: string, passwordWillBeSet: boolean): string | null {
  return passwordWillBeSet && username === '' ? M.passwordNeedsUsername : null
}

/** '' is valid: the firmware's default. */
export function checkClientId(clientId: string): string | null {
  if (utf8Bytes(clientId) > MQTT_LIMITS.client_id) return M.clientIdTooLong
  return /^[\x21-\x7e]*$/.test(clientId) ? null : M.clientIdChars
}

/** '' is valid: the firmware's default. */
export function checkTopicPrefix(prefix: string): string | null {
  if (utf8Bytes(prefix) > MQTT_LIMITS.topic_prefix) return M.topicPrefixTooLong
  if (prefix.includes('+') || prefix.includes('#')) return M.topicPrefixWildcard
  if (!isText(prefix)) return M.topicPrefixText
  if (prefix.startsWith('$')) return M.topicPrefixDollar
  return prefix.endsWith('/') ? M.topicPrefixSlash : null
}

/** The settings a save would leave stored. `password` is a new one being sent ('' clears it);
 *  without it, `password_set` says whether the stored one stays. */
export type MqttSettingsDraft = Required<Omit<MqttSettingsUpdate, 'password'>> & {
  password_set: boolean
  password?: string
}

/** The first error in validate()'s order, or null. */
export function validateSettings(s: MqttSettingsDraft, ipv6 = false): string | null {
  const passwordWillBeSet = s.password !== undefined ? s.password !== '' : s.password_set
  return (
    checkBroker(s.broker, s.enabled, ipv6) ??
    checkPort(s.port) ??
    checkUsername(s.username) ??
    (s.password !== undefined ? checkPassword(s.password) : null) ??
    checkCredentialsPair(s.username, passwordWillBeSet) ??
    checkClientId(s.client_id) ??
    checkTopicPrefix(s.topic_prefix)
  )
}

export type MqttPatchResult = { ok: true; patch: MqttSettingsUpdate } | { ok: false; error: string }

/** parse_patch() on a body already known to be a JSON object: keys in the body's order, the
 *  first bad one is the answer. Types only; validateSettings() judges the values. */
export function parseMqttPatch(body: Record<string, unknown>): MqttPatchResult {
  const patch: Record<string, unknown> = {}
  for (const [key, value] of Object.entries(body)) {
    if (key === 'port') {
      // A whole number out of range parses, so validate() can answer with the range.
      if (checkPort(value) === MQTT_PATCH_MESSAGES.portNotWhole) return { ok: false, error: MQTT_PATCH_MESSAGES.portNotWhole }
    } else if (BOOLEAN_KEYS.has(key)) {
      if (typeof value !== 'boolean') return { ok: false, error: MQTT_PATCH_MESSAGES.notBoolean(key as 'enabled' | 'discovery') }
    } else if (STRING_KEYS.has(key)) {
      if (typeof value !== 'string') return { ok: false, error: MQTT_PATCH_MESSAGES.notString(key as MqttStringKey) }
    } else {
      return { ok: false, error: MQTT_PATCH_MESSAGES.unknownKey(key) }
    }
    patch[key] = value
  }
  return { ok: true, patch: patch as MqttSettingsUpdate }
}

/** A 200's message, from what the save did; the first row that matches wins. */
export function saveMessage(r: {
  started: boolean
  reboot_required: boolean
  discovery_cleanup: MqttDiscoveryCleanup
  enabled: boolean
}): string {
  const S = MQTT_SAVE_MESSAGES
  if (r.started) return S.started
  if (r.reboot_required && r.discovery_cleanup === 'running') return S.cleaningThenReboot
  if (r.reboot_required && r.discovery_cleanup === 'pending' && !r.enabled) return S.rebootEntriesStay
  if (r.reboot_required) return S.reboot
  if (r.discovery_cleanup === 'running') return S.cleaning
  if (r.discovery_cleanup === 'pending' && !r.enabled) return S.entriesStayUntilOn
  if (r.discovery_cleanup === 'pending') return S.cleaningPending
  return S.saved
}

// --- Subscription slots (mqtt_subscriptions: read_slot(), validate_slot(), post()) ---

/** Size limits in UTF-8 bytes; `json_path_keys` keys at most, `decimals` 0 to `decimals`. */
export const SLOT_LIMITS = { name: 32, topic: 128, json_path: 64, json_path_keys: 6, payload: 32, decimals: 4 } as const

export const SLOT_KINDS: readonly MqttSlotKind[] = ['sensor', 'binary_sensor', 'text_sensor']

/** A new slot, and what a save fills in for a field it leaves out. */
export const SLOT_DEFAULTS: Readonly<MqttSlotFields> = {
  enabled: false,
  name: '',
  topic: '',
  kind: 'sensor',
  json_path: '',
  unit: '',
  decimals: 1,
  payload_on: 'ON',
  payload_off: 'OFF'
}

/** validate_slot()'s messages, in its order: the first rule that fails is the answer. */
export const SLOT_MESSAGES = {
  nameRequired: "'name' is required",
  nameTooLong: "'name' is over 32 bytes",
  nameText: "'name' must be text without control characters",
  nameSlash: "'name' cannot contain '/'",
  topicRequired: "'topic' is required",
  topicTooLong: "'topic' is over 128 bytes",
  topicText: "'topic' must be text without control characters",
  topicSpace: "'topic' cannot start or end with a space",
  topicWildcard: "'topic' cannot contain '+' or '#': a slot takes one topic",
  jsonPathTooLong: "'json_path' is over 64 bytes",
  jsonPathText: "'json_path' must be text without control characters",
  jsonPathKeys: "'json_path' must be up to 6 keys separated by '.'",
  unitUnknown: "'unit' is not one this firmware offers",
  decimalsRange: "'decimals' must be a whole number from 0 to 4",
  payloadOnLength: "'payload_on' must be 1 to 32 bytes",
  payloadOnText: "'payload_on' must be text without control characters",
  payloadOffLength: "'payload_off' must be 1 to 32 bytes",
  payloadOffText: "'payload_off' must be text without control characters",
  payloadsEqual: "'payload_on' and 'payload_off' must differ"
} as const

/** The names the device checks against what else it has, after validate_slot(). */
export const SLOT_NAME_MESSAGES = {
  otherSlot: (slot: number): string => `'name' gives the same id as slot ${slot}; add a Latin letter or a digit`,
  entity: (name: string): string => `'name' gives the same id as the entity '${name}'`,
  probe: (name: string): string => `'name' gives the same id as '${name}', which a temperature probe takes`
} as const

type SlotStringKey = 'name' | 'topic' | 'json_path' | 'unit' | 'payload_on' | 'payload_off'

/** read_slot() and post()'s messages: the slot number, the action, a key or a type. */
export const SLOT_BODY_MESSAGES = {
  slotRange: (maxSlots: number): string => `'slot' must be a whole number from 1 to ${maxSlots}`,
  action: "'action' must be 'clear'",
  unknownKey: (key: string): string => `'${key}' is not a slot field`,
  notString: (key: SlotStringKey): string => `'${key}' must be a string`,
  notBoolean: "'enabled' must be true or false",
  kind: "'kind' must be 'sensor', 'binary_sensor' or 'text_sensor'"
} as const

/** A 200's `message`. */
export const SLOT_SAVE_MESSAGES = {
  savedForReboot: (slot: number): string => `Slot ${slot} saved; applies after a reboot`,
  saved: (slot: number): string => `Slot ${slot} saved`,
  unchanged: 'Nothing changed',
  clearedForReboot: (slot: number): string => `Slot ${slot} cleared; its entity goes after a reboot`,
  cleared: (slot: number): string => `Slot ${slot} cleared`
} as const

/** The 503s a save can answer besides `Device busy`. */
export const SLOT_STORAGE_UNAVAILABLE = 'Storage unavailable'
export const SLOT_NEWER_FILE = 'The subscriptions file was written by newer firmware; it is left as it is'

const SLOT_STRING_KEYS: ReadonlySet<string> = new Set(['name', 'topic', 'json_path', 'unit', 'payload_on', 'payload_off'])
const SLOT_ECHO_KEYS: ReadonlySet<string> = new Set(['slot', 'pending', 'status', 'entity'])
const SM = SLOT_MESSAGES

// ASCII only, as the device compares.
const asciiLower = (s: string): string => s.replace(/[A-Z]/g, (c) => c.toLowerCase())
const ASCII_SPACE = /^[ \t\n\r\f\v]+|[ \t\n\r\f\v]+$/g
// int64_t on the device: a JSON integer it can hold.
const isWhole = (v: unknown): v is number => typeof v === 'number' && Number.isInteger(v) && Math.abs(v) < 2 ** 63

/** EntityBase's object id: per UTF-8 byte, A–Z lowered, a space and every byte outside
 *  [a-z0-9_-] made `_`, so two Cyrillic names of the same byte length come out alike. */
export function slotObjectId(name: string): string {
  let id = ''
  for (const byte of encoder.encode(name)) {
    const c = byte >= 0x41 && byte <= 0x5a ? byte + 0x20 : byte
    const keep = (c >= 0x61 && c <= 0x7a) || (c >= 0x30 && c <= 0x39) || c === 0x2d || c === 0x5f
    id += keep ? String.fromCharCode(c) : '_'
  }
  return id
}

/** 1 to SLOT_LIMITS.json_path_keys non-empty keys separated by '.'. */
export function jsonPathValid(path: string): boolean {
  const keys = path.split('.')
  return keys.length <= SLOT_LIMITS.json_path_keys && keys.every((key) => key !== '')
}

/** validate_slot(): the slot's own rules, in the device's order; null when it breaks none.
 *  Names other entities already have are slotNameConflict()'s. */
export function validateSlot(slot: MqttSlotFields, units: readonly string[]): string | null {
  if (slot.name === '') return SM.nameRequired
  if (utf8Bytes(slot.name) > SLOT_LIMITS.name) return SM.nameTooLong
  if (!isText(slot.name)) return SM.nameText
  if (slot.name.includes('/')) return SM.nameSlash
  const topic = slot.topic
  if (topic === '') return SM.topicRequired
  if (utf8Bytes(topic) > SLOT_LIMITS.topic) return SM.topicTooLong
  if (!isText(topic)) return SM.topicText
  if (topic.startsWith(' ') || topic.endsWith(' ')) return SM.topicSpace
  if (topic.includes('+') || topic.includes('#')) return SM.topicWildcard
  if (slot.json_path !== '') {
    if (utf8Bytes(slot.json_path) > SLOT_LIMITS.json_path) return SM.jsonPathTooLong
    if (!isText(slot.json_path)) return SM.jsonPathText
    if (!jsonPathValid(slot.json_path)) return SM.jsonPathKeys
  }
  if (slot.kind === 'sensor') {
    if (slot.unit !== '' && !units.includes(slot.unit)) return SM.unitUnknown
    if (!isWhole(slot.decimals) || slot.decimals < 0 || slot.decimals > SLOT_LIMITS.decimals) return SM.decimalsRange
  }
  if (slot.kind === 'binary_sensor') {
    for (const [key, length, text] of [
      ['payload_on', SM.payloadOnLength, SM.payloadOnText],
      ['payload_off', SM.payloadOffLength, SM.payloadOffText]
    ] as const) {
      const bytes = utf8Bytes(slot[key])
      if (bytes === 0 || bytes > SLOT_LIMITS.payload) return length
      if (!isText(slot[key])) return text
    }
    if (asciiLower(slot.payload_on) === asciiLower(slot.payload_off)) return SM.payloadsEqual
  }
  return null
}

/** What the device stores: the fields the kind does not use back at their defaults, and an
 *  empty slot (no topic) all defaults. */
export function normalizeSlot(slot: MqttSlotFields): MqttSlotFields {
  if (slot.topic === '') return { ...SLOT_DEFAULTS }
  const out = { ...slot }
  if (out.kind !== 'sensor') {
    out.unit = SLOT_DEFAULTS.unit
    out.decimals = SLOT_DEFAULTS.decimals
  }
  if (out.kind !== 'binary_sensor') {
    out.payload_on = SLOT_DEFAULTS.payload_on
    out.payload_off = SLOT_DEFAULTS.payload_off
  }
  return out
}

const SLOT_FIELD_KEYS = Object.keys(SLOT_DEFAULTS) as (keyof MqttSlotFields)[]
export function sameSlot(a: MqttSlotFields, b: MqttSlotFields): boolean {
  return SLOT_FIELD_KEYS.every((key) => a[key] === b[key])
}

/** The device's `pending`: the saved slot changes what the next boot runs. `runs` says the slot
 *  has an entity this boot; one that runs nothing and is not enabled in `saved` stays as it is. */
export function slotPending(saved: MqttSlotFields, running: MqttSlotFields, runs: boolean): boolean {
  return !sameSlot(saved, running) && (saved.enabled || runs)
}

/** An entity the device already has, for slotNameConflict(). */
export interface SlotNamedEntity {
  domain: string
  name: string
}

/**
 * The device's name check after validateSlot(): another saved slot (`others`, by index, empty
 * ones skipped), then another entity of the slot's kind, then (a sensor) a temperature probe's
 * name, `<prefix> 1` … `<prefix> <count>`. `index` is the slot's own, 0-based.
 *
 * `entities` must leave out the entities the slots run as (`MqttSlot.entity`), as the device
 * does: the saved slots in `others` stand for them. `probes` is GET's `reserved_names`, null on
 * a firmware without temperature probes.
 */
export function slotNameConflict(
  index: number,
  slot: MqttSlotFields,
  others: readonly MqttSlotFields[],
  entities: readonly SlotNamedEntity[],
  probes: MqttReservedNames | null
): string | null {
  const id = slotObjectId(slot.name)
  for (let j = 0; j < others.length; j++) {
    const other = others[j]!
    if (j !== index && other.topic !== '' && slotObjectId(other.name) === id) return SLOT_NAME_MESSAGES.otherSlot(j + 1)
  }
  const taken = entities.find((e) => e.domain === slot.kind && slotObjectId(e.name) === id)
  if (taken) return SLOT_NAME_MESSAGES.entity(taken.name)
  if (slot.kind === 'sensor' && probes) {
    for (let n = 1; n <= probes.count; n++) {
      const probe = `${probes.prefix} ${n}`
      if (slotObjectId(probe) === id) return SLOT_NAME_MESSAGES.probe(probe)
    }
  }
  return null
}

export type SlotBodyResult =
  | { ok: true; slot: number; clear: true }
  | { ok: true; slot: number; clear: false; fields: MqttSlotFields }
  | { ok: false; error: string }

/**
 * post() up to the file, on a body already known to be a JSON object: the slot number, then
 * `action`, then read_slot()'s types in the body's key order (an unknown key refused, the keys
 * a GET answers with skipped), then validate_slot(). The fields come back normalized.
 */
export function parseSlotBody(body: Record<string, unknown>, maxSlots: number, units: readonly string[]): SlotBodyResult {
  const B = SLOT_BODY_MESSAGES
  const number = body.slot
  if (!isWhole(number) || number < 1 || number > maxSlots) return { ok: false, error: B.slotRange(maxSlots) }
  if (Object.prototype.hasOwnProperty.call(body, 'action')) {
    return body.action === 'clear' ? { ok: true, slot: number, clear: true } : { ok: false, error: B.action }
  }
  const fields: MqttSlotFields = { ...SLOT_DEFAULTS }
  let hasEnabled = false
  let hasKind = false
  for (const [key, value] of Object.entries(body)) {
    if (SLOT_ECHO_KEYS.has(key)) continue
    if (SLOT_STRING_KEYS.has(key)) {
      if (typeof value !== 'string') return { ok: false, error: B.notString(key as SlotStringKey) }
      ;(fields as unknown as Record<string, unknown>)[key] = value
    } else if (key === 'enabled') {
      if (typeof value !== 'boolean') return { ok: false, error: B.notBoolean }
      fields.enabled = value
      hasEnabled = true
    } else if (key === 'kind') {
      if (typeof value !== 'string' || !SLOT_KINDS.includes(value as MqttSlotKind)) return { ok: false, error: B.kind }
      fields.kind = value as MqttSlotKind
      hasKind = true
    } else if (key === 'decimals') {
      if (!isWhole(value)) return { ok: false, error: SM.decimalsRange }
      fields.decimals = value
    } else {
      return { ok: false, error: B.unknownKey(key) }
    }
  }
  if (!hasEnabled) return { ok: false, error: B.notBoolean }
  if (!hasKind) return { ok: false, error: B.kind }
  fields.name = fields.name.replace(ASCII_SPACE, '')
  const invalid = validateSlot(fields, units)
  return invalid ? { ok: false, error: invalid } : { ok: true, slot: number, clear: false, fields: normalizeSlot(fields) }
}
