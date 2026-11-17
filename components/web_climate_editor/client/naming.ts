// What the device accepts as a thermostat's or a preset's name, and what it derives
// from one. climate_hub follows these rules byte for byte — the device is the
// authority, this is what lets a client say so before the round trip.

import { foldCase } from '../../case_fold/client/caseFold'
import type { StandardPresetName } from './types'
import { NAME_MAX_BYTES, NAME_MAX_LENGTH, STANDARD_PRESETS } from './types'

// C's isspace() in the "C" locale: inside a name the device collapses these and leaves every
// other character alone.
const ASCII_SPACE = /[ \t\n\v\f\r]+/g

// The white space the device trims from both ends: ASCII's, and the Unicode spaces a paste brings
// along (no-break, ideographic and the like).
const EDGE_SPACE = '[\\t\\n\\v\\f\\r \\u00a0\\u1680\\u2000-\\u200a\\u2028\\u2029\\u202f\\u205f\\u3000\\ufeff]+'
const EDGES = new RegExp(`^${EDGE_SPACE}|${EDGE_SPACE}$`, 'g')

/// The name the device stores: white space trimmed from both ends.
export function trimName(name: string): string {
  return name.replace(EDGES, '')
}

/// The name as a person reads it: trimmed, inner whitespace collapsed, case folded
/// as the firmware folds it (caseFold.ts).
export function nameKey(name: string): string {
  return foldCase(trimName(name.replace(ASCII_SPACE, ' ')))
}

/// The id the device derives from a name at creation: lowercased ASCII
/// alphanumerics, every other run folded to a single dash, at most 48 characters,
/// `fallback` when nothing is left. A rename never changes a stored id.
export function slugify(name: string, fallback = 'climate'): string {
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
  out = out.replace(/-+$/, '').slice(0, 48).replace(/-+$/, '')
  return out || fallback
}

/// Ids a create never gets: the dashboard's editor opens a blank form at
/// /climate/new, so a thermostat with that id could not be reached.
export const RESERVED_IDS: ReadonlyArray<string> = ['new']

/// The id a create settles on: `base`, or the first free `<base>-2`, `<base>-3`…,
/// with `base` shortened so the whole id stays within 48 characters. A reserved
/// id counts as taken.
export function uniqueId(base: string, taken: ReadonlyArray<string>): string {
  return firstFree(base, (id) => !taken.includes(id) && !RESERVED_IDS.includes(id))
}

/// The key a preset gets: the slug of its name, "preset" when nothing is left, and
/// `-2`, `-3`… when another preset of the thermostat has it. It keeps it through a rename.
export function presetKey(name: string, taken: ReadonlyArray<string>): string {
  return firstFree(slugify(name, 'preset'), (key) => !taken.includes(key))
}

function firstFree(base: string, free: (candidate: string) => boolean): string {
  if (free(base)) return base
  for (let n = 2; ; n++) {
    const suffix = `-${n}`
    const candidate = base.slice(0, 48 - suffix.length).replace(/-+$/, '') + suffix
    if (free(candidate)) return candidate
  }
}

type Named = { id: string; name: string }

/// Whether another controller already answers to `name`, as a person reads it. `id`
/// is the controller being saved ('' for a new one), which never blocks itself.
export function isNameTaken(name: string, id: string, existing: ReadonlyArray<Named>): boolean {
  const key = nameKey(name)
  return existing.some((other) => other.id !== id && nameKey(other.name) === key)
}

/// Why the device would refuse `name`, in its own words, or '' when it would take
/// it. The checks run in the device's order. `existing` is every other climate the
/// name must differ from: the controllers, plus any YAML climate under an id no
/// controller has.
export function nameError(name: string, id: string, existing: ReadonlyArray<Named>): string {
  const trimmed = trimName(name)
  const broken = nameRuleError(trimmed)
  if (broken) return broken
  if (isNameTaken(trimmed, id, existing)) return `"${trimmed}" is already used by another thermostat`
  return ''
}

// One character of the object id Home Assistant 2026.7 and older rebuilds from a name
// (aioesphomeapi's compute_object_id): a space is '_', A-Z is lowercased, and anything else
// outside [a-z0-9_-] is '_'.
function legacyIdChar(c: string): string {
  const lower = c === ' ' ? '_' : c.replace(/[A-Z]/, (u) => u.toLowerCase())
  return /^[a-z0-9_-]$/.test(lower) ? lower : '_'
}

/// The object id Home Assistant 2026.7 and older gives a thermostat, and its entity's unique id
/// with it: one character per code point, so "Кухня" and "Ванна" are both "_____".
export function legacyObjectIdOf(name: string): string {
  let out = ''
  for (const c of name) out += legacyIdChar(c)
  return out
}

/// The first of `versions` older than Home Assistant 2026.8, which is the first to tell
/// thermostats apart by the whole name, or '' when none is.
export function legacyHomeAssistant(versions: ReadonlyArray<string> | undefined): string {
  for (const version of versions ?? []) {
    const parts = /^(\d+)\.(\d+)/.exec(version)
    if (!parts) continue
    const year = Number(parts[1])
    if (year < 2026 || (year === 2026 && Number(parts[2]) < 8)) return version
  }
  return ''
}

/// The name of the first thermostat in `existing` (not `id` itself) that Home Assistant 2026.7
/// and older would show as one with `name`, or '' when there is none. A name the device takes
/// for the same one is nameError's, not this.
export function legacyLookalike(name: string, id: string, existing: ReadonlyArray<Named>): string {
  const trimmed = trimName(name)
  const key = nameKey(trimmed)
  const objectId = legacyObjectIdOf(trimmed)
  const other = existing.find(
    (o) => o.id !== id && nameKey(o.name) !== key && legacyObjectIdOf(trimName(o.name)) === objectId
  )
  return other ? trimName(other.name) : ''
}

/// The names in `existing` that Home Assistant 2026.7 and older shows as one thermostat, in
/// groups of two or more, each group and its names in `existing`'s order.
export function legacyLookalikeGroups(existing: ReadonlyArray<Named>): string[][] {
  const groups = new Map<string, string[]>()
  for (const other of existing) {
    const name = trimName(other.name)
    const group = groups.get(legacyObjectIdOf(name))
    if (group) group.push(name)
    else groups.set(legacyObjectIdOf(name), [name])
  }
  return [...groups.values()].filter((group) => group.length > 1)
}

// A string holds no bytes that are not UTF-8; a lone surrogate, which JSON would carry
// garbled, is refused in their place.
const LONE_SURROGATE = /[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/
// C0, DEL and C1.
const CONTROL = /[\u0000-\u001f\u007f-\u009f]/

// The rules a name must keep whatever it names, on the trimmed name, in the
// device's order. The length counts characters (code points), then UTF-8 bytes.
function nameRuleError(trimmed: string): string {
  if (trimmed === '') return 'Name is required'
  if (LONE_SURROGATE.test(trimmed)) return 'Name is not valid UTF-8'
  if ([...trimmed].length > NAME_MAX_LENGTH) return `Name is longer than ${NAME_MAX_LENGTH} characters`
  if (new TextEncoder().encode(trimmed).length > NAME_MAX_BYTES) return `Name is longer than ${NAME_MAX_BYTES} bytes`
  if (CONTROL.test(trimmed)) return 'Name cannot contain control characters'
  if (trimmed.includes('/')) return "Name cannot contain '/'"
  if (trimmed.includes('\\')) return "Name cannot contain '\\'"
  return ''
}

/// Which of Home Assistant's built-in presets `name` is, in any case, or null for a
/// custom one. The device asks it of the trimmed name.
export function standardPreset(name: string): StandardPresetName | null {
  const lower = name.replace(/[A-Z]/g, (c) => c.toLowerCase())
  return STANDARD_PRESETS.find((standard) => standard === lower) ?? null
}

/// Why the device would refuse the name of `presets[index]`, in its own words and
/// naming the row from 1 (`Preset 2: "none" is reserved`), or '' when it would take
/// it: the thermostat name rules, then `none`, then a clash with a preset before it,
/// ignoring case and runs of spaces. The device checks the rows in order and reports
/// the first refusal, so of two clashing names the later one is refused.
export function presetNameError(presets: ReadonlyArray<{ name: string }>, index: number): string {
  const row = presets[index]
  if (!row) return ''
  const at = `Preset ${index + 1}`
  const trimmed = trimName(row.name)
  const broken = nameRuleError(trimmed)
  if (broken) return `${at}: ${broken}`
  const key = nameKey(trimmed)
  if (key === 'none') return `${at}: "${trimmed}" is reserved`
  const earlier = presets.slice(0, index).findIndex((other) => nameKey(other.name) === key)
  return earlier < 0 ? '' : `${at}: "${trimmed}" is already used by Preset ${earlier + 1}`
}
