// In-process mock for the web_file_browser HTTP API — the dev/QA/test double for
// the contract in ../fileApi.ts + ../types.ts. Lives WITH the SDK so the mock,
// the types and the client stay one unit and cannot drift apart; it replaces two
// hand-rolled copies (the dashboard's and the standalone UI's vite.config.ts)
// that had already drifted apart.
//
// This file is the transport-agnostic CORE and stays dependency-free (client/ has
// no package.json / node_modules): seed data + a stateful dispatcher + a fetch
// adapter. The Vite dev-server glue is env-specific, so it lives in each
// consumer's vite.config.ts and just wraps createFileBrowserMockStore():
//
//   const files = createFileBrowserMockStore({ apiBase: prefix })
//   // Downloads are RAW bytes; handle() does NOT claim them, so try this first.
//   if (path.startsWith(`${prefix}/download`)) {
//     const bytes = files.rawDownload(path, search)
//     if (bytes) { res.end(Buffer.from(bytes)) } else { send 404 }
//     return
//   }
//   // Stall uploads so the progress bar and the cancel path are exercised.
//   if (path.startsWith(`${prefix}/upload`)) await sleep(files.uploadDelayMs)
//   const r = files.handle(method, path, search, binaryBody)
//   if (r) sendJson(r.status, r.body); else sendJson(404, { success: false, error: 'Not Found' })
//
// ==== BINARY CONTRACT — every transport MUST match this ======================
// File content lives in the store as a BINARY STRING: one JS char per byte, each
// charCodeAt() in 0..255 (latin1). Nothing above 0xFF ever enters it, so
// `content.length` IS the byte count and no TextEncoder sits in the data path.
// A transport therefore has to:
//
//   1. BODY IN — decode the raw request bytes as latin1 and pass THAT to
//      handle():  Buffer.concat(chunks).toString('latin1').
//      Never 'utf8', and never `data += chunk` — that is a utf8 decode which
//      also breaks when a multi-byte sequence straddles two chunks. A 145 B PNG
//      arrived as 185 B and previewed broken until this was fixed.
//   2. JSON OUT — JSON.stringify(result.body), written as UTF-8 (node's default
//      for res.end(string)) with Content-Type: application/json. Endpoints that
//      hand back text (/read) decode to real text here, so UTF-8 is correct.
//   3. BYTES OUT — rawDownload() returns a Uint8Array of the exact stored bytes.
//      Write it verbatim (res.end(Buffer.from(bytes))) as
//      application/octet-stream. Never re-encode it, never .toString() it.
//
// Text crossing the API (seeds, /read, /write) is UTF-8: seeds are encoded on
// the way in and /read decodes on the way out, so `used` and the listing sizes
// are the TRUE byte counts for multi-byte text too — '°C' lists as 3 bytes, not
// 2 chars.
// ============================================================================
//
// Behaviour mirrors web_file_browser.cpp: routes dispatch on the URL path and
// answer one method each (405 otherwise), getParam() reads the query string AND
// an urlencoded POST body, /write takes the RAW body, /list and /info answer bare
// objects, and everything else answers the {success,error|message} envelope.
import type { FileEntry, StorageInfo, FileApiResponse } from '../types'

// --- Constants ---------------------------------------------------------------
/**
 * Fixed mtime base (2023-11-14T22:13:20Z). Seed and mutation mtimes both derive
 * from it — never Date.now() — so listings and screenshots do not churn.
 */
export const SEED_MTIME = 1_700_000_000

/**
 * An upload whose destination path contains this marker fails with the error
 * envelope, so the honest-status path (the backend used to answer 200 on a failed
 * on-device write) is exercisable from the UI: upload a file named `fail-me.txt`.
 */
export const UPLOAD_FAIL_MARKER = 'fail-me'

const DEFAULT_TOTAL_BYTES = 4 * 1024 * 1024
const DEFAULT_UPLOAD_DELAY_MS = 500

/**
 * Baseline usage added to the seeded content's byte count. A mounted device's
 * filesystem is never empty, and without it the usage bar would be a hairline the
 * eye cannot check; `used` still tracks every write, upload and delete.
 */
const BASE_USED_BYTES = 1024 * 1024

const ENCODER = new TextEncoder()
const DECODER = new TextDecoder()

// --- Bytes <-> binary string -------------------------------------------------
// The store's currency. Kept here rather than in the transports so both consumers
// convert identically; see the BINARY CONTRACT above.

/** Raw bytes as a binary string (one char per byte). Chunked: spread has a limit. */
function bytesToBinary(bytes: Uint8Array): string {
  let out = ''
  for (let i = 0; i < bytes.length; i += 0x8000) {
    out += String.fromCharCode(...bytes.subarray(i, i + 0x8000))
  }
  return out
}

function binaryToBytes(binary: string): Uint8Array {
  const out = new Uint8Array(binary.length)
  for (let i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i) & 0xff
  return out
}

/** Text -> its UTF-8 bytes as a binary string (what a browser puts on the wire). */
function textToBinary(text: string): string {
  return bytesToBinary(ENCODER.encode(text))
}

/** Stored bytes back to text. Invalid UTF-8 (a binary file) yields U+FFFD. */
function binaryToText(binary: string): string {
  return DECODER.decode(binaryToBytes(binary))
}

// --- Seed --------------------------------------------------------------------
/**
 * Seed filesystem, in creation order (readdir on the device returns creation
 * order, and `/list` preserves it). `content: null` marks a directory.
 * Typed content doubles as canonical example payloads for the editor view.
 */
export const seedTree: ReadonlyArray<{ path: string; content: string | null }> = [
  { path: '/config', content: null },
  { path: '/automations', content: null },
  { path: '/logs', content: null },
  { path: '/www', content: null },
  // Empty directories on purpose: /www exercises the empty-folder state, /backup
  // gives copy/move a ready destination outside the source subtree.
  { path: '/backup', content: null },
  {
    path: '/configuration.yaml',
    content:
      'esphome:\n  name: jethub-d1p\n  friendly_name: "JetHub D1+"\n\nesp32:\n  board: esp32dev\n'
  },
  { path: '/secrets.yaml', content: '# Sensitive values\nwifi_password: "REDACTED"\napi_key: "REDACTED"\n' },
  { path: '/device.json', content: '{\n  "name": "JetHub D1+",\n  "board": "esp32",\n  "relays": 6\n}' },
  {
    path: '/settings.json',
    content:
      '{\n  "wifi_ssid": "MyNetwork",\n  "wifi_password": "secret123",\n  "mqtt_host": "192.168.1.100",\n  "mqtt_port": 1883\n}'
  },
  { path: '/readme.txt', content: 'Welcome to the ESPHome file browser!\n\nThis is a mock filesystem for development.' },
  {
    path: '/automation.yaml',
    content:
      'automation:\n  - trigger:\n      platform: time\n      at: "07:00:00"\n    action:\n      - service: light.turn_on\n        entity_id: light.bedroom\n'
  },
  { path: '/config/sensors.yaml', content: 'sensor:\n  - platform: dallas\n    name: "Living Room Temp"\n' },
  { path: '/config/mqtt.yaml', content: 'mqtt:\n  broker: 192.168.1.10\n  port: 1883\n' },
  { path: '/config/device.json', content: '{\n  "device_name": "esp32-sensor",\n  "location": "Living Room"\n}' },
  { path: '/config/network.json', content: '{\n  "static_ip": false,\n  "dns": "8.8.8.8"\n}' },
  // One file per automation, named after it — see components/automations.
  { path: '/automations/night_mode.json', content: '{\n  "id": 1,\n  "name": "Night Mode",\n  "enabled": true\n}' },
  {
    path: '/logs/system.log',
    content:
      '[2024-01-15 10:30:00] INFO: System started\n[2024-01-15 10:30:01] INFO: WiFi connected\n[2024-01-15 10:30:02] INFO: MQTT connected\n[2024-01-15 10:31:00] DEBUG: Sensor reading: 23.5°C\n'
  }
]

// --- Types -------------------------------------------------------------------
export interface MockResult {
  status: number
  body: unknown
}

export interface FileBrowserMockStore {
  /**
   * Dispatch one API call. pathname is the FULL path incl. apiBase; returns null
   * if not ours. `body` is the request body as a BINARY STRING (latin1, one char
   * per byte) — see the BINARY CONTRACT at the top of this file.
   */
  handle(method: string, pathname: string, search: URLSearchParams, body: string): MockResult | null
  /** Raw bytes for GET <apiBase>/download; null when the path is not a download or not found. */
  rawDownload(pathname: string, search: URLSearchParams): Uint8Array | null
  /** Milliseconds a caller should stall before answering, so the progress UI is exercised. */
  uploadDelayMs: number
  /** Restore the seed filesystem (for tests). */
  reset(): void
}

export interface FileBrowserMockOptions {
  /** API base the store answers on, e.g. '/files' or '/files' under a dashboard prefix. */
  apiBase?: string
  /** Total filesystem bytes reported by /info. Default 4 MiB. */
  totalBytes?: number
  /** Artificial per-upload delay in ms. Default 500. */
  uploadDelayMs?: number
}

interface MockNode {
  type: 'file' | 'directory'
  /** Raw bytes as a binary string — never text. `content.length` is the size. */
  content: string
  mtime: number
}

// --- Path helpers ------------------------------------------------------------
// Rebuild from segments, dropping empty ones (duplicate or trailing separators)
// and "." ones — matching resolve_path_() in the C++ exactly, because /copy's
// subtree guard compares paths as strings and a shape the mock canonicalises
// differently is a bypass the dev server cannot reproduce. ".." is kept: it is
// what makes a path invalid, so collapsing it would defeat isValidPath.
function normalize(path: string): string {
  const segments = path.split('/').filter((s) => s !== '' && s !== '.')
  return segments.length > 0 ? '/' + segments.join('/') : '/'
}

/** Prefix every descendant of `path` shares (root is its own special case). */
function childPrefix(path: string): string {
  return path === '/' ? '/' : path + '/'
}

function parentOf(path: string): string {
  const i = path.lastIndexOf('/')
  return i <= 0 ? '/' : path.slice(0, i)
}

function nameOf(path: string): string {
  return path.slice(path.lastIndexOf('/') + 1)
}

// --- Multipart ---------------------------------------------------------------
/**
 * Pull the first file part out of a multipart/form-data body. The device streams
 * the part's bytes straight to fopen/fwrite, so the mock must land the REAL name
 * and the REAL content — a placeholder would make folder upload untestable.
 * Returns null for a body that is not multipart (then the caller treats the whole
 * body as the content).
 *
 * Body and content are binary strings; the delimiters and headers are ASCII, so
 * the same slicing works for a PNG as for a text file.
 */
function parseMultipartFile(body: string): { filename: string; content: string } | null {
  if (!body.startsWith('--')) return null
  const eol = body.indexOf('\r\n')
  if (eol < 3) return null
  const boundary = body.slice(0, eol)
  for (const part of body.split(boundary)) {
    const headerEnd = part.indexOf('\r\n\r\n')
    if (headerEnd < 0) continue
    const filename = /filename="([^"]*)"/i.exec(part.slice(0, headerEnd))
    if (!filename) continue
    let content = part.slice(headerEnd + 4)
    // The CRLF before the next delimiter belongs to the delimiter, not the file.
    if (content.endsWith('\r\n')) content = content.slice(0, -2)
    return { filename: filename[1] ?? '', content }
  }
  return null
}

// --- Store -------------------------------------------------------------------
/** Fresh, isolated mock state (built from the seed, so instances never share it). */
export function createFileBrowserMockStore(options: FileBrowserMockOptions = {}): FileBrowserMockStore {
  const apiBase = options.apiBase ?? '/files'
  const totalBytes = options.totalBytes ?? DEFAULT_TOTAL_BYTES

  let fs = new Map<string, MockNode>()
  let clock = SEED_MTIME

  function seed(): void {
    fs = new Map<string, MockNode>()
    clock = SEED_MTIME
    fs.set('/', { type: 'directory', content: '', mtime: SEED_MTIME })
    for (const entry of seedTree) {
      clock += 60
      fs.set(normalize(entry.path), {
        type: entry.content === null ? 'directory' : 'file',
        // Seeds are text; the store holds bytes, so encode once here.
        content: entry.content === null ? '' : textToBinary(entry.content),
        mtime: clock
      })
    }
  }
  seed()

  /** Monotonic mtime for a mutation — deterministic, unlike Date.now(). */
  const tick = () => ++clock

  const isDir = (path: string) => fs.get(path)?.type === 'directory'
  // One char per byte, so length is the real size — for a PNG and for '°C' alike.
  const sizeOf = (node: MockNode) => (node.type === 'directory' ? 0 : node.content.length)

  // Typed as FileApiResponse so the envelope cannot drift from the contract.
  const ok = (message: string): MockResult => ({
    status: 200,
    body: { success: true, message } satisfies FileApiResponse
  })
  const err = (error: string, status = 400): MockResult => ({
    status,
    body: { success: false, error } satisfies FileApiResponse
  })

  function list(path: string): FileEntry[] {
    const out: FileEntry[] = []
    for (const [p, node] of fs) {
      if (p !== path && parentOf(p) === path) {
        out.push({ name: nameOf(p), type: node.type, size: sizeOf(node), mtime: node.mtime })
      }
    }
    return out
  }

  function usedBytes(): number {
    let used = BASE_USED_BYTES
    for (const node of fs.values()) used += sizeOf(node)
    return used
  }

  function deleteTree(path: string): void {
    const prefix = childPrefix(path)
    for (const p of [...fs.keys()]) {
      if (p === path || p.startsWith(prefix)) fs.delete(p)
    }
    // The mount point itself survives a recursive wipe of the root.
    if (path === '/') fs.set('/', { type: 'directory', content: '', mtime: tick() })
  }

  function moveTree(from: string, to: string): void {
    const prefix = childPrefix(from)
    for (const [p, node] of [...fs]) {
      if (p === from || p.startsWith(prefix)) {
        fs.delete(p)
        // rename() keeps mtimes; only the entry's path changes.
        fs.set(to + p.slice(from.length), node)
      }
    }
  }

  function copyTree(from: string, to: string): void {
    const prefix = childPrefix(from)
    for (const [p, node] of [...fs]) {
      if (p === from || p.startsWith(prefix)) {
        fs.set(to + p.slice(from.length), { ...node, mtime: tick() })
      }
    }
  }

  /**
   * getParam() semantics: the device reads a parameter from the query string AND
   * from an urlencoded POST body, so form fields and query params are
   * interchangeable; the query wins. /write is the exception — its body is the
   * raw file content — and /upload's body is multipart, not urlencoded.
   */
  function readParams(endpoint: string, search: URLSearchParams, body: string): URLSearchParams {
    const params = new URLSearchParams(search)
    if (!body || endpoint.startsWith('/write') || endpoint.startsWith('/upload')) return params
    try {
      for (const [k, v] of new URLSearchParams(body)) if (!params.has(k)) params.append(k, v)
    } catch {
      /* not urlencoded — nothing to merge */
    }
    return params
  }

  /** Endpoint under apiBase ('/list', '/copy', …), or null when the path is not ours. */
  function endpointOf(pathname: string): string | null {
    if (!pathname.startsWith(apiBase)) return null
    const rest = pathname.slice(apiBase.length)
    return rest === '' || rest.startsWith('/') ? rest : null
  }

  /** The device rejects any path containing '..' — a substring check, not a segment one. */
  const isValidPath = (path: string) => !path.includes('..')

  /** The one method each route answers — routes.h on the device, same order. */
  const ROUTE_METHOD: ReadonlyArray<readonly [string, 'GET' | 'POST']> = [
    ['info', 'GET'],
    ['list', 'GET'],
    ['read', 'GET'],
    ['download', 'GET'],
    ['write', 'POST'],
    ['upload', 'POST'],
    ['delete', 'POST'],
    ['mkdir', 'POST'],
    ['rename', 'POST'],
    ['copy', 'POST']
  ]

  function handleUpload(params: URLSearchParams, body: string): MockResult {
    const part = parseMultipartFile(body)
    const content = part ? part.content : body
    const raw = params.get('path') ?? (part?.filename || '')
    if (!raw) return err('Missing path parameter')
    // `path` is the FULL destination path incl. the filename (the device fopen()s
    // exactly it) — that is what makes a folder upload land its real tree.
    const path = normalize(raw)
    if (!isValidPath(path)) return err('Invalid path')
    if (path.includes(UPLOAD_FAIL_MARKER)) return err('Failed to open file for writing')
    // The device's multipart reader skips zero-length parts, so its upload handler
    // never runs and nothing is written. The SDK routes empty files to /write.
    if (content === '') return err('No file received')
    // mkdir() on the device is not recursive, so a client that skipped the parent
    // chain must see fopen() fail here rather than a silent success.
    if (!isDir(parentOf(path)) || isDir(path)) return err('Failed to open file for writing')
    fs.set(path, { type: 'file', content, mtime: tick() })
    return ok('Upload complete')
  }

  function handleCopy(params: URLSearchParams): MockResult {
    const rawOld = params.get('old_path')
    const rawNew = params.get('new_path')
    if (rawOld === null) return err('Missing old_path parameter')
    if (rawNew === null) return err('Missing new_path parameter')
    const oldPath = normalize(rawOld)
    const newPath = normalize(rawNew)
    if (!isValidPath(oldPath)) return err('Invalid source path')
    if (!isValidPath(newPath)) return err('Invalid destination path')
    if (!fs.has(oldPath)) return err('Source not found', 404)
    // Checked BEFORE the destination-exists guard: old_path == new_path satisfies
    // both, and copy-into-itself is the more useful diagnosis. Without this guard
    // a recursive copy walks the tree it is growing.
    if (newPath === oldPath || newPath.startsWith(childPrefix(oldPath))) return err('Cannot copy into itself')
    if (fs.has(newPath)) return err('Destination already exists')
    if (!isDir(parentOf(newPath))) return err('Failed to copy')
    copyTree(oldPath, newPath)
    return ok('Copied successfully')
  }

  function handleRename(params: URLSearchParams): MockResult {
    const rawOld = params.get('old_path')
    const rawNew = params.get('new_path')
    if (rawOld === null) return err('Missing old_path parameter')
    if (rawNew === null) return err('Missing new_path parameter')
    const oldPath = normalize(rawOld)
    const newPath = normalize(rawNew)
    if (!isValidPath(oldPath)) return err('Invalid source path')
    if (!isValidPath(newPath)) return err('Invalid destination path')
    if (!fs.has(oldPath)) return err('Source file not found', 404)
    if (fs.has(newPath)) return err('Destination already exists')
    // POSIX rename() fails with EINVAL when the destination is inside the source.
    if (newPath.startsWith(childPrefix(oldPath))) return err('Failed to rename')
    if (!isDir(parentOf(newPath))) return err('Failed to rename')
    moveTree(oldPath, newPath)
    return ok('Renamed successfully')
  }

  function handle(method: string, pathname: string, search: URLSearchParams, body: string): MockResult | null {
    const endpoint = endpointOf(pathname)
    if (endpoint === null) return null
    // Mutating routes are POST-only on the device, so a GET must fail here too —
    // otherwise the dev server accepts what the firmware answers with a 405.
    const route = ROUTE_METHOD.find(([name]) => endpoint.startsWith(`/${name}`))
    if (route && method.toUpperCase() !== route[1]) return err('Method not allowed', 405)
    const params = readParams(endpoint, search, body)
    const raw = params.get('path')
    const path = raw === null ? null : normalize(raw)

    if (endpoint === '/info') {
      const used = usedBytes()
      const info: StorageInfo = {
        valid: true,
        total: totalBytes,
        used,
        free: Math.max(0, totalBytes - used),
        filesystem: 'littlefs'
      }
      return { status: 200, body: info }
    }

    // The backend matches with starts_with, so trailing junk hits the same route.
    if (endpoint.startsWith('/list')) {
      const dir = path ?? '/'
      if (!isValidPath(dir)) return err('Invalid path')
      if (!isDir(dir)) return err('Failed to open directory')
      // A BARE ARRAY, not the {success} envelope — see fileApi.list().
      return { status: 200, body: list(dir) }
    }

    if (endpoint.startsWith('/download')) {
      // Raw bytes are not a MockResult; the transport must call rawDownload().
      return null
    }

    if (endpoint.startsWith('/upload')) return handleUpload(params, body)

    if (endpoint.startsWith('/read')) {
      if (path === null) return err('Missing path parameter')
      if (!isValidPath(path)) return err('Invalid path')
      const node = fs.get(path)
      if (!node || node.type !== 'file') return err('Failed to open file', 404)
      if (node.content.length > 1024 * 1024) return err('File too large to edit')
      // The envelope carries TEXT; the JSON reply is UTF-8 on the wire.
      return { status: 200, body: { success: true, content: binaryToText(node.content) } satisfies FileApiResponse }
    }

    if (endpoint.startsWith('/write')) {
      if (path === null) return err('Missing path parameter')
      if (!isValidPath(path)) return err('Invalid path')
      if (isDir(path) || !isDir(parentOf(path))) return err('Failed to open file for writing')
      // The RAW body is the content — never urlencoded, never JSON. It arrives
      // already byte-per-char, so it is stored verbatim.
      fs.set(path, { type: 'file', content: body, mtime: tick() })
      return ok('File written successfully')
    }

    if (endpoint.startsWith('/delete')) {
      if (path === null) return err('Missing path parameter')
      if (!isValidPath(path)) return err('Invalid path')
      if (!fs.has(path)) return err('File not found', 404)
      deleteTree(path)
      return ok('Deleted successfully')
    }

    if (endpoint.startsWith('/mkdir')) {
      if (path === null) return err('Missing path parameter')
      if (!isValidPath(path)) return err('Invalid path')
      // Idempotent: recursive folder upload walks the chain level by level and
      // re-creates directories it already made. A path taken by a FILE still fails.
      if (isDir(path)) return ok('Directory already exists')
      if (fs.has(path) || !isDir(parentOf(path))) return err('Failed to create directory')
      fs.set(path, { type: 'directory', content: '', mtime: tick() })
      return ok('Directory created successfully')
    }

    if (endpoint.startsWith('/copy')) return handleCopy(params)
    if (endpoint.startsWith('/rename')) return handleRename(params)

    return null
  }

  function rawDownload(pathname: string, search: URLSearchParams): Uint8Array | null {
    const endpoint = endpointOf(pathname)
    if (endpoint === null || !endpoint.startsWith('/download')) return null
    const raw = search.get('path')
    if (raw === null) return null
    const node = fs.get(normalize(raw))
    if (!node || node.type !== 'file') return null
    return binaryToBytes(node.content)
  }

  return {
    handle,
    rawDownload,
    uploadDelayMs: options.uploadDelayMs ?? DEFAULT_UPLOAD_DELAY_MS,
    reset: seed
  }
}

// --- Transport: fetch adapter (programmatic / unit tests) --------------------
/**
 * A fetch implementation backed by `store` — inject into createFileBrowserApi()
 * to exercise the SDK with no server:
 *   const store = createFileBrowserMockStore()
 *   const api = createFileBrowserApi({ base: '/files', fetchImpl: createMockFetch(store) })
 *
 * It answers immediately: `uploadDelayMs` is for the dev-server transport (the
 * progress/cancel path runs over XHR, which never reaches a FetchImpl anyway), so
 * stalling here would only make tests sleep.
 */
export function createMockFetch(store: FileBrowserMockStore): (url: string, init?: RequestInit) => Promise<Response> {
  return async (url, init) => {
    const u = new URL(url, 'http://localhost')
    const method = (init?.method ?? 'GET').toUpperCase()

    const bytes = store.rawDownload(u.pathname, u.searchParams)
    if (bytes) {
      const name = nameOf(normalize(u.searchParams.get('path') ?? ''))
      return new Response(bytes, {
        status: 200,
        headers: {
          'Content-Type': 'application/octet-stream',
          'Content-Disposition': `attachment; filename="${name}"`
        }
      })
    }

    const result = store.handle(method, u.pathname, u.searchParams, await bodyToBinary(init?.body))
    const { status, body } = result ?? { status: 404, body: { success: false, error: 'Not Found' } }
    return new Response(JSON.stringify(body), {
      status,
      headers: { 'Content-Type': 'application/json' }
    })
  }
}

/**
 * Body of an outgoing request as a BINARY STRING — the store's currency, so a
 * File in a FormData round-trips byte for byte. FormData/Blob go through Response
 * so this file needs no DOM types (`as never` keeps BodyInit out of a node-only
 * build) and the store sees the same multipart bytes a browser would send; a
 * plain string body is UTF-8 encoded first, exactly as fetch() would put it on
 * the wire.
 */
async function bodyToBinary(body: unknown): Promise<string> {
  if (body === undefined || body === null) return ''
  if (typeof body === 'string') return textToBinary(body)
  try {
    return bytesToBinary(new Uint8Array(await new Response(body as never).arrayBuffer()))
  } catch {
    return ''
  }
}
