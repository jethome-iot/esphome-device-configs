// What the device accepts as a thermostat's or a preset's name, and what it derives
// from one. climate_hub follows these rules byte for byte — the device is the
// authority, this is what lets a client say so before the round trip.

import type { StandardPresetName } from './types'
import { NAME_MAX_LENGTH, STANDARD_PRESETS } from './types'

// C's isspace() in the "C" locale: the device leaves every other character alone.
const ASCII_SPACE = /[ \t\n\v\f\r]+/g

/// The name the device stores: ASCII whitespace trimmed from both ends.
export function trimName(name: string): string {
  return name.replace(/^[ \t\n\v\f\r]+|[ \t\n\v\f\r]+$/g, '')
}

/// The name as a person reads it: trimmed, inner whitespace collapsed, ASCII
/// lowercased. Non-ASCII is left alone, exactly as the firmware leaves it.
export function nameKey(name: string): string {
  return trimName(name.replace(ASCII_SPACE, ' ')).replace(/[A-Z]/g, (c) => c.toLowerCase())
}

/// The object id ESPHome derives from an entity name, and so the id Home
/// Assistant and the device's own lookups know the entity by: per UTF-8 byte, a
/// space becomes '_', A-Z is lowercased, and anything outside [a-z0-9_-] is '_'.
export function objectIdOf(name: string): string {
  let out = ''
  // The device writes it into a 128-byte buffer.
  for (const byte of new TextEncoder().encode(name).subarray(0, 127)) {
    const c = String.fromCharCode(byte >= 0x41 && byte <= 0x5a ? byte + 0x20 : byte)
    out += /[a-z0-9_-]/.test(c) ? c : '_'
  }
  return out
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

// The other thermostat (or YAML climate) this name collides with, and how.
function clashOf(
  name: string,
  id: string,
  existing: ReadonlyArray<Named>
): { by: 'name' | 'object_id'; other: Named } | null {
  const key = nameKey(name)
  const objectId = objectIdOf(trimName(name))
  for (const other of existing) {
    if (other.id === id) continue
    if (nameKey(other.name) === key) return { by: 'name', other }
  }
  for (const other of existing) {
    if (other.id === id) continue
    if (objectIdOf(trimName(other.name)) === objectId) return { by: 'object_id', other }
  }
  return null
}

/// Whether another controller already answers to `name`, by its name or by the
/// object id both would get. `id` is the controller being saved ('' for a new
/// one), which never blocks itself.
export function isNameTaken(name: string, id: string, existing: ReadonlyArray<Named>): boolean {
  return clashOf(name, id, existing) !== null
}

/// Why the device would refuse `name`, in its own words, or '' when it would take
/// it. The checks run in the device's order. `existing` is every other climate the
/// name must differ from: the controllers, plus any YAML climate under an id no
/// controller has.
export function nameError(name: string, id: string, existing: ReadonlyArray<Named>): string {
  const trimmed = trimName(name)
  const broken = nameRuleError(trimmed)
  if (broken) return broken
  const clash = clashOf(trimmed, id, existing)
  if (clash?.by === 'name') return `"${trimmed}" is already used by another thermostat`
  if (clash) {
    const other = trimName(clash.other.name)
    return `"${trimmed}" is too close to "${other}": both are ${objectIdOf(trimmed)} to Home Assistant`
  }
  return ''
}

// The rules a name must keep whatever it names, on the trimmed name.
function nameRuleError(trimmed: string): string {
  if (trimmed === '') return 'Name is required'
  if (new TextEncoder().encode(trimmed).length > NAME_MAX_LENGTH) {
    return `Name is longer than ${NAME_MAX_LENGTH} characters`
  }
  if (!/^[\x20-\x7e]*$/.test(trimmed)) return 'Use printable ASCII characters only'
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
