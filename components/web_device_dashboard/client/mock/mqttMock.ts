// In-process mock of the device's MQTT settings: GET/POST /api/device/mqtt and the `mqtt`
// parts of /status and /capabilities. The dev-server and unit-test double for the contract in
// ../device/mqttApi.ts and ../device/types.ts. Dependency-free; the dev-server glue lives in
// the dashboard's vite.config.ts.
//
// Behaviour follows mqtt_config: the first enable in a boot connects at once unless it also
// changes the topic prefix, later changes wait for reboot(), and turning discovery off while
// it runs removes the Home Assistant entries without one. Time moves only on tick(now): a dev
// server calls it with Date.now() before each request, a test when it wants an attempt over.
import type {
  MqttDiscoveryCleanup,
  MqttError,
  MqttLiveStatus,
  MqttSettings,
  MqttSettingsUpdate,
  MqttState
} from '../device/types'
import { MQTT_SAVE_MESSAGES, parseMqttPatch, saveMessage, utf8Bytes, validateSettings } from '../device/mqttRules'

/** How long a connection attempt takes, in tick() time. */
export const MQTT_MOCK_CONNECT_MS = 1500

/** A broker under this reserved domain fails the way an unknown host name does. */
export const MQTT_MOCK_DNS_FAIL_SUFFIX = '.invalid'

export interface MqttMockOptions {
  /**
   * Unset: the factory state. `connected`: on and connected. `unreachable`: on, and the
   * broker never answers. `refused`: on, and the broker refuses the login. `held_back`: on
   * with discovery, held back after repeated crashes until reboot(). `none`: a firmware
   * without MQTT, so the route is a 404 and neither /status nor /capabilities has it.
   */
  mode?: string
  /** The node name, MAC-suffixed as a device has it: the default topic prefix. */
  nodeName: string
  /** The base MAC, `AA:BB:CC:DD:EE:FF`: the default client id ends with it. */
  mac: string
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
  capabilities(): { mqtt?: true }
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

export function createMqttMockStore(o: MqttMockOptions): MqttMockStore {
  const mode = o.mode ?? ''
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

  function endAttempt(): void {
    attemptSince = null
    if (mode === 'unreachable') lastError = 'unreachable'
    else if (mode === 'refused') lastError = 'not_authorized'
    else lastError = applied.broker.endsWith(MQTT_MOCK_DNS_FAIL_SUFFIX) ? 'dns' : null
    connected = lastError === null
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
      apply_now: !started && !heldBack,
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
      cleanup = true
      if (!merged.discovery) applied.discovery = false
    } else if (heldBack && stored.enabled && stored.discovery && leaves(stored) && !(merged.enabled && merged.discovery)) {
      cleanup = true
    }
    if (merged.enabled && merged.discovery) merged.clean_pending = false
    else if (cleanup) merged.clean_pending = true
    stored = merged

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
    if (pathname !== ROUTE) return undefined
    if (!served) return err(404, 'Not found')
    const m = method.toUpperCase()
    if (m === 'GET') return { status: 200, body: settings() }
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
  boot()
  // A seeded device has been up a while: its first attempt is over.
  if (attemptSince !== null) endAttempt()

  return {
    handle,
    status() {
      if (!served) return undefined
      return { available: true, enabled: started, connected, state: state(), last_error: lastError }
    },
    rebootRequired: () => served && rebootRequired(),
    capabilities() {
      return served ? { mqtt: true } : {}
    },
    tick,
    reboot() {
      if (connected && cleanup) finishCleanup()
      heldBack = false
      boot()
    },
    factoryReset() {
      stored = { ...FACTORY }
      heldBack = false
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
