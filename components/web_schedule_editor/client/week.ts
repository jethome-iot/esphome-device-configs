// The week, the names and the document as the device reads them: pure helpers the
// mock and the page share. The schedules component follows these rules byte for byte
// — the device is the authority, this is what lets a client say so before the round
// trip, in the device's own words.

import type {
  ColorKey,
  Day,
  Mode,
  ModeAction,
  PointRef,
  ScheduleDocument,
  ScheduleTarget,
  ScheduleValue,
  SwitchPoint,
  Week
} from './types'
import {
  COLOR_KEYS,
  DAYS,
  ID_MAX_LENGTH,
  MAX_ACTIONS,
  MAX_MODES,
  MAX_POINTS_PER_DAY,
  MAX_WAIT_SECONDS,
  MIN_WAIT_SECONDS,
  MODE_ID_MAX_LENGTH,
  MODE_NAME_MAX_LENGTH,
  NAME_MAX_LENGTH,
  SCHEDULE_VERSION
} from './types'

export const MINUTES_PER_DAY = 24 * 60
export const MINUTES_PER_WEEK = 7 * MINUTES_PER_DAY

/** How a message names a day: `Mon: at most 12 switch points a day`, `Mon 06:30: …`. */
export const DAY_LABELS: Readonly<Record<Day, string>> = {
  mon: 'Mon',
  tue: 'Tue',
  wed: 'Wed',
  thu: 'Thu',
  fri: 'Fri',
  sat: 'Sat',
  sun: 'Sun'
}

// --- Clock -------------------------------------------------------------------

/** "HH:MM" on the 24-hour clock, 00:00 to 23:59, into minutes since midnight; null for anything else. */
export function parseHHMM(text: unknown): number | null {
  if (typeof text !== 'string' || !/^\d\d:\d\d$/.test(text)) return null
  const hour = Number(text.slice(0, 2))
  const minute = Number(text.slice(3))
  return hour < 24 && minute < 60 ? hour * 60 + minute : null
}

/** Minutes since midnight as "HH:MM". */
export function formatHHMM(minute: number): string {
  const m = ((Math.floor(minute) % MINUTES_PER_DAY) + MINUTES_PER_DAY) % MINUTES_PER_DAY
  return `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`
}

/** Monday 0 … Sunday 6. */
export function dayIndex(day: Day): number {
  return DAYS.indexOf(day)
}

/** Minutes since Monday 00:00. */
export function weekMinute(day: Day, minute: number): number {
  return dayIndex(day) * MINUTES_PER_DAY + minute
}

/** The day and the minute of the day a week minute falls on. */
export function fromWeekMinute(value: number): { day: Day; minute: number } {
  const m = ((Math.floor(value) % MINUTES_PER_WEEK) + MINUTES_PER_WEEK) % MINUTES_PER_WEEK
  return { day: DAYS[Math.floor(m / MINUTES_PER_DAY)] ?? 'mon', minute: m % MINUTES_PER_DAY }
}

// --- Week --------------------------------------------------------------------

export function emptyWeek(): Week {
  return { mon: [], tue: [], wed: [], thu: [], fri: [], sat: [], sun: [] }
}

/**
 * Every day, Monday first, each day's points by time, as the device writes them. A point whose
 * `at` does not parse (a form mid-edit) goes after the ones that do.
 */
export function sortedWeek(week: Partial<Week>): Week {
  const out = emptyWeek()
  const order = (p: SwitchPoint) => parseHHMM(p.at) ?? MINUTES_PER_DAY
  for (const day of DAYS) out[day] = [...(week[day] ?? [])].sort((a, b) => order(a) - order(b))
  return out
}

/** All the points of the week. */
export function pointCount(week: Partial<Week>): number {
  return DAYS.reduce((n, day) => n + (week[day]?.length ?? 0), 0)
}

// The week's points by their minute of the week; those whose `at` does not parse left out.
function timeline(week: Partial<Week>): Array<{ minute: number; ref: PointRef }> {
  const points: Array<{ minute: number; ref: PointRef }> = []
  for (const day of DAYS) {
    for (const point of week[day] ?? []) {
      const at = parseHHMM(point.at)
      if (at !== null) points.push({ minute: weekMinute(day, at), ref: { day, at: point.at, mode: point.mode } })
    }
  }
  // Stable: two points at one minute (a form mid-edit) keep their order.
  return points.sort((a, b) => a.minute - b.minute)
}

/**
 * The point in force on `day` at `minute`, so the mode in force and since when: the last one at
 * or before it, from an earlier day when that day has none yet, and from the end of the week
 * before Monday's first. Null for a week with no points.
 */
export function inForce(week: Partial<Week>, day: Day, minute: number): PointRef | null {
  const points = timeline(week)
  const now = weekMinute(day, minute)
  let found: PointRef | null = points[points.length - 1]?.ref ?? null
  for (const point of points) {
    if (point.minute > now) break
    found = point.ref
  }
  return found
}

/**
 * The first point after `minute` on `day`, wrapping from Sunday into Monday; the same point a
 * week on for a week with one. A point of the mode already in force counts. Null for a week
 * with no points.
 */
export function nextAfter(week: Partial<Week>, day: Day, minute: number): PointRef | null {
  const points = timeline(week)
  const now = weekMinute(day, minute)
  return (points.find((p) => p.minute > now) ?? points[0])?.ref ?? null
}

// --- Names and ids -----------------------------------------------------------

// C's isspace() in the "C" locale: the device leaves every other character alone.
const ASCII_SPACE = /[ \t\n\v\f\r]+/g

/** The name the device stores: ASCII whitespace trimmed from both ends. */
export function trimName(name: string): string {
  return name.replace(/^[ \t\n\v\f\r]+|[ \t\n\v\f\r]+$/g, '')
}

/**
 * The name as a person reads it: trimmed, inner whitespace collapsed, ASCII lowercased.
 * Non-ASCII is left alone, exactly as the firmware leaves it. Two schedules may not share one,
 * nor two modes of one schedule.
 */
export function nameKey(name: string): string {
  return trimName(name.replace(ASCII_SPACE, ' ')).replace(/[A-Z]/g, (c) => c.toLowerCase())
}

/**
 * The id the device derives from a name at creation: lowercased ASCII alphanumerics, every
 * other run folded to a single dash, at most 48 characters, `fallback` when nothing is left
 * (a Cyrillic name is `schedule`). A rename never changes a stored id.
 */
export function slugify(name: string, fallback = 'schedule'): string {
  let out = ''
  let prevDash = false
  for (const c of name) {
    if (/^[a-zA-Z0-9]$/.test(c)) {
      out += c.toLowerCase()
      prevDash = false
    } else if (!prevDash && out !== '') {
      out += '-'
      prevDash = true
    }
  }
  out = out.replace(/-+$/, '').slice(0, ID_MAX_LENGTH).replace(/-+$/, '')
  return out || fallback
}

/** Whether `id` is a slug the device takes as a file name. */
export function isSlug(id: string): boolean {
  return id !== '' && slugify(id) === id
}

/**
 * Ids a create never gets: the dashboard's editor opens a blank form at /schedules/new, so a
 * schedule with that id could not be reached; an import of one is refused.
 */
export const RESERVED_IDS: ReadonlyArray<string> = ['new']

/**
 * The id a create settles on: `base`, or the first free `<base>-2`, `<base>-3`…, with `base`
 * shortened so the whole id stays within 48 characters. A reserved id counts as taken.
 */
export function uniqueId(base: string, taken: ReadonlyArray<string>): string {
  const free = (id: string) => !taken.includes(id) && !RESERVED_IDS.includes(id)
  if (free(base)) return base
  for (let n = 2; ; n++) {
    const suffix = `-${n}`
    const head = base.slice(0, ID_MAX_LENGTH - suffix.length).replace(/-+$/, '')
    const candidate = (head || 'schedule') + suffix
    if (free(candidate)) return candidate
  }
}

// A surrogate without its pair: what a string holds where its UTF-8 would be broken.
const LONE_SURROGATE = /[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/
// C0, DEL and C1: none of them prints, on the display or in a list.
const CONTROL = /[\u0000-\u001f\u007f-\u009f]/

/**
 * Why the device would refuse `name` whatever other schedules are called, in its words, or ''.
 * The rules run in the device's order, on the trimmed name; the length counts characters (code
 * points), so 48 Cyrillic letters fit.
 */
export function nameRuleError(name: string): string {
  const trimmed = trimName(name)
  if (trimmed === '') return 'Name is required'
  if ([...trimmed].length > NAME_MAX_LENGTH) return `Name is longer than ${NAME_MAX_LENGTH} characters`
  if (LONE_SURROGATE.test(trimmed)) return 'Name is not valid UTF-8'
  if (CONTROL.test(trimmed)) return 'Name cannot contain control characters'
  if (trimmed.includes('/')) return "Name cannot contain '/'"
  if (trimmed.includes('\\')) return "Name cannot contain '\\'"
  return ''
}

/**
 * Why the device would refuse `name` for the schedule `id` ('' for a new one), or '' when it
 * would take it: the rules, then a clash with another schedule's name (409 on the device).
 */
export function nameError(name: string, id: string, existing: ReadonlyArray<{ id: string; name: string }>): string {
  const broken = nameRuleError(name)
  if (broken) return broken
  const key = nameKey(name)
  const clash = existing.some((other) => other.id !== id && nameKey(other.name) === key)
  return clash ? nameTakenError(trimName(name)) : ''
}

/**
 * What the device says of a mode's `name` on its own, after `Mode 2: `, or '': required, at most
 * 24 characters, valid, no control characters. A mode name may hold '/' and '\'.
 */
export function modeNameRuleError(name: string): string {
  const trimmed = trimName(name)
  if (trimmed === '') return 'name is required'
  if (LONE_SURROGATE.test(trimmed)) return 'name is not valid UTF-8'
  if ([...trimmed].length > MODE_NAME_MAX_LENGTH) return `name is longer than ${MODE_NAME_MAX_LENGTH} characters`
  if (CONTROL.test(trimmed)) return 'name cannot contain control characters'
  return ''
}

/** Whether the device takes `id` as a mode's: 1 to 24 of a-z, 0-9 and '-'. */
export function isModeId(id: string): boolean {
  return id.length >= 1 && id.length <= MODE_ID_MAX_LENGTH && /^[a-z0-9-]+$/.test(id)
}

/**
 * The id the editor gives a new mode: from its name when that leaves a free one, else the first
 * free `m1`, `m2`…. A rename keeps the id, since the week's points name it.
 */
export function newModeId(name: string, modes: ReadonlyArray<{ id: string }>): string {
  const taken = (id: string) => modes.some((m) => m.id === id)
  const fromName = slugify(name, '').slice(0, MODE_ID_MAX_LENGTH).replace(/-+$/, '')
  if (fromName && !taken(fromName)) return fromName
  for (let n = 1; ; n++) if (!taken(`m${n}`)) return `m${n}`
}

/**
 * The colour each mode shows, by mode order: its own, or, for one without, the first palette
 * key no other mode has, neither one of its own nor one shown already.
 */
export function modeColors(modes: ReadonlyArray<{ color?: ColorKey }>): ColorKey[] {
  const used = new Set<ColorKey>(modes.flatMap((m) => (m.color ? [m.color] : [])))
  return modes.map((m) => {
    if (m.color) return m.color
    const free = COLOR_KEYS.find((key) => !used.has(key)) ?? COLOR_KEYS[0]
    used.add(free)
    return free
  })
}

// --- The device's refusals a page acts on ---------------------------------------
// Built here and matched here, so the mock, the page and the firmware's sentences cannot drift
// apart: the contract checks the mock's answers, which come from these builders, against the
// device's.

/** The 409 of a Save, an import or an enable without take_over: `holder` drives `target` already. */
export function conflictError(holder: string, target: string): string {
  return `"${holder}" already drives ${target}; switch it off first or take over`
}

const CONFLICT = /^"(.*)" already drives .*; switch it off first or take over$/s

/** The name of the schedule a conflictError() says drives the target, or null for any other message. */
export function conflictHolder(message: string): string | null {
  return CONFLICT.exec(message)?.[1] ?? null
}

/** A `conflict` status's reason: `holder`, first by id, drives `target`, the first the two share. */
export function conflictReason(holder: string, target: string): string {
  return `Schedule "${holder}" drives ${target}`
}

/** The 409 of a name another schedule has, ignoring ASCII case and runs of spaces. */
export function nameTakenError(name: string): string {
  return `"${name}" is already used by another schedule`
}

const NAME_TAKEN = /^".*" is already used by another schedule$/s

/** Whether `message` is a nameTakenError(). */
export function isNameTaken(message: string): boolean {
  return NAME_TAKEN.test(message)
}

/**
 * A save's or an enable's `warning`: the action at `index` (from 0) of the mode called `mode`
 * is skipped, for the issue's `text`.
 */
export function skippedWarning(mode: string, index: number, text: string): string {
  return `skips mode "${mode}", action ${index + 1}: ${text.charAt(0).toLowerCase()}${text.slice(1)}`
}

// --- What a schedule drives ------------------------------------------------------

/** Every relay and thermostat the modes name, each once, in the order they first name them. */
export function scheduleTargets(doc: { modes: ReadonlyArray<{ actions?: ReadonlyArray<ModeAction> }> }): ScheduleTarget[] {
  const out: ScheduleTarget[] = []
  for (const mode of doc.modes) {
    for (const action of mode.actions ?? []) {
      if (action.type === 'wait') continue
      if (!out.some((t) => t.type === action.type && t.id === action.id)) out.push({ type: action.type, id: action.id })
    }
  }
  return out
}

/** Whether any of the modes' actions drives `target`. */
export function namesTarget(doc: { modes: ReadonlyArray<{ actions?: ReadonlyArray<ModeAction> }> }, target: ScheduleTarget): boolean {
  return scheduleTargets(doc).some((t) => t.type === target.type && t.id === target.id)
}

// --- The document ------------------------------------------------------------
// The schedules component's deserialize: the same refusals in the same order. The id
// (an import's), the name, the modes one at a time and each one's actions, the days the
// week names, then each day Monday first and its points in the order the body lists them,
// and last the count.

type Json = Record<string, unknown>

function objectOf(value: unknown): Json | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? (value as Json) : null
}

const VALUE_TYPES: ReadonlyArray<string> = ['on', 'off', 'preset', 'target']
const COLOR_RULE = `color must be one of ${COLOR_KEYS.slice(0, -1).join(', ')} or ${COLOR_KEYS[COLOR_KEYS.length - 1]}`

/** The value of a relay's or a thermostat's action, as the device keeps it, or why it refuses it. */
function decodeValue(raw: unknown, type: 'relay' | 'thermostat', at: string): { value: ScheduleValue } | { error: string } {
  const obj = objectOf(raw)
  // A value whose type is "" says nothing more than none at all.
  if (!obj || typeof obj.type !== 'string' || obj.type === '') return { error: `${at}value is required` }
  const kind = obj.type
  if (!VALUE_TYPES.includes(kind)) return { error: `${at}unknown value type "${kind}"` }
  if (type === 'relay' && kind !== 'on' && kind !== 'off') return { error: `${at}a relay takes on or off` }
  if (kind === 'on' || kind === 'off') return { value: { type: kind } }
  if (kind === 'preset') {
    if (typeof obj.preset !== 'string' || obj.preset === '') return { error: `${at}preset is required` }
    if (!isSlug(obj.preset)) return { error: `${at}preset must be a preset's key` }
    return { value: { type: 'preset', preset: obj.preset } }
  }
  // The device keeps a float: a number past a float's range turns infinite, which is no target either.
  if (typeof obj.target !== 'number' || !Number.isFinite(Math.fround(obj.target))) {
    return { error: `${at}target must be a number` }
  }
  return { value: { type: 'target', target: obj.target } }
}

/**
 * An action as the device keeps it (other keys dropped), or why it refuses it; `where` heads
 * the sentence: `Mode "Day", action 2`.
 */
export function decodeAction(raw: unknown, where: string): { action: ModeAction } | { error: string } {
  const at = `${where}: `
  const obj = objectOf(raw)
  if (!obj) return { error: `${at}an action must be an object` }
  if (typeof obj.type !== 'string' || obj.type === '') return { error: `${at}type is required` }
  const type = obj.type
  if (type === 'wait') {
    const seconds = obj.seconds
    if (typeof seconds !== 'number' || !Number.isInteger(seconds)) return { error: `${at}seconds must be a whole number` }
    if (seconds < MIN_WAIT_SECONDS || seconds > MAX_WAIT_SECONDS) return { error: `${at}a wait is 1 s to 24 h` }
    return { action: { type: 'wait', seconds } }
  }
  if (type !== 'relay' && type !== 'thermostat') return { error: `${at}unknown action type "${type}"` }
  if (typeof obj.id !== 'string' || obj.id === '') return { error: `${at}id is required` }
  const value = decodeValue(obj.value, type, at)
  if ('error' in value) return value
  if (type === 'relay') return { action: { type: 'relay', id: obj.id, value: value.value as RelayValue } }
  return { action: { type: 'thermostat', id: obj.id, value: value.value } }
}

type RelayValue = { type: 'on' } | { type: 'off' }

/** The mode numbered `number` from 1, read after `before`, as the device keeps it, or why it refuses it. */
export function decodeMode(raw: unknown, number: number, before: ReadonlyArray<Mode>): { mode: Mode } | { error: string } {
  const at = `Mode ${number}: `
  const obj = objectOf(raw)
  if (!obj) return { error: `${at}a mode must be an object` }
  const name = typeof obj.name === 'string' ? trimName(obj.name) : ''
  const badName = modeNameRuleError(name)
  if (badName) return { error: at + badName }
  if (before.some((m) => nameKey(m.name) === nameKey(name))) return { error: `Two modes are called "${name}"` }
  const label = `Mode "${name}"`
  if (typeof obj.id !== 'string' || obj.id === '') return { error: `${label}: id is required` }
  if (!isModeId(obj.id)) return { error: `${label}: id must be at most 24 lowercase letters, digits and dashes` }
  const id = obj.id
  if (before.some((m) => m.id === id)) return { error: `Two modes have the id "${id}"` }
  let color: ColorKey | undefined
  if (obj.color !== undefined && obj.color !== null) {
    if (typeof obj.color !== 'string' || !(COLOR_KEYS as ReadonlyArray<string>).includes(obj.color)) {
      return { error: `${label}: ${COLOR_RULE}` }
    }
    color = obj.color as ColorKey
  }
  const list = obj.actions
  if (list !== undefined && list !== null && !Array.isArray(list)) return { error: `${label}: actions must be a list` }
  const items = (list ?? []) as unknown[]
  if (items.length > MAX_ACTIONS) return { error: `${label}: at most ${MAX_ACTIONS} actions` }
  const actions: ModeAction[] = []
  for (const [i, item] of items.entries()) {
    const decoded = decodeAction(item, `${label}, action ${i + 1}`)
    if ('error' in decoded) return decoded
    actions.push(decoded.action)
  }
  return { mode: color ? { id, name, color, actions } : { id, name, actions } }
}

/** A file older than this firmware reads as this version; a newer one keeps its number. */
function readVersion(value: unknown): number {
  return typeof value === 'number' && value > SCHEDULE_VERSION ? Math.min(Math.ceil(value), 65535) : SCHEDULE_VERSION
}

/** A revision as the device reads one, a whole number that fits 32 bits; null for anything else. */
export function readRevision(value: unknown): number | null {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= 0xffffffff ? value : null
}

export interface DecodeOptions {
  /** Read it as the device reads a file, and an import: the id first, a slug. */
  requireId?: boolean
}

/**
 * The document as the device keeps it: names trimmed, `enabled` true when left out, every
 * mode's actions, all seven days, each sorted by time, no stray keys; or the first rule it
 * breaks, in the device's words. A save body's `id` is kept as sent ('' when it is not a
 * string); whether it names a schedule is the store's question. `version` is the body's, for a
 * store to tell a newer file, and `revision` a file's (0 without one); a store writes its own.
 */
export function decodeSchedule(raw: unknown, options: DecodeOptions = {}): { doc: ScheduleDocument } | { error: string } {
  const root = objectOf(raw)
  if (!root) return { error: 'document is not an object' }
  let id = ''
  if (options.requireId) {
    if (typeof root.id !== 'string' || root.id === '') return { error: 'id is required' }
    if (!isSlug(root.id)) return { error: 'id must be a slug: lowercase letters, digits and single dashes' }
    id = root.id
  } else if (typeof root.id === 'string') {
    id = root.id
  }

  const name = typeof root.name === 'string' ? trimName(root.name) : ''
  const badName = nameRuleError(name)
  if (badName) return { error: badName }
  const enabled = typeof root.enabled === 'boolean' ? root.enabled : true

  if (!Array.isArray(root.modes)) return { error: 'modes is required' }
  const rawModes = root.modes as unknown[]
  if (rawModes.length === 0) return { error: 'A schedule needs at least one mode' }
  if (rawModes.length > MAX_MODES) return { error: `A schedule has at most ${MAX_MODES} modes` }
  const modes: Mode[] = []
  for (const [i, item] of rawModes.entries()) {
    const decoded = decodeMode(item, i + 1, modes)
    if ('error' in decoded) return decoded
    modes.push(decoded.mode)
  }

  const rawWeek = objectOf(root.week)
  if (!rawWeek) return { error: 'week is required' }
  for (const key of Object.keys(rawWeek)) {
    if (!(DAYS as ReadonlyArray<string>).includes(key)) {
      return { error: `Unknown day "${key}": expected mon, tue, wed, thu, fri, sat or sun` }
    }
  }
  const week = emptyWeek()
  for (const day of DAYS) {
    const label = DAY_LABELS[day]
    const list = rawWeek[day]
    if (list === undefined || list === null) continue
    if (!Array.isArray(list)) return { error: `${label}: switch points must be a list` }
    if (list.length > MAX_POINTS_PER_DAY) return { error: `${label}: at most ${MAX_POINTS_PER_DAY} switch points a day` }
    const seen = new Set<number>()
    for (const item of list as unknown[]) {
      const point = objectOf(item)
      if (!point) return { error: `${label}: a switch point must be an object` }
      const minute = parseHHMM(point.at)
      if (minute === null) return { error: `${label}: at must be HH:MM from 00:00 to 23:59` }
      const at = formatHHMM(minute)
      if (seen.has(minute)) return { error: `${label}: ${at} is there twice` }
      seen.add(minute)
      if (typeof point.mode !== 'string' || point.mode === '') return { error: `${label} ${at}: mode is required` }
      const mode = point.mode
      if (!modes.some((m) => m.id === mode)) return { error: `${label} ${at}: there is no mode "${mode}"` }
      week[day].push({ at, mode })
    }
  }
  if (pointCount(week) === 0) return { error: 'A schedule needs at least one switch point' }

  return {
    doc: {
      version: readVersion(root.version),
      revision: readRevision(root.revision) ?? 0,
      id,
      name,
      enabled,
      modes,
      week: sortedWeek(week)
    }
  }
}

/** The first rule the document breaks, in the device's words, or '' when it would take it. */
export function scheduleError(raw: unknown, options: DecodeOptions = {}): string {
  const decoded = decodeSchedule(raw, options)
  return 'error' in decoded ? decoded.error : ''
}

/** An action with its keys in the order the device writes them. */
export function canonicalAction(action: ModeAction): ModeAction {
  if (action.type === 'wait') return { type: 'wait', seconds: action.seconds }
  const v = action.value
  const value: ScheduleValue =
    v.type === 'preset'
      ? { type: 'preset', preset: v.preset }
      : v.type === 'target'
        ? { type: 'target', target: v.target }
        : { type: v.type }
  return action.type === 'relay'
    ? { type: 'relay', id: action.id, value: value as RelayValue }
    : { type: 'thermostat', id: action.id, value }
}

/** The keys in the order the device writes them, so two documents compare as JSON. */
export function canonicalDocument(doc: ScheduleDocument): ScheduleDocument {
  const week = emptyWeek()
  const sorted = sortedWeek(doc.week)
  for (const day of DAYS) week[day] = sorted[day].map((p) => ({ at: p.at, mode: p.mode }))
  return {
    version: doc.version,
    revision: doc.revision,
    id: doc.id,
    name: doc.name,
    enabled: doc.enabled,
    modes: doc.modes.map((m) => ({
      id: m.id,
      name: m.name,
      ...(m.color ? { color: m.color } : {}),
      actions: m.actions.map(canonicalAction)
    })),
    week
  }
}
