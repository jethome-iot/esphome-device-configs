// The single TypeScript mirror of the device's cron grammar.
//
// The firmware (automations/automation_config.cpp) does NOT store the cron
// string a client posts. It deserialises every field into a set of integers and
// RE-SERIALISES on every read (serialize_cron_field / parse_cron_part /
// deserialize_cron_field), so a device round-trips `0 0 8 * * 1,7` back as
// `0 0 8 * * */6`, and an empty field back as `*`. Any TS consumer that wants to
// parse, rebuild, validate or fake a device cron therefore has to expand and
// format fields exactly the way the firmware does — otherwise it drifts and
// silently corrupts schedules.
//
// This file is that mirror, kept beside the SDK so the dashboard parser, the
// client-side validator and the dev mock all import the SAME grammar and cannot
// diverge. It is dependency-free (no package.json in client/) — only plain TS.
//
// Field bounds match automation_config.cpp exactly:
//   seconds 0-60 (60 is deliberate: ESPHome bitset<61>, leap second),
//   minutes 0-59, hours 0-23, day-of-month 1-31, month 1-12,
//   day-of-week 1-7 (1=Sun .. 7=Sat, the ESPHome convention).

export interface CronBound {
  min: number
  max: number
}

// Ordered as the 6 space-separated fields appear on the wire.
export const CRON_FIELDS: Array<{ key: string; label: string; bound: CronBound }> = [
  { key: 'seconds', label: 'Seconds', bound: { min: 0, max: 60 } },
  { key: 'minutes', label: 'Minutes', bound: { min: 0, max: 59 } },
  { key: 'hours', label: 'Hours', bound: { min: 0, max: 23 } },
  { key: 'days_of_month', label: 'Day of month', bound: { min: 1, max: 31 } },
  { key: 'months', label: 'Month', bound: { min: 1, max: 12 } },
  { key: 'days_of_week', label: 'Day of week', bound: { min: 1, max: 7 } }
]

// --- expand: string field -> sorted unique int list -------------------------
// The device's parse_cron_part, minus its refusals: a broken or out-of-range
// part is skipped here so a builder can show what it can of a half-typed
// field. The device refuses the whole rule instead (400 on save); run
// validateCronExpression() before sending, its rules are the device's.

function parseCronPart(part: string, min: number, max: number, out: number[]): void {
  if (!part) return

  let base = part
  let step = 1
  const slash = part.indexOf('/')
  if (slash !== -1) {
    base = part.slice(0, slash)
    const stepStr = part.slice(slash + 1)
    const stepN = Number(stepStr)
    if (stepStr === '' || !Number.isInteger(stepN) || stepN <= 0) return // invalid step -> skip
    step = stepN
  }

  let rangeStart = min
  let rangeEnd = max

  if (base === '*') {
    rangeStart = min
    rangeEnd = max
  } else {
    const dash = base.indexOf('-')
    if (dash !== -1) {
      const startStr = base.slice(0, dash)
      const endStr = base.slice(dash + 1)
      const s = Number(startStr)
      const e = Number(endStr)
      if (startStr === '' || endStr === '' || !Number.isInteger(s) || !Number.isInteger(e)) return
      rangeStart = s
      rangeEnd = e
      if (rangeStart < min) rangeStart = min
      if (rangeEnd > max) rangeEnd = max
      if (rangeStart > rangeEnd) return
    } else {
      const v = Number(base)
      if (base === '' || !Number.isInteger(v)) return
      if (v >= min && v <= max) out.push(v)
      return
    }
  }

  for (let i = rangeStart; i <= rangeEnd; i += step) out.push(i)
}

/** Expand one cron field to its sorted, de-duplicated integer set (device semantics). */
export function expandCronField(field: string, min: number, max: number): number[] {
  const out: number[] = []
  if (!field) return out
  for (const part of field.split(',')) parseCronPart(part, min, max, out)
  return Array.from(new Set(out)).sort((a, b) => a - b)
}

// --- format: int list -> compact string field -------------------------------
// Mirrors serialize_cron_field byte-for-byte: full range or empty -> "*",
// step-from-min -> "*/N", contiguous -> "X-Y", single -> "N", else comma list.

/** Format an integer set back to the compact field string the device would emit. */
export function formatCronField(values: number[], min: number, max: number): string {
  if (values.length === 0) return '*'

  const v = Array.from(new Set(values)).sort((a, b) => a - b)
  const first = v[0] as number
  const last = v[v.length - 1] as number

  // Full contiguous range from min -> "*"
  const fullRange = max - min + 1
  if (v.length === fullRange) {
    let isFull = true
    for (let i = 0; i < v.length; i++) {
      if (v[i] !== min + i) {
        isFull = false
        break
      }
    }
    if (isFull) return '*'
  }

  // Step pattern from min -> "*/N"
  if (v.length >= 2 && first === min) {
    const step = (v[1] as number) - first
    if (step > 1) {
      let isStep = true
      for (let i = 1; i < v.length; i++) {
        if ((v[i] as number) - (v[i - 1] as number) !== step) {
          isStep = false
          break
        }
      }
      if (isStep) {
        let expectedLast = min
        while (expectedLast + step <= max) expectedLast += step
        if (last === expectedLast) return `*/${step}`
      }
    }
  }

  // Contiguous range -> "X-Y"
  if (v.length >= 2) {
    let contiguous = true
    for (let i = 1; i < v.length; i++) {
      if ((v[i] as number) !== (v[i - 1] as number) + 1) {
        contiguous = false
        break
      }
    }
    if (contiguous) return `${first}-${last}`
  }

  if (v.length === 1) return String(first)
  return v.join(',')
}

/** Normalise one field the way a device would (expand then re-format). */
export function normalizeCronField(field: string, min: number, max: number): string {
  return formatCronField(expandCronField(field, min, max), min, max)
}

/**
 * Normalise a whole 6-field cron the way the device would after one store/read
 * cycle. Two crons with the same firing set normalise to the same string, so
 * this is the correct way to compare "does the builder still represent this?".
 * A cron that is not 6 space-separated fields is returned unchanged.
 */
export function normalizeCron(expr: string): string {
  const fields = expr.trim().split(/\s+/)
  if (fields.length !== CRON_FIELDS.length) return expr
  return CRON_FIELDS.map((f, i) => normalizeCronField(fields[i] as string, f.bound.min, f.bound.max)).join(' ')
}

// --- validate: strict check for the raw-cron ("Custom") input ----------------
// The device's rules: a malformed part, an out-of-range value or a field that
// matches nothing fails the save with `Failed to parse automation config`.
// Checking here lets the UI name the field before the round trip.

function validateCronPart(part: string, min: number, max: number): boolean {
  if (part === '') return false

  let base = part
  const slash = part.indexOf('/')
  if (slash !== -1) {
    base = part.slice(0, slash)
    const stepStr = part.slice(slash + 1)
    const step = Number(stepStr)
    if (stepStr === '' || !Number.isInteger(step) || step <= 0) return false
  }

  if (base === '*') return true

  const dash = base.indexOf('-')
  if (dash !== -1) {
    const startStr = base.slice(0, dash)
    const endStr = base.slice(dash + 1)
    const s = Number(startStr)
    const e = Number(endStr)
    if (startStr === '' || endStr === '' || !Number.isInteger(s) || !Number.isInteger(e)) return false
    if (s < min || e > max || s > e) return false
    return true
  }

  const v = Number(base)
  if (!Number.isInteger(v)) return false
  return v >= min && v <= max
}

/**
 * Validate a raw cron expression. Returns a human-readable error naming the
 * offending field, or null when the expression is well-formed and every field
 * yields at least one in-range value, which is what the device requires.
 */
export function validateCronExpression(expr: string): string | null {
  const trimmed = (expr || '').trim()
  if (!trimmed) return 'Cron expression is empty'

  const fields = trimmed.split(/\s+/)
  if (fields.length !== CRON_FIELDS.length) {
    return `Expected 6 fields (sec min hour day month weekday), got ${fields.length}`
  }

  for (let i = 0; i < CRON_FIELDS.length; i++) {
    const f = CRON_FIELDS[i] as (typeof CRON_FIELDS)[number]
    const field = fields[i] as string
    for (const part of field.split(',')) {
      if (!validateCronPart(part, f.bound.min, f.bound.max)) {
        return `${f.label}: "${field}" is invalid (allowed ${f.bound.min}-${f.bound.max})`
      }
    }
    // Non-"*" spec that expands to nothing = the "never fires, re-served as *" trap.
    if (field !== '*' && expandCronField(field, f.bound.min, f.bound.max).length === 0) {
      return `${f.label}: "${field}" matches no value in ${f.bound.min}-${f.bound.max}`
    }
  }

  return null
}
