// The firmware's MQTT settings rules (mqtt_config's validate() and parse_patch(), and the
// messages a save answers with), with its messages verbatim, so a form can refuse what the
// device would before the round trip. The device stays the authority: keep the two in step.
import type { MqttDiscoveryCleanup, MqttSettingsUpdate } from './types'

/** Size limits in UTF-8 bytes. broker and client_id take ASCII only, so there bytes are characters. */
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
  if (r.discovery_cleanup === 'pending') return S.cleaningPending
  return S.saved
}
