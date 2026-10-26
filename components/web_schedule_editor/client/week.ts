// The week, the names and the document as the device reads them: pure helpers the
// mock and the page share. The schedules component follows these rules byte for byte
// — the device is the authority, this is what lets a client say so before the round
// trip, in the device's own words.

import type { Day, PointRef, ScheduleDocument, ScheduleValue, SwitchPoint, TargetType, Week } from './types'
import { DAYS, ID_MAX_LENGTH, MAX_POINTS_PER_DAY, NAME_MAX_LENGTH, SCHEDULE_VERSION } from './types'

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
      if (at !== null) points.push({ minute: weekMinute(day, at), ref: { day, at: point.at, value: point.value } })
    }
  }
  // Stable: two points at one minute (a form mid-edit) keep their order.
  return points.sort((a, b) => a.minute - b.minute)
}

/**
 * The point in force on `day` at `minute`: the last one at or before it, from an earlier day
 * when that day has none yet, and from the end of the week before Monday's first. Null for a
 * week with no points.
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
 * week on for a week with one. A point with the value already in force counts. Null for a week
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
 * Non-ASCII is left alone, exactly as the firmware leaves it. Two schedules may not share one.
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
const CONTROL = /[\u0000-\u001f\u007f]/

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
  return clash ? `"${trimName(name)}" is already used by another schedule` : ''
}

// --- The document ------------------------------------------------------------
// The schedules component's deserialize: the same refusals in the same order. The id
// (an import's), the name, the target, the days the week names, then each day Monday
// first and its points in the order the body lists them, and last the count.

type Json = Record<string, unknown>

function objectOf(value: unknown): Json | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? (value as Json) : null
}

const VALUE_TYPES: ReadonlyArray<string> = ['on', 'off', 'preset', 'target']

/**
 * The value of the point at `where` ("Mon 06:30") for a target of `type`, as the device keeps
 * it (other keys dropped), or why it refuses it.
 */
export function decodeValue(raw: unknown, type: TargetType, where: string): { value: ScheduleValue } | { error: string } {
  const at = `${where}: `
  const obj = objectOf(raw)
  if (!obj || typeof obj.type !== 'string') return { error: `${at}value is required` }
  const kind = obj.type
  if (!VALUE_TYPES.includes(kind)) return { error: `${at}unknown value type "${kind}"` }
  if (type === 'relay' && kind !== 'on' && kind !== 'off') return { error: `${at}a relay takes on or off` }
  if (type === 'thermostat' && kind === 'on') return { error: `${at}a thermostat takes a preset, off or a target` }
  if (kind === 'on' || kind === 'off') return { value: { type: kind } }
  if (kind === 'preset') {
    if (typeof obj.preset !== 'string' || obj.preset === '') return { error: `${at}preset is required` }
    if (!isSlug(obj.preset)) return { error: `${at}preset must be a preset's key` }
    return { value: { type: 'preset', preset: obj.preset } }
  }
  if (typeof obj.target !== 'number' || !Number.isFinite(obj.target)) return { error: `${at}target must be a number` }
  return { value: { type: 'target', target: obj.target } }
}

/** A file older than this firmware reads as this version; a newer one keeps its number. */
function readVersion(value: unknown): number {
  return typeof value === 'number' && value > SCHEDULE_VERSION ? Math.min(Math.ceil(value), 65535) : SCHEDULE_VERSION
}

export interface DecodeOptions {
  /** Read it as the device reads a file, and an import: the id first, a slug. */
  requireId?: boolean
}

/**
 * The document as the device keeps it: name trimmed, `enabled` true when left out, all seven
 * days, each sorted by time, values without stray keys; or the first rule it breaks, in the
 * device's words. A save body's `id` is kept as sent ('' when it is not a string); whether it
 * names a schedule is the store's question. `version` is the body's, for a store to tell a
 * newer file; a store writes its own.
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

  const target = objectOf(root.target)
  if (!target) return { error: 'target is required' }
  if (target.type !== 'relay' && target.type !== 'thermostat') {
    return { error: 'target.type must be relay or thermostat' }
  }
  if (typeof target.id !== 'string' || target.id === '') return { error: 'target.id is required' }
  const type: TargetType = target.type

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
      const value = decodeValue(point.value, type, `${label} ${at}`)
      if ('error' in value) return value
      week[day].push({ at, value: value.value })
    }
  }
  if (pointCount(week) === 0) return { error: 'A schedule needs at least one switch point' }

  return {
    doc: {
      version: readVersion(root.version),
      id,
      name,
      enabled,
      target: { type, id: target.id },
      week: sortedWeek(week)
    }
  }
}

/** The first rule the document breaks, in the device's words, or '' when it would take it. */
export function scheduleError(raw: unknown, options: DecodeOptions = {}): string {
  const decoded = decodeSchedule(raw, options)
  return 'error' in decoded ? decoded.error : ''
}

/** A save's or an enable's `warning` for a schedule that does not run for `reason`. */
export function notRunningWarning(reason: string): string {
  return `not running: ${reason.charAt(0).toLowerCase()}${reason.slice(1)}`
}

/** The keys in the order the device writes them, so two documents compare as JSON. */
export function canonicalDocument(doc: ScheduleDocument): ScheduleDocument {
  const value = (v: ScheduleValue): ScheduleValue =>
    v.type === 'preset'
      ? { type: 'preset', preset: v.preset }
      : v.type === 'target'
        ? { type: 'target', target: v.target }
        : { type: v.type }
  const week = emptyWeek()
  const sorted = sortedWeek(doc.week)
  for (const day of DAYS) week[day] = sorted[day].map((p) => ({ at: p.at, value: value(p.value) }))
  return {
    version: doc.version,
    id: doc.id,
    name: doc.name,
    enabled: doc.enabled,
    target: { type: doc.target.type, id: doc.target.id },
    week
  }
}
