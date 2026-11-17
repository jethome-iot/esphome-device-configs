// Where the device stores an automation: one file per name, so two names that
// reduce to one key cannot coexist. Mirrors name_key_ in
// automations/automation_storage.cpp — the firmware is the authority, this is what
// lets a client say so before the round trip.

import { foldCase } from '../../case_fold/client/caseFold'

/// The key the device stores an automation under: the name case folded as the
/// firmware folds it (caseFold.ts), ASCII alphanumerics kept, space/-/_ folded to one
/// underscore, other ASCII dropped, non-ASCII kept. A key too long for a file name is
/// cut on the device, but two names collide only when their whole keys do.
export function storageKey(name: string): string {
  const folded = Array.from(foldCase(name))
    .map((c) =>
      (c.codePointAt(0) ?? 0) >= 0x80
        ? c
        : /[a-zA-Z0-9]/.test(c)
          ? c.toLowerCase()
          : ' -_'.includes(c)
            ? '_'
            : ''
    )
    .join('')
  return folded.replace(/_+/g, '_').replace(/^_+|_+$/g, '') || 'automation'
}

/// Whether `name` would land in a file another automation already owns. `id` is
/// the automation being saved (0 for a new one), which never blocks itself.
export function isNameTaken(
  name: string,
  id: number,
  existing: ReadonlyArray<{ id: number; name: string }>
): boolean {
  const wanted = storageKey(name)
  return existing.some((a) => a.id !== id && storageKey(a.name) === wanted)
}

/// Why the device refuses `name`, in its words; '' when it takes it. Mirrors
/// AutomationStorage::check_name: any script, but no C0, DEL or C1 character. A string holds no
/// bytes that are not UTF-8; a lone surrogate, which JSON would carry garbled, stands in for them.
export function nameProblem(name: string): string {
  for (const ch of name) {
    const c = ch.codePointAt(0) ?? 0
    if (c >= 0xd800 && c <= 0xdfff) return 'Name is not valid UTF-8'
    if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) return 'Name cannot contain control characters'
  }
  return ''
}
