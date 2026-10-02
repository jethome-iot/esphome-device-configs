// In-process mock of the device's MQTT settings and subscription slots: GET/POST
// /api/device/mqtt and /api/device/mqtt/subscriptions, and the MQTT parts of /status and
// /capabilities. The dev-server and unit-test double for the contract in ../device/mqttApi.ts
// and ../device/types.ts. Dependency-free; the dev-server glue lives in the dashboard's
// vite.config.ts.
//
// Behaviour follows mqtt_config: the first enable in a boot connects at once unless the
// effective topic prefix differs from the one this boot started with, later changes wait for
// reboot(), and turning discovery off while it runs removes the Home Assistant entries without
// one. Slots follow mqtt_subscriptions: a save waits for reboot(), and what runs reads what
// deliver() publishes while connected, the retained message again on every connect. A number
// reads as strtof() reads it, except a hexadecimal one with a fraction or an exponent. Time moves only on tick(now): a dev
// server calls it with Date.now() before each request, a test when it wants an attempt over.
import type {
  MqttDiscoveryCleanup,
  MqttError,
  MqttLiveStatus,
  MqttReservedNames,
  MqttSettings,
  MqttSettingsUpdate,
  MqttSlot,
  MqttSlotFields,
  MqttSlotState,
  MqttState,
  MqttSubscriptions
} from '../device/types'
import {
  MQTT_SAVE_MESSAGES,
  SLOT_DEFAULTS,
  SLOT_SAVE_MESSAGES,
  parseMqttPatch,
  parseSlotBody,
  sameSlot,
  saveMessage,
  slotNameConflict,
  slotPending,
  utf8Bytes,
  validateSettings,
  type SlotNamedEntity
} from '../device/mqttRules'

/** How long a connection attempt takes, in tick() time. */
export const MQTT_MOCK_CONNECT_MS = 1500

/** A broker under this reserved domain fails the way an unknown host name does. */
export const MQTT_MOCK_DNS_FAIL_SUFFIX = '.invalid'

/** The states the mock can start in; leaving the mode unset (or empty) is the factory state. */
export const MQTT_MOCK_MODES = ['connected', 'unreachable', 'refused', 'held_back', 'noslots', 'none'] as const
export type MqttMockMode = (typeof MQTT_MOCK_MODES)[number]

/** The firmware's units for a Number slot (mqtt_subscriptions' DEFAULT_UNITS). */
export const MQTT_MOCK_UNITS = [
  '°C', '°F', '%', 'W', 'kW', 'Wh', 'kWh', 'V', 'A', 'Hz', 'lx', 'ppm', 'µg/m³', 'hPa',
  'Pa', 'bar', 'm³', 'L', 'L/min', 'm³/h', 's', 'min', 'h', 'dB', 'dBm', 'mm', 'm/s'
]

/** The web_server sorting group the slots' entities sit in. */
export const MQTT_SORTING_GROUP = { name: 'MQTT', sorting_weight: 35 } as const

/** A running slot as web_server's REST and /events carry it. */
export interface MqttSlotEntityState {
  domain: 'sensor' | 'binary_sensor' | 'text_sensor'
  name: string
  /** As web_server words it: `21.4 °C`, `ON`, the text; `NA` while unknown. */
  state: string
  value: number | boolean | string | null
  uom?: string
  sorting_group: string
  sorting_weight: number
}

export interface MqttMockOptions {
  /**
   * Unset: the factory state. `connected`: on and connected, three slots set up and reading.
   * `unreachable`: on, and the broker never answers. `refused`: on, and the broker refuses the
   * login. `held_back`: on with discovery, held back after repeated crashes until reboot().
   * `noslots`: the factory state on a firmware without mqtt_subscriptions. `none`: a firmware
   * without MQTT, so both routes are a 404 and neither /status nor /capabilities has it.
   * The dev server passes VITE_MOCK_MQTT as it is, so any string is taken here (`string & {}`
   * keeps the modes as completions) and anything but these throws.
   */
  mode?: MqttMockMode | (string & {})
  /** The node name, MAC-suffixed as a device has it: the default topic prefix. */
  nodeName: string
  /** The base MAC, `AA:BB:CC:DD:EE:FF`: the default client id ends with it. */
  mac: string
  /** A firmware with mqtt_subscriptions and this many slots (the devices have 8). Left out,
   *  one without, as `noslots` says too. */
  maxSlots?: number
  /** What a Number slot may show; MQTT_MOCK_UNITS by default. */
  units?: readonly string[]
  /** The device's other entities, whose names a slot cannot take; not the slots' own. */
  entities?: () => readonly SlotNamedEntity[]
  /** The names temperature probes take; the devices' `Temp 1` … `Temp 16` by default, null
   *  for a firmware without probes. */
  reservedNames?: MqttReservedNames | null
}

export interface MqttMockResult {
  status: number
  body: unknown
}

export interface MqttMockStore {
  /**
   * One request; undefined when `pathname` is not this store's. `body` is the raw text and
   * `contentType` the request's header: the device checks both before it reads the JSON.
   */
  handle(method: string, pathname: string, body: string, contentType?: string): MqttMockResult | undefined
  /** /status `mqtt`; undefined on a firmware without MQTT. */
  status(): MqttLiveStatus | undefined
  /** /status `reboot_required`, and `reboot_reasons: ['mqtt']` when true. */
  rebootRequired(): boolean
  /** The keys this store adds to /capabilities. */
  capabilities(): { mqtt?: true; mqtt_subscriptions?: { max_slots: number } }
  /** A message from the broker; `retain` keeps it for every later connect. Read only while
   *  connected, as on the device. */
  deliver(topic: string, payload: string, retain?: boolean): void
  /** The running slots' entities, for the dev server's entity list and /events. */
  slotEntities(): MqttSlotEntityState[]
  /** Moves the clock; an attempt ends MQTT_MOCK_CONNECT_MS after it began. */
  tick(now: number): void
  /** A restart: what is saved runs. A cleanup in progress finishes first, as on the device. */
  reboot(): void
  /** The settings go back to the factory state, then a restart. */
  factoryReset(): void
}

interface MqttRecord {
  enabled: boolean
  broker: string
  port: number
  username: string
  password: string
  client_id: string
  topic_prefix: string
  discovery: boolean
  /** Entries from an earlier discovery are still on the broker. */
  clean_pending: boolean
}

const ROUTE = '/api/device/mqtt'
const SLOTS_ROUTE = '/api/device/mqtt/subscriptions'
const PAYLOAD_MAX = 2048
const TEXT_MAX = 255
const RAW_MAX = 64
const MAX_BODY_BYTES = 4096
const SETTING_KEYS = ['enabled', 'broker', 'port', 'username', 'password', 'client_id', 'topic_prefix', 'discovery'] as const
const FACTORY: Readonly<MqttRecord> = {
  enabled: false,
  broker: '',
  port: 1883,
  username: '',
  password: '',
  client_id: '',
  topic_prefix: '',
  discovery: false,
  clean_pending: false
}

const err = (status: number, error: string): MqttMockResult => ({ status, body: { success: false, error } })

// The device's test: the media type before any parameter, case and padding aside.
function saysJson(contentType: string | undefined): boolean {
  return (contentType ?? '').split(';')[0]!.trim().toLowerCase() === 'application/json'
}

// --- Reading a message the way a slot does (payload.cpp, without its byte-level corners) ---

interface SlotReading {
  /** null: unknown. undefined: leave the value as it was. */
  value: number | boolean | string | null | undefined
  error: string | null
}

const NULL_LIKE = new Set(['', 'nan', 'none', 'null', 'unknown', 'unavailable'])
const lowerAscii = (s: string): string => s.replace(/[A-Z]/g, (c) => c.toLowerCase())
const trimAscii = (s: string): string => s.replace(/^[ \t\n\r\f\v]+|[ \t\n\r\f\v]+$/g, '')

function cutBytes(text: string, max: number): string {
  let out = ''
  let bytes = 0
  for (const ch of text) {
    const n = utf8Bytes(ch)
    if (bytes + n > max) break
    out += ch
    bytes += n
  }
  return out
}

function locate(payload: string, path: string): { json: boolean; value: unknown } | { error: string } {
  if (path === '') return { json: false, value: trimAscii(payload) }
  let node: unknown
  try {
    node = JSON.parse(payload)
  } catch {
    return { error: 'not JSON' }
  }
  for (const key of path.split('.')) {
    if (Array.isArray(node) && /^[0-9]{1,9}$/.test(key) && Number(key) < node.length) node = node[Number(key)]
    else if (node !== null && typeof node === 'object' && !Array.isArray(node) && Object.prototype.hasOwnProperty.call(node, key))
      node = (node as Record<string, unknown>)[key]
    else return { error: `key '${path}' not found` }
  }
  return { json: true, value: node }
}

// strtof() takes infinities and NaN in any case, and no 0b or 0o; Number() the other way round.
const STRTOF_NOT_FINITE = /^[+-]?(inf|infinity|nan(\([0-9a-z_]*\))?)$/i
const NOT_STRTOF = /^[+-]?0[bo]/i

function numberFrom(text: string): SlotReading {
  const t = trimAscii(text)
  if (NULL_LIKE.has(lowerAscii(t)) || STRTOF_NOT_FINITE.test(t)) return { value: null, error: null }
  const n = NOT_STRTOF.test(t) ? Number.NaN : Number(t)
  if (Number.isNaN(n)) return { value: null, error: 'not a number' }
  return { value: Number.isFinite(n) ? n : null, error: null }
}

function readSlot(slot: MqttSlotFields, payload: string): SlotReading {
  const keep = slot.kind === 'text_sensor' ? undefined : null
  if (utf8Bytes(payload) > PAYLOAD_MAX) return { value: keep, error: 'message over 2 KiB' }
  if (payload === '') return { value: slot.kind === 'text_sensor' ? '' : null, error: null }
  const at = locate(payload, slot.json_path)
  if ('error' in at) return { value: keep, error: at.error }
  const v = at.value
  if (slot.kind === 'sensor') {
    if (!at.json) return numberFrom(v as string)
    if (v === null) return { value: null, error: null }
    if (typeof v === 'boolean') return { value: v ? 1 : 0, error: null }
    if (typeof v === 'number') return { value: Number.isFinite(v) ? v : null, error: null }
    if (typeof v === 'string') return numberFrom(v)
    return { value: null, error: 'not a number' }
  }
  if (slot.kind === 'binary_sensor') {
    if (at.json && v === null) return { value: null, error: null }
    const text = typeof v === 'string' ? v : JSON.stringify(v)
    if (lowerAscii(text) === lowerAscii(slot.payload_on)) return { value: true, error: null }
    if (lowerAscii(text) === lowerAscii(slot.payload_off)) return { value: false, error: null }
    // A JSON bool, or a bare payload of one; a string at a JSON path is text.
    if (v === true || (!at.json && text === 'true')) return { value: true, error: null }
    if (v === false || (!at.json && text === 'false')) return { value: false, error: null }
    return { value: null, error: 'neither ON nor OFF' }
  }
  return { value: cutBytes(typeof v === 'string' ? v : JSON.stringify(v), TEXT_MAX), error: null }
}

/** What a slot read last, this boot. */
interface SlotRun {
  has: boolean
  at: number
  raw: string
  error: string | null
  value: number | boolean | string | null
}

const freshRun = (): SlotRun => ({ has: false, at: 0, raw: '', error: null, value: null })

export function createMqttMockStore(o: MqttMockOptions): MqttMockStore {
  const mode = o.mode ?? ''
  if (mode !== '' && !(MQTT_MOCK_MODES as readonly string[]).includes(mode)) {
    // A typo would otherwise start the factory state and look like a working mode.
    throw new Error(`Unknown MQTT mock mode '${mode}': use one of ${MQTT_MOCK_MODES.join(', ')}, or leave it unset`)
  }
  const served = mode !== 'none'
  const defaultClientId = `${o.nodeName}-${o.mac.replace(/:/g, '').toLowerCase()}`
  const defaultPrefix = o.nodeName
  const effClientId = (r: MqttRecord) => r.client_id || defaultClientId
  const effPrefix = (r: MqttRecord) => r.topic_prefix || defaultPrefix

  let stored: MqttRecord = { ...FACTORY }
  // What this boot runs, defaults filled in. Kept while not started for the boot's prefix.
  let applied: MqttRecord = { ...FACTORY }
  let started = false
  /** The crash guard holds the client back this boot; a restart retries it. */
  let heldBack = mode === 'held_back'
  let connected = false
  let lastError: MqttError | null = null
  /** When the attempt in flight began; null when none is. */
  let attemptSince: number | null = null
  /** A discovery cleanup is due: it runs while connected and waits otherwise. */
  let cleanup = false
  let clock = 0

  // Slots: what is saved, what this boot runs, what each running one read, and the broker's
  // retained messages, which outlive a reboot.
  const slotsServed = served && mode !== 'noslots' && o.maxSlots !== undefined
  const maxSlots = o.maxSlots ?? 0
  const units = o.units ?? MQTT_MOCK_UNITS
  const otherEntities = o.entities ?? (() => [])
  const reservedNames = o.reservedNames === undefined ? { prefix: 'Temp', count: 16 } : o.reservedNames
  let savedSlots: MqttSlotFields[] = Array.from({ length: maxSlots }, () => ({ ...SLOT_DEFAULTS }))
  let runningSlots: MqttSlotFields[] = savedSlots.map((slot) => ({ ...slot }))
  let runs: SlotRun[] = runningSlots.map(freshRun)
  const retained = new Map<string, string>()

  function endAttempt(): void {
    attemptSince = null
    if (mode === 'unreachable') lastError = 'unreachable'
    else if (mode === 'refused') lastError = 'not_authorized'
    else lastError = applied.broker.endsWith(MQTT_MOCK_DNS_FAIL_SUFFIX) ? 'dns' : null
    connected = lastError === null
    // Every connect subscribes afresh, and the broker hands over what it retained.
    if (connected) for (const [topic, payload] of retained) dispatch(topic, payload)
  }

  const slotRuns = (i: number): boolean => runningSlots[i]!.enabled && runningSlots[i]!.topic !== ''

  function dispatch(topic: string, payload: string): void {
    runningSlots.forEach((slot, i) => {
      if (!slotRuns(i) || slot.topic !== topic) return
      const reading = readSlot(slot, payload)
      const run = runs[i]!
      run.has = true
      run.at = clock
      run.raw = cutBytes(payload.replace(/[\t\n\r]/g, ' ').replace(/[\u0000-\u001f\u007f-\u009f]/g, '?'), RAW_MAX)
      run.error = reading.error
      if (reading.value !== undefined) run.value = reading.value
    })
  }

  function slotValueText(i: number): string {
    const slot = runningSlots[i]!
    const value = runs[i]!.value
    if (!slotRuns(i) || value === null) return ''
    if (slot.kind === 'sensor') return `${(value as number).toFixed(slot.decimals)}${slot.unit ? ` ${slot.unit}` : ''}`
    if (slot.kind === 'binary_sensor') return value ? 'On' : 'Off'
    return value as string
  }

  function slotState(i: number): MqttSlotState {
    if (!slotRuns(i)) return 'off'
    const run = runs[i]!
    if (!connected || !run.has) return 'waiting'
    return run.error === null ? 'ok' : 'error'
  }

  const pendingAt = (i: number): boolean => slotPending(savedSlots[i]!, runningSlots[i]!, slotRuns(i))
  const slotsWait = (): boolean => slotsServed && savedSlots.some((_, i) => pendingAt(i))

  function subscriptions(): MqttSubscriptions {
    return {
      max_slots: maxSlots,
      reboot_required: slotsWait(),
      suspended: false,
      file_error: null,
      units: [...units],
      reserved_names: reservedNames && { ...reservedNames },
      slots: savedSlots.map(
        (slot, i): MqttSlot => ({
          slot: i + 1,
          ...slot,
          pending: pendingAt(i),
          entity: slotRuns(i) ? { domain: runningSlots[i]!.kind, name: runningSlots[i]!.name } : null,
          status: {
            state: slotState(i),
            value: slotValueText(i),
            raw: runs[i]!.raw,
            error: slotRuns(i) ? runs[i]!.error : null,
            age_s: runs[i]!.has ? Math.max(0, Math.floor((clock - runs[i]!.at) / 1000)) : null
          }
        })
      )
    }
  }

  function saveSlotBody(parsed: Record<string, unknown>): MqttMockResult {
    const body = parseSlotBody(parsed, maxSlots, units)
    if (!body.ok) return err(400, body.error)
    const i = body.slot - 1
    const next = body.clear ? { ...SLOT_DEFAULTS } : body.fields
    if (!body.clear) {
      const conflict = slotNameConflict(i, next, savedSlots, otherEntities(), reservedNames)
      if (conflict) return err(400, conflict)
    }
    const answer = (message: string): MqttMockResult => ({
      status: 200,
      // The slots' own, as the device's answer: /status is what adds MQTT's.
      body: { success: true, message, reboot_required: slotsWait() }
    })
    if (sameSlot(savedSlots[i]!, next)) return answer(SLOT_SAVE_MESSAGES.unchanged)
    savedSlots[i] = next
    const M = SLOT_SAVE_MESSAGES
    if (body.clear) return answer(slotRuns(i) ? M.clearedForReboot(body.slot) : M.cleared(body.slot))
    return answer(pendingAt(i) ? M.savedForReboot(body.slot) : M.saved(body.slot))
  }

  function bootSlots(): void {
    runningSlots = savedSlots.map((slot) => ({ ...slot }))
    runs = runningSlots.map(freshRun)
  }

  function start(): void {
    applied = { ...stored, client_id: effClientId(stored), topic_prefix: effPrefix(stored) }
    started = true
    connected = false
    lastError = null
    attemptSince = clock
  }

  function boot(): void {
    applied = { ...stored, client_id: effClientId(stored), topic_prefix: effPrefix(stored) }
    started = false
    connected = false
    lastError = null
    attemptSince = null
    // With discovery on, the entries are announced again anyway.
    cleanup = stored.clean_pending && !(stored.enabled && stored.discovery)
    if (stored.enabled && heldBack) lastError = 'crash_guard'
    else if (stored.enabled) start()
  }

  function finishCleanup(): void {
    cleanup = false
    stored.clean_pending = false
  }

  function state(): MqttState {
    if (!started) return stored.broker === '' ? 'not_configured' : 'off'
    if (connected) return 'connected'
    return lastError === null ? 'connecting' : 'disconnected'
  }

  function cleanupState(): MqttDiscoveryCleanup {
    if (!cleanup) return 'none'
    return connected ? 'running' : 'pending'
  }

  function rebootRequired(): boolean {
    if (!started) return stored.enabled && (heldBack || effPrefix(stored) !== applied.topic_prefix)
    return !(
      stored.enabled === applied.enabled &&
      stored.broker === applied.broker &&
      stored.port === applied.port &&
      stored.username === applied.username &&
      stored.password === applied.password &&
      effClientId(stored) === applied.client_id &&
      effPrefix(stored) === applied.topic_prefix &&
      stored.discovery === applied.discovery
    )
  }

  function settings(): MqttSettings {
    return {
      enabled: stored.enabled,
      broker: stored.broker,
      port: stored.port,
      username: stored.username,
      password_set: stored.password !== '',
      client_id: stored.client_id,
      client_id_default: defaultClientId,
      topic_prefix: stored.topic_prefix,
      topic_prefix_default: defaultPrefix,
      discovery: stored.discovery,
      state: state(),
      last_error: lastError,
      running: started
        ? {
            broker: applied.broker,
            port: applied.port,
            client_id: applied.client_id,
            topic_prefix: applied.topic_prefix,
            status_topic: `${applied.topic_prefix}/status`,
            discovery: applied.discovery
          }
        : null,
      apply_now: !started && !heldBack && effPrefix(stored) === applied.topic_prefix,
      reboot_required: rebootRequired(),
      discovery_cleanup: cleanupState(),
      stored_notice: null
    }
  }

  function update(patch: MqttSettingsUpdate): MqttMockResult {
    const merged: MqttRecord = { ...stored, ...patch }
    if (SETTING_KEYS.every((k) => merged[k] === stored[k])) {
      return {
        status: 200,
        body: {
          success: true,
          message: MQTT_SAVE_MESSAGES.unchanged,
          reboot_required: rebootRequired(),
          started: false,
          discovery_cleanup: cleanupState()
        }
      }
    }
    const invalid = validateSettings({ ...merged, password_set: merged.password !== '' })
    if (invalid) return err(400, invalid)

    // While discovery runs, turning it off, turning MQTT off or moving the broker removes the
    // entries now; discovery off then needs no reboot. Held back, nothing runs, but the broker
    // keeps what earlier boots announced: the next connect removes it.
    const leaves = (from: MqttRecord) =>
      !merged.discovery || !merged.enabled || merged.broker !== from.broker || merged.port !== from.port
    if (started && applied.discovery && leaves(applied)) {
      // Clean mode for the rest of the boot, as on the device: discovery no longer runs, so a
      // save that keeps it on, or puts the broker back, still needs the reboot to announce again.
      cleanup = true
      applied.discovery = false
    } else if (heldBack && stored.enabled && stored.discovery && leaves(stored) && !(merged.enabled && merged.discovery)) {
      cleanup = true
    }
    if (merged.enabled && merged.discovery) merged.clean_pending = false
    else if (cleanup) merged.clean_pending = true
    stored = merged
    // The guard holds back a client that is on; with MQTT off it is no reason for anything.
    if (heldBack) lastError = merged.enabled ? 'crash_guard' : null

    // A changed prefix cannot start live: command topics were subscribed with the boot's.
    let didStart = false
    if (!started && !heldBack && merged.enabled && effPrefix(merged) === applied.topic_prefix) {
      start()
      cleanup = merged.clean_pending && !merged.discovery
      didStart = true
    }
    const result = {
      reboot_required: rebootRequired(),
      started: didStart,
      discovery_cleanup: cleanupState()
    }
    return {
      status: 200,
      body: { success: true, message: saveMessage({ ...result, enabled: merged.enabled }), ...result }
    }
  }

  function handle(method: string, pathname: string, body: string, contentType?: string): MqttMockResult | undefined {
    if (pathname !== ROUTE && pathname !== SLOTS_ROUTE) return undefined
    const slots = pathname === SLOTS_ROUTE
    if (!(slots ? slotsServed : served)) return err(404, 'Not found')
    const m = method.toUpperCase()
    if (m === 'GET') return { status: 200, body: slots ? subscriptions() : settings() }
    if (m !== 'POST') return err(405, 'Method not allowed')
    if (!saysJson(contentType)) return err(415, 'Expected Content-Type: application/json')
    if (utf8Bytes(body) > MAX_BODY_BYTES) return err(413, 'Request body over 4 KiB')
    let parsed: unknown
    try {
      parsed = JSON.parse(body)
    } catch {
      parsed = undefined
    }
    if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) return err(400, 'Invalid JSON')
    if (slots) return saveSlotBody(parsed as Record<string, unknown>)
    const patch = parseMqttPatch(parsed as Record<string, unknown>)
    return patch.ok ? update(patch.patch) : err(400, patch.error)
  }

  function tick(now: number): void {
    clock = now
    if (attemptSince !== null && now - attemptSince >= MQTT_MOCK_CONNECT_MS) endAttempt()
    if (connected && cleanup) finishCleanup()
  }

  if (mode === 'connected' || mode === 'unreachable' || mode === 'refused') {
    stored = { ...FACTORY, enabled: true, broker: '192.168.1.10', username: 'jxd', password: 'secret' }
  } else if (mode === 'held_back') {
    stored = { ...FACTORY, enabled: true, broker: '192.168.1.10', discovery: true }
  }
  if (mode === 'connected' && slotsServed && maxSlots >= 3) {
    savedSlots[0] = {
      ...SLOT_DEFAULTS,
      enabled: true,
      name: 'Outdoor temperature',
      topic: 'zigbee2mqtt/outdoor',
      json_path: 'temperature',
      unit: '°C'
    }
    savedSlots[1] = {
      ...SLOT_DEFAULTS,
      enabled: true,
      name: 'Garage door',
      topic: 'zigbee2mqtt/garage_door',
      kind: 'binary_sensor',
      json_path: 'contact',
      payload_on: 'false',
      payload_off: 'true'
    }
    savedSlots[2] = { ...SLOT_DEFAULTS, enabled: true, name: 'Weather', topic: 'weather/summary', kind: 'text_sensor' }
    retained.set('zigbee2mqtt/outdoor', '{"battery":97,"temperature":21.4,"humidity":48}')
    retained.set('zigbee2mqtt/garage_door', '{"battery":100,"contact":true}')
    retained.set('weather/summary', 'Light rain, 9 °C')
  }
  bootSlots()
  boot()
  // A seeded device has been up a while: its first attempt is over.
  if (attemptSince !== null) endAttempt()

  return {
    handle,
    status() {
      if (!served) return undefined
      return { available: true, enabled: started, connected, state: state(), last_error: lastError }
    },
    rebootRequired: () => served && (rebootRequired() || slotsWait()),
    capabilities() {
      if (!served) return {}
      return slotsServed ? { mqtt: true, mqtt_subscriptions: { max_slots: maxSlots } } : { mqtt: true }
    },
    deliver(topic, payload, retain = false) {
      if (retain) {
        if (payload === '') retained.delete(topic)
        else retained.set(topic, payload)
      }
      if (connected) dispatch(topic, payload)
    },
    slotEntities() {
      const out: MqttSlotEntityState[] = []
      runningSlots.forEach((slot, i) => {
        if (!slotRuns(i)) return
        const value = runs[i]!.value
        const text = slotValueText(i)
        const base = { name: slot.name, sorting_group: MQTT_SORTING_GROUP.name, sorting_weight: i + 1 }
        if (slot.kind === 'sensor') out.push({ ...base, domain: 'sensor', state: text || 'NA', value, uom: slot.unit })
        else if (slot.kind === 'binary_sensor')
          out.push({ ...base, domain: 'binary_sensor', state: value === null ? 'NA' : value ? 'ON' : 'OFF', value })
        else out.push({ ...base, domain: 'text_sensor', state: text, value: text })
      })
      return out
    },
    tick,
    reboot() {
      if (connected && cleanup) finishCleanup()
      heldBack = false
      bootSlots()
      boot()
    },
    factoryReset() {
      stored = { ...FACTORY }
      heldBack = false
      savedSlots = savedSlots.map(() => ({ ...SLOT_DEFAULTS }))
      bootSlots()
      boot()
    }
  }
}

// --- Transport: fetch adapter (unit tests) -----------------------------------
/** A fetch backed by `store`, for createMqttApi({ fetchImpl }) with no server. */
export function createMqttMockFetch(store: MqttMockStore): (url: string, init?: RequestInit) => Promise<Response> {
  return async (url, init) => {
    const method = (init?.method ?? 'GET').toUpperCase()
    const contentType = new Headers(init?.headers ?? {}).get('content-type') ?? undefined
    const body = typeof init?.body === 'string' ? init.body : ''
    const result = store.handle(method, new URL(url, 'http://localhost').pathname, body, contentType) ?? {
      status: 404,
      body: { success: false, error: 'Not found' }
    }
    return new Response(JSON.stringify(result.body), {
      status: result.status,
      headers: { 'Content-Type': 'application/json' }
    })
  }
}
