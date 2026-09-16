// Shared types for the web_automation_editor API contract.
// Canonical source consumed by TS frontends (the unified dashboard). Mirrors the
// C++ backend routes at <url_prefix>/api/*.

export interface AutomationSummary {
  id: number
  name: string
  enabled: boolean
  trigger_count: number
  action_count: number
  else_action_count: number
  mode: string
}

export interface AutomationListResponse {
  automations: AutomationSummary[]
}

export interface AutomationTrigger {
  source?: string
  type?: string
  object_id?: string
  threshold?: number
  min_threshold?: number
  max_threshold?: number
  // Cron triggers round-trip only the flattened expression + preset. The backend
  // does not persist granular cron-builder fields (hour mode / start-end hour /
  // interval / day-of-month) — those are a UI-form concern, not part of the wire
  // contract, so they are intentionally not modelled here.
  cron?: string
  cron_preset?: string
}

export interface AutomationCondition {
  type: string // 'input' | 'temperature' | 'and' | 'or' | 'xor'
  object_id?: string
  state?: string
  temperature_type?: string
  threshold?: number
  min_threshold?: number
  max_threshold?: number
  conditions?: AutomationCondition[]
}

export interface AutomationAction {
  source?: string
  type?: string
  object_id?: string
  // Milliseconds is the scheduler's own unit and the one the device writes; the
  // editors pick a display unit from the value. delay_s is what firmware before
  // this wrote and is still read, never sent.
  delay_ms?: number
  delay_s?: number
  // Only meaningful for type 'follow', and only sent back for it; absent is false.
  invert?: boolean
}

export interface AutomationConfig {
  id: number
  name: string
  enabled: boolean
  mode: string // 'single' | 'restart' | 'parallel'
  triggers: AutomationTrigger[]
  condition?: AutomationCondition
  actions: AutomationAction[]
  else_actions?: AutomationAction[]
}

/** What POST /save takes: `id` absent or 0 creates; `enabled` and `mode` default on the device. */
export type AutomationSaveInput = Omit<AutomationConfig, 'id' | 'enabled' | 'mode'> &
  Partial<Pick<AutomationConfig, 'id' | 'enabled' | 'mode'>>

export interface AutomationSaveResponse {
  success: boolean
  message?: string
  id?: number // present on create
  error?: string
}

export interface AutomationEntityRef {
  object_id: string
  name: string
  unit?: string // sensors only
}

export interface AutomationEntitiesResponse {
  binary_sensors: AutomationEntityRef[]
  sensors: AutomationEntityRef[]
  switches: AutomationEntityRef[]
}

export interface AutomationSchema {
  triggers: Array<{ type: string; subtypes: string[] }>
  conditions: Array<Record<string, unknown>>
  actions: Array<Record<string, unknown>>
  cron_presets: string[]
}

/** GET /export — every automation's full config; the file a client downloads and re-imports. */
export interface AutomationExport {
  version: 1
  automations: AutomationConfig[]
}
