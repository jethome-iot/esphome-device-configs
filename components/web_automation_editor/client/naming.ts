// Where the device stores an automation: one file per name, so two names that
// reduce to one key cannot coexist. Mirrors name_key_ in
// automations/automation_storage.cpp — the firmware is the authority, this is what
// lets a client say so before the round trip.

/// The key the device stores an automation under: lowercased ASCII alphanumerics,
/// space/-/_ folded to one underscore, non-ASCII kept. A key too long for a file
/// name is cut on the device, but two names collide only when their whole keys do.
export function storageKey(name: string): string {
  const folded = Array.from(name)
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
