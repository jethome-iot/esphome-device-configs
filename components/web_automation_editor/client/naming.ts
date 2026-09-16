// Where the device stores an automation: one file per name, so two names that
// reduce to one key cannot coexist. Mirrors sanitize_filename_ in
// automations/automation_storage.cpp — the firmware is the authority, this is what
// lets a client say so before the round trip.

/// The automation's filename, without the .json: lowercased ASCII alphanumerics,
/// space/-/_ folded to one underscore, non-ASCII kept, capped at 48 UTF-8 bytes.
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
  let key = folded.replace(/_+/g, '_').replace(/^_+|_+$/g, '')
  // The device's LittleFS filename budget, cut on a character boundary.
  const bytes = new TextEncoder().encode(key)
  if (bytes.length > 48) key = new TextDecoder().decode(bytes.slice(0, 48)).replace(/�+$/, '')
  return key.replace(/_+$/, '') || 'automation'
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
