// In-process mock of the web_file_browser HTTP API, the dev-server and unit-test
// double for the contract in ../fileApi.ts and ../types.ts. Dependency-free:
// seed data, a stateful dispatcher and a fetch adapter; dev-server glue lives
// with each consumer (README, "client/").
//
// File content is a BINARY STRING — one char per byte, charCodeAt() in 0..255 —
// so `content.length` is the byte count and a PNG survives the round trip. A
// transport hands handle() the body decoded as latin1, never utf8, and writes
// rawDownload()'s bytes verbatim. Text crossing the API is UTF-8 on the wire.
//
// Behaviour mirrors web_file_browser.cpp: exact route names, one method each
// (405 otherwise), getParam() reads an urlencoded POST body first and the query
// string second, /write takes the raw body, /list and /info answer bare objects.
import type { FileEntry, StorageInfo, FileApiResponse } from '../types'

// --- Constants ---------------------------------------------------------------
/**
 * Fixed mtime base (2023-11-14T22:13:20Z). Seed and mutation mtimes both derive
 * from it — never Date.now() — so listings and screenshots do not churn.
 */
export const SEED_MTIME = 1_700_000_000

/**
 * An upload whose destination contains this marker fails with the error envelope,
 * so the UI's failed-upload path is reachable: upload a file named `fail-me.txt`.
 */
export const UPLOAD_FAIL_MARKER = 'fail-me'

const DEFAULT_TOTAL_BYTES = 4 * 1024 * 1024
const DEFAULT_UPLOAD_DELAY_MS = 500

/**
 * Added to the seeded byte count: a real filesystem is never empty, and a hairline
 * usage bar cannot be checked by eye. `used` still tracks every mutation.
 */
const BASE_USED_BYTES = 1024 * 1024

const ENCODER = new TextEncoder()
const DECODER = new TextDecoder()

// --- Bytes <-> binary string -------------------------------------------------
// The store's currency, converted here so every consumer converts identically.

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
 * Seed filesystem in creation order — what readdir() on the device returns and
 * `/list` preserves. `content: null` marks a directory.
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
   * Dispatch one API call; null when the path is not ours. `pathname` includes
   * apiBase, `body` is the request body as a binary string (latin1) and
   * `contentType` its Content-Type, which /write needs to refuse a form-encoded
   * or multipart body the way the device does.
   */
  handle(method: string, pathname: string, search: URLSearchParams, body: string, contentType?: string): MockResult | null
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
// Rebuild from segments, dropping empty and "." ones, exactly like resolve_path_()
// in the C++: /copy's subtree guard compares paths as strings. ".." stays, so
// that isValidPath can still reject it.
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
 * The first file part of a multipart/form-data body, real name and real bytes —
 * the device streams exactly those to fopen/fwrite, so a folder upload must land
 * its real tree. null when the body is not multipart.
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
        // LittleFS keeps no timestamp for a directory, so the device reports 0.
        const mtime = node.type === 'directory' ? 0 : node.mtime
        out.push({ name: nameOf(p), type: node.type, size: sizeOf(node), mtime })
      }
    }
    return out
  }

  /** The device recurses at most this deep into a tree it deletes or copies. */
  const MAX_DEPTH = 8

  /** Whether a directory sits more than MAX_DEPTH levels below `path`. */
  function tooDeep(path: string): boolean {
    const prefix = childPrefix(path)
    for (const [p, node] of fs) {
      if (node.type === 'directory' && p.startsWith(prefix) && p.slice(prefix.length).split('/').length > MAX_DEPTH) {
        return true
      }
    }
    return false
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
   * getParam() on the device searches an urlencoded POST body before the query
   * string, so a body field wins. /write's body is the file and /upload's is
   * multipart, so neither is a parameter source.
   */
  function readParams(endpoint: string, search: URLSearchParams, body: string): URLSearchParams {
    const params = new URLSearchParams(search)
    if (!body || endpoint === '/write' || endpoint === '/upload') return params
    try {
      for (const [k, v] of new URLSearchParams(body)) params.set(k, v)
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
    // Before the destination-exists guard: old_path == new_path trips both, and
    // copy-into-itself is the more useful diagnosis.
    if (newPath === oldPath || newPath.startsWith(childPrefix(oldPath))) return err('Cannot copy into itself')
    if (fs.has(newPath)) return err('Destination already exists')
    if (!isDir(parentOf(newPath))) return err('Failed to copy')
    // The device gives up past MAX_DEPTH and rolls the partial copy back.
    if (isDir(oldPath) && tooDeep(oldPath)) return err('Failed to copy')
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

  function handle(
    method: string,
    pathname: string,
    search: URLSearchParams,
    body: string,
    contentType?: string
  ): MockResult | null {
    const endpoint = endpointOf(pathname)
    if (endpoint === null) return null
    // Exact names and one method each, as on the device — otherwise the dev
    // server accepts what the firmware answers with a 404 or 405.
    const route = ROUTE_METHOD.find(([name]) => endpoint === `/${name}`)
    if (!route) return null
    if (method.toUpperCase() !== route[1]) return err('Method not allowed', 405)
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

    if (endpoint === '/list') {
      const dir = path ?? '/'
      if (!isValidPath(dir)) return err('Invalid path')
      if (!isDir(dir)) return err('Failed to open directory')
      // A BARE ARRAY, not the {success} envelope — see fileApi.list().
      return { status: 200, body: list(dir) }
    }

    if (endpoint === '/download') {
      // Raw bytes are not a MockResult; the transport must call rawDownload().
      return null
    }

    if (endpoint === '/upload') return handleUpload(params, body)

    if (endpoint === '/read') {
      if (path === null) return err('Missing path parameter')
      if (!isValidPath(path)) return err('Invalid path')
      const node = fs.get(path)
      if (!node || node.type !== 'file') return err('Failed to open file', 404)
      if (node.content.length > 1024 * 1024) return err('File too large to edit')
      // The envelope carries TEXT; the JSON reply is UTF-8 on the wire.
      return { status: 200, body: { success: true, content: binaryToText(node.content) } satisfies FileApiResponse }
    }

    if (endpoint === '/write') {
      if (path === null) return err('Missing path parameter')
      if (!isValidPath(path)) return err('Invalid path')
      // On the device such a body goes to the form or multipart parser and never
      // reaches the file, so it is refused rather than written as an empty file.
      if (body && /x-www-form-urlencoded|multipart\/form-data/i.test(contentType ?? '')) {
        return err('write takes a raw body, not form-encoded or multipart')
      }
      if (isDir(path) || !isDir(parentOf(path))) return err('Failed to open file for writing')
      // The RAW body is the content — never urlencoded, never JSON. It arrives
      // already byte-per-char, so it is stored verbatim.
      fs.set(path, { type: 'file', content: body, mtime: tick() })
      return ok('File written successfully')
    }

    if (endpoint === '/delete') {
      if (path === null) return err('Missing path parameter')
      if (!isValidPath(path)) return err('Invalid path')
      // An empty path normalises to the mount root; the device refuses to empty it.
      if (path === '/') return err('Cannot delete the mount root')
      if (!fs.has(path)) return err('File not found', 404)
      // Measured before anything is removed, as on the device.
      if (isDir(path) && tooDeep(path)) return err('Directory tree too deep to delete')
      deleteTree(path)
      return ok('Deleted successfully')
    }

    if (endpoint === '/mkdir') {
      if (path === null) return err('Missing path parameter')
      if (!isValidPath(path)) return err('Invalid path')
      // Idempotent: recursive folder upload walks the chain level by level and
      // re-creates directories it already made. A path taken by a FILE still fails.
      if (isDir(path)) return ok('Directory already exists')
      if (fs.has(path) || !isDir(parentOf(path))) return err('Failed to create directory')
      fs.set(path, { type: 'directory', content: '', mtime: tick() })
      return ok('Directory created successfully')
    }

    if (endpoint === '/copy') return handleCopy(params)
    if (endpoint === '/rename') return handleRename(params)

    return null
  }

  function rawDownload(pathname: string, search: URLSearchParams): Uint8Array | null {
    const endpoint = endpointOf(pathname)
    if (endpoint !== '/download') return null
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
 * A fetch backed by `store`, for createFileBrowserApi({ fetchImpl }) with no
 * server. Answers at once: `uploadDelayMs` is for the dev-server transport, and
 * the progress path runs over XHR, which never reaches a FetchImpl anyway.
 */
export function createMockFetch(store: FileBrowserMockStore): (url: string, init?: RequestInit) => Promise<Response> {
  return async (url, init) => {
    const u = new URL(url, 'http://localhost')
    const method = (init?.method ?? 'GET').toUpperCase()

    // Raw bytes only for a GET; any other method falls through to the 405.
    const bytes = method === 'GET' ? store.rawDownload(u.pathname, u.searchParams) : null
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

    // fetch() would label a FormData body multipart on the wire; a string body
    // carries whatever Content-Type the caller set.
    const contentType =
      typeof FormData !== 'undefined' && init?.body instanceof FormData
        ? 'multipart/form-data'
        : new Headers(init?.headers ?? {}).get('content-type') ?? undefined
    const result = store.handle(method, u.pathname, u.searchParams, await bodyToBinary(init?.body), contentType)
    const { status, body } = result ?? { status: 404, body: { success: false, error: 'Not Found' } }
    return new Response(JSON.stringify(body), {
      status,
      headers: { 'Content-Type': 'application/json' }
    })
  }
}

/**
 * Request body as a binary string. FormData and Blob go through Response, so
 * the store sees the multipart bytes a browser would send without this file
 * needing DOM types; a string body is UTF-8 encoded first, as fetch() does.
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
