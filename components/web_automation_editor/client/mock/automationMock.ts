// In-process mock for the web_automation_editor HTTP API — the dev/QA/test double
// for the contract in ../automationApi.ts + ../types.ts. Lives WITH the SDK so the
// mock, the types, and the client stay one unit and cannot drift apart.
//
// This file is the transport-agnostic CORE and stays dependency-free (client/ has
// no package.json / node_modules): seed data + a stateful dispatcher + a FetchImpl
// adapter. The Vite dev-server glue is thin and env-specific, so it lives in each
// consumer's vite.config.ts (which actually depends on vite) and just wraps
// createAutomationMockStore() — see web/vite.config.ts.
//
// Behaviour mirrors web_automation_editor.cpp (verified): save, delete and reboot
// answer POST only and the rest GET only (405 otherwise); get/save/delete return
// 400/404 with a {success:false,error} body; /ping returns {status:"ok"} (not the
// success envelope).
import type {
  AutomationAction,
  AutomationCondition,
  AutomationConfig,
  AutomationSaveInput,
  AutomationTrigger,
  AutomationEntitiesResponse,
  AutomationSchema
} from '../types'
import type { FetchImpl } from '../automationApi'
import { normalizeCron, validateCronExpression } from '../cron'
import { isNameTaken } from '../naming'

// The firmware does NOT store a posted cron string — it parses each field into an
// integer set and RE-SERIALISES on every read, so `1,7` comes back as `*/6`.
// Reproduce that here so the dev/QA loop sees the same round-trip the hardware
// does. A cron the device would refuse is refused in /save before this runs.
function normalizeStoredCron(cfg: AutomationConfig): AutomationConfig {
  const triggers = cfg.triggers.map((t) =>
    t.source === 'cron' && typeof t.cron === 'string' ? { ...t, cron: normalizeCron(t.cron) } : t
  )
  return { ...cfg, triggers }
}

// --- Seed data ---------------------------------------------------------------
// Typed against the SDK contract, so these double as canonical example payloads
// AND cannot silently drift from ../types.ts (a shape change fails tsc here).
export const seedAutomations: AutomationConfig[] = [
  {
    id: 1,
    name: 'Porch light at sunset',
    enabled: true,
    mode: 'single',
    triggers: [{ source: 'cron', cron: '0 0 18 * * *', cron_preset: 'daily' }],
    condition: { type: 'input', object_id: 'front_door', state: 'true' },
    actions: [{ source: 'switch', type: 'turn_on', object_id: 'porch_light' }],
    else_actions: [{ source: 'switch', type: 'turn_off', object_id: 'porch_light' }]
  },
  {
    id: 2,
    name: 'Fan on when hot',
    enabled: false,
    mode: 'restart',
    triggers: [{ source: 'temperature', type: 'above', object_id: 'room_temp', threshold: 28 }],
    actions: [
      { source: 'switch', type: 'turn_on', object_id: 'fan' },
      { source: 'delay', delay_ms: 300000 },
      { source: 'switch', type: 'turn_off', object_id: 'fan' }
    ]
  },
  {
    id: 3,
    name: 'Porch light follows the fan',
    enabled: true,
    mode: 'single',
    triggers: [{ source: 'switch', type: 'state_change', object_id: 'fan' }],
    actions: [
      { source: 'switch', type: 'follow', object_id: 'porch_light', invert: false },
      { source: 'switch', type: 'follow', object_id: 'relay_3', invert: true }
    ]
  }
]

export const seedEntities: AutomationEntitiesResponse = {
  binary_sensors: [
    { object_id: 'front_door', name: 'Front Door' },
    { object_id: 'motion', name: 'Motion Sensor' }
  ],
  sensors: [
    { object_id: 'room_temp', name: 'Room Temperature', unit: '°C' },
    { object_id: 'humidity', name: 'Humidity', unit: '%' }
  ],
  switches: [
    { object_id: 'porch_light', name: 'Porch Light' },
    { object_id: 'fan', name: 'Fan' },
    { object_id: 'relay_3', name: 'Relay 3' }
  ]
}

export const seedSchema: AutomationSchema = {
  triggers: [
    { type: 'input', subtypes: ['press', 'release', 'click', 'state_change'] },
    { type: 'temperature', subtypes: ['above', 'below', 'range'] },
    { type: 'cron', subtypes: [] },
    { type: 'startup', subtypes: [] },
    { type: 'switch', subtypes: ['turn_on', 'turn_off', 'state_change'] }
  ],
  conditions: [
    { type: 'input' },
    { type: 'temperature', subtypes: ['above', 'below', 'range'] },
    { type: 'and' },
    { type: 'or' },
    { type: 'xor' }
  ],
  actions: [
    { type: 'switch', subtypes: ['turn_on', 'turn_off', 'toggle', 'follow'] },
    { type: 'delay' }
  ],
  cron_presets: ['daily', 'hourly', 'every_n_minutes', 'weekly', 'monthly', 'custom']
}

// --- What the device's deserialize refuses -----------------------------------
// The words come from the same catalog /schema serves; thresholds, object ids and
// the like are not checked, the device resolves those at boot anyway.
function knownType(entries: Array<{ type: string; subtypes?: string[] }>, type: unknown, subtype: unknown): boolean {
  const entry = entries.find((e) => e.type === type)
  if (!entry) return false
  if (!entry.subtypes || entry.subtypes.length === 0) return true
  return typeof subtype === 'string' && entry.subtypes.includes(subtype)
}

function validTrigger(t: AutomationTrigger): boolean {
  if (!knownType(seedSchema.triggers, t.source, t.type)) return false
  return t.source !== 'cron' || validateCronExpression(t.cron ?? '') === null
}

function validAction(a: AutomationAction): boolean {
  return knownType(seedSchema.actions as Array<{ type: string; subtypes?: string[] }>, a.source, a.type)
}

function validCondition(c: AutomationCondition): boolean {
  if (c.type === 'input') return typeof c.object_id === 'string'
  if (c.type === 'temperature') {
    return knownType(seedSchema.conditions as Array<{ type: string; subtypes?: string[] }>, c.type, c.temperature_type)
  }
  if (c.type === 'and' || c.type === 'or' || c.type === 'xor') {
    return Array.isArray(c.conditions) && c.conditions.length > 0 && c.conditions.every(validCondition)
  }
  return false
}

function validRule(cfg: unknown): cfg is AutomationSaveInput {
  if (!cfg || typeof cfg !== 'object') return false
  const c = cfg as Partial<AutomationSaveInput>
  return (
    typeof c.name === 'string' &&
    Array.isArray(c.triggers) &&
    c.triggers.every(validTrigger) &&
    Array.isArray(c.actions) &&
    c.actions.every(validAction) &&
    (c.else_actions === undefined || (Array.isArray(c.else_actions) && c.else_actions.every(validAction))) &&
    (c.condition === undefined || validCondition(c.condition))
  )
}

// The device's id parameter: the whole value, decimal, non-zero.
function idParam(search: URLSearchParams): number | MockResult {
  const raw = search.get('id')
  if (raw === null) return { status: 400, body: { success: false, error: 'Missing id parameter' } }
  if (!/^\d+$/.test(raw) || Number(raw) === 0 || Number(raw) > 0xffffffff) {
    return { status: 400, body: { success: false, error: 'Invalid id parameter' } }
  }
  return Number(raw)
}

const MAX_BODY_BYTES = 16384

// --- Stateful core -----------------------------------------------------------
export interface MockResult {
  status: number
  body: unknown
}

export interface AutomationMockStore {
  /**
   * Dispatch one API call. `endpoint` is the path AFTER the api base, e.g.
   * '/list', '/get'. `method` must match the route, as on the device. `body` is
   * the raw request body (used by /save).
   */
  handle(method: string, endpoint: string, search: URLSearchParams, body: string): MockResult
}

/** Fresh, isolated mock state (seed is deep-copied, so instances never share state). */
export function createAutomationMockStore(): AutomationMockStore {
  const automations: AutomationConfig[] = structuredClone(seedAutomations)
  let nextId = automations.reduce((max, a) => Math.max(max, a.id), 0) + 1

  const mutating = new Set(['/save', '/delete', '/reboot'])

  function handle(method: string, endpoint: string, search: URLSearchParams, body: string): MockResult {
    if (method.toUpperCase() !== (mutating.has(endpoint) ? 'POST' : 'GET')) {
      return { status: 405, body: { success: false, error: 'Method not allowed' } }
    }
    switch (endpoint) {
      case '/list':
        return {
          status: 200,
          body: {
            automations: automations.map((a) => ({
              id: a.id,
              name: a.name,
              enabled: a.enabled,
              trigger_count: a.triggers.length,
              action_count: a.actions.length,
              else_action_count: a.else_actions?.length ?? 0,
              mode: a.mode
            }))
          }
        }

      case '/get': {
        const id = idParam(search)
        if (typeof id !== 'number') return id
        const found = automations.find((a) => a.id === id)
        return found
          ? { status: 200, body: found }
          : { status: 404, body: { success: false, error: 'Automation not found' } }
      }

      case '/entities':
        return { status: 200, body: seedEntities }

      case '/schema':
        return { status: 200, body: seedSchema }

      case '/export':
        return { status: 200, body: { version: 1, automations } }

      case '/ping':
        return { status: 200, body: { status: 'ok' } }

      case '/reboot':
        return { status: 200, body: { success: true, message: 'Rebooting device...' } }

      case '/save': {
        if (new TextEncoder().encode(body).length > MAX_BODY_BYTES) {
          return { status: 413, body: { success: false, error: 'Request body over 16 KiB' } }
        }
        if (!body) {
          return { status: 400, body: { success: false, error: 'Empty request body' } }
        }
        let parsed: unknown
        try {
          parsed = JSON.parse(body)
        } catch {
          return { status: 400, body: { success: false, error: 'JSON parse error' } }
        }
        if (!validRule(parsed)) {
          return { status: 400, body: { success: false, error: 'Failed to parse automation config' } }
        }
        // The device fills the defaults a client leaves out.
        const cfg: AutomationConfig = { ...parsed, id: parsed.id ?? 0, enabled: parsed.enabled ?? true, mode: parsed.mode ?? 'single' }
        // Name clash by stored filename, as the backend checks it.
        if (isNameTaken(cfg.name, cfg.id, automations)) {
          return {
            status: 400,
            body: { success: false, error: `An automation named "${cfg.name}" already exists` }
          }
        }
        if (cfg.id > 0) {
          const i = automations.findIndex((a) => a.id === cfg.id)
          if (i < 0) {
            return { status: 404, body: { success: false, error: 'Automation not found' } }
          }
          automations[i] = normalizeStoredCron({ ...cfg })
          return { status: 200, body: { success: true, message: 'Automation updated' } }
        }
        const id = nextId++
        automations.push(normalizeStoredCron({ ...cfg, id }))
        return { status: 200, body: { success: true, message: 'Automation created', id } }
      }

      case '/delete': {
        const id = idParam(search)
        if (typeof id !== 'number') return id
        const i = automations.findIndex((a) => a.id === id)
        if (i < 0) {
          return { status: 404, body: { success: false, error: 'Automation not found' } }
        }
        automations.splice(i, 1)
        return { status: 200, body: { success: true, message: 'Automation deleted' } }
      }

      default:
        return { status: 404, body: { success: false, error: 'Unknown endpoint' } }
    }
  }

  return { handle }
}

export interface AutomationMockOptions {
  /**
   * URL prefix the SDK calls, WITHOUT the trailing endpoint. Default '/api'.
   * - Standalone editor (app served at the url_prefix root): '/api'.
   * - Dashboard embedding the editor as a sibling: '/automation-editor/api'.
   */
  apiBase?: string
}

// --- Transport: FetchImpl (programmatic / unit tests) ------------------------
/**
 * A `FetchImpl` backed by a fresh mock store — inject into createAutomationApi()
 * to exercise the SDK with no dev server:
 *   const api = createAutomationApi({ base: '/api', fetchImpl: createMockFetch() })
 */
export function createMockFetch(options: AutomationMockOptions = {}): FetchImpl {
  const apiBase = options.apiBase ?? '/api'
  const store = createAutomationMockStore()
  return async (url, init) => {
    const u = new URL(url, 'http://localhost')
    const endpoint = u.pathname.startsWith(apiBase) ? u.pathname.slice(apiBase.length) : u.pathname
    const body = typeof init?.body === 'string' ? init.body : ''
    const { status, body: payload } = store.handle(init?.method ?? 'GET', endpoint, u.searchParams, body)
    return new Response(JSON.stringify(payload), {
      status,
      headers: { 'Content-Type': 'application/json' }
    })
  }
}
