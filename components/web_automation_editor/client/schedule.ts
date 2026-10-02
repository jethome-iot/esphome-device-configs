// The TypeScript mirror of the device's schedule trigger: which windows it refuses
// (read_window in automations/automation_config.cpp) and the form it writes them
// back in. The editor's form, its checks and the dev mock import it from here, so
// none of them can drift from the device. Dependency-free, like cron.ts.
import type { ScheduleWindow, Weekday } from './types'

/** The days a window may name, in the order the device writes them back: Monday first. */
export const WEEKDAYS: readonly Weekday[] = ['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun']

const HH_MM = /^(?:[01]\d|2[0-3]):[0-5]\d$/

/**
 * Minutes since midnight of a strict `HH:MM` (two-digit hour and minute, 24-hour
 * clock), or null. `24:00` is 1440 and only an end may be it.
 */
export function scheduleMinutes(text: string, end = false): number | null {
  if (end && text === '24:00') return 1440
  if (!HH_MM.test(text)) return null
  return Number(text.slice(0, 2)) * 60 + Number(text.slice(3))
}

/** Minutes since midnight as `HH:MM`; 1440 is `24:00`. */
export function formatScheduleTime(minutes: number): string {
  return `${String(Math.floor(minutes / 60)).padStart(2, '0')}:${String(minutes % 60).padStart(2, '0')}`
}

/**
 * Why the device would refuse this window, or null. The types are checked too: the
 * mock runs this on whatever JSON a client posted.
 */
export function validateScheduleWindow(window: ScheduleWindow): string | null {
  if (!window || typeof window !== 'object') return 'Not a time window'
  const { days, from, to } = window
  // The device reads a null like an absent key: every day.
  if (days != null) {
    if (!Array.isArray(days) || days.length === 0) return 'Pick at least one day'
    const unknown = days.findIndex((d) => !WEEKDAYS.includes(d))
    if (unknown >= 0) return `Unknown day "${String(days[unknown])}"`
  }
  const start = typeof from === 'string' ? scheduleMinutes(from) : null
  if (start === null) return 'From must be HH:MM, 00:00 to 23:59'
  const end = typeof to === 'string' ? scheduleMinutes(to, true) : null
  if (end === null) return 'To must be HH:MM, 00:00 to 24:00'
  // 00:00 to 24:00 is the whole day; any other pair of equal times is no time at all.
  if (start === end) return 'From and to are the same time'
  return null
}

/** Why the device would refuse a schedule trigger's `windows`, naming the window, or null. */
export function validateScheduleWindows(windows: readonly ScheduleWindow[] | undefined): string | null {
  if (!Array.isArray(windows) || windows.length === 0) return 'Add at least one time window'
  for (let i = 0; i < windows.length; i++) {
    const err = validateScheduleWindow(windows[i] as ScheduleWindow)
    if (err) return `Window ${i + 1}: ${err}`
  }
  return null
}

/**
 * The windows as the device writes them back: `days` in full (absent is all seven),
 * each day once, Monday first. Assumes windows the device took.
 */
export function normalizeScheduleWindows(windows: readonly ScheduleWindow[]): Required<ScheduleWindow>[] {
  return windows.map((w) => ({
    days: WEEKDAYS.filter((d) => (w.days ?? WEEKDAYS).includes(d)),
    from: w.from,
    to: w.to
  }))
}
