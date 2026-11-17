// Names are compared without case in Latin, Greek and Cyrillic, and as they are in any other
// script. The device folds with the same table (case_fold.cpp); toLowerCase() would fold letters
// the device does not, and a name the editor accepts could then be one the device refuses.

import { FOLD_RANGES } from './caseFoldTable'

/** What `codePoint` folds to: its simple case folding when the table has it, itself otherwise. */
export function foldCodePoint(codePoint: number): number {
  // Only the last range that starts at or before the code point can hold it.
  let low = 0
  let high = FOLD_RANGES.length
  while (low < high) {
    const mid = (low + high) >> 1
    if (FOLD_RANGES[mid]![0] <= codePoint) low = mid + 1
    else high = mid
  }
  const range = FOLD_RANGES[low - 1]
  if (!range) return codePoint
  const [first, last, delta, stride] = range
  return codePoint <= last && (codePoint - first) % stride === 0 ? codePoint + delta : codePoint
}

/** `text` with every code point folded; a lone surrogate stays as it is. */
export function foldCase(text: string): string {
  let out = ''
  for (const char of text) {
    const codePoint = char.codePointAt(0)!
    const folded = foldCodePoint(codePoint)
    out += folded === codePoint ? char : String.fromCodePoint(folded)
  }
  return out
}
