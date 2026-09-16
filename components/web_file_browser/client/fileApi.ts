// TypeScript client for the web_file_browser HTTP API. Lives with the backend
// component that owns the /files/* routes; the dashboard injects its base URL
// and fetch wrapper through createFileBrowserApi().
//
// Backend quirks encoded here:
//  - every route answers one method only (405 otherwise), the one used below;
//  - getParam() reads both the query string AND the urlencoded POST body, so the
//    form-encoded bodies below are equivalent to query params;
//  - /write expects the RAW request body (not urlencoded, not JSON);
//  - /upload reports a failed on-device write as {"success":false,"error":...};
//    older firmware answered a bare 200 with no body no matter what, so an empty
//    or non-JSON body on a 2xx is still taken as success.
import type { FileEntry, StorageInfo, FileApiResponse } from './types'

export type { FileEntry, StorageInfo, FileApiResponse } from './types'

/** Thrown when an in-flight upload is cancelled via its AbortSignal. */
export class UploadCancelledError extends Error {
  constructor() {
    super('Upload cancelled')
    this.name = 'UploadCancelledError'
  }
}

/** Injectable fetch — pass one that adds a timeout / auth / logging. Defaults to window.fetch. */
export type FetchImpl = (url: string, init?: RequestInit) => Promise<Response>

export interface FileBrowserApiOptions {
  /** Base URL for the /files API, e.g. "/files" or `${basePath}/files` (no trailing slash). */
  base: string
  /** Fetch implementation. Defaults to `window.fetch`. */
  fetchImpl?: FetchImpl
}

/** One file of a folder upload. `relativePath` uses "/" and never starts with one. */
export interface UploadItem {
  relativePath: string
  size: number
}

/** Directories to create (parents first) and the byte total, for a folder upload. */
export interface UploadPlan {
  folders: string[]
  totalBytes: number
}

export interface FileBrowserApi {
  /** GET /list?path= — directory contents. */
  list(path: string): Promise<FileEntry[]>
  /** GET /info — filesystem capacity/usage. */
  info(): Promise<StorageInfo>
  /** GET /read?path= — text file contents (backend caps at 1 MB). */
  read(path: string): Promise<string>
  /** POST /write?path= — save/create a text file (raw body). */
  write(path: string, content: string): Promise<void>
  /** POST /delete — delete a file or (recursively) a directory. */
  remove(path: string): Promise<void>
  /** POST /mkdir — create a directory (idempotent: an existing directory is a success). */
  mkdir(path: string): Promise<void>
  /** Create every level of `path` that is missing — the device's mkdir is not recursive. */
  ensureDir(path: string): Promise<void>
  /** POST /rename — rename OR move (params old_path/new_path). */
  rename(oldPath: string, newPath: string): Promise<void>
  /** POST /copy — copy a file or (recursively) a directory (params old_path/new_path). */
  copy(oldPath: string, newPath: string): Promise<void>
  /** `name`, or the first free "name (n)" in `destDir` if it is taken. */
  uniqueName(destDir: string, name: string): Promise<string>
  /** Directories + byte total implied by a folder upload (pure — see planUpload). */
  planUpload(items: UploadItem[]): UploadPlan
  /** Build a direct download/preview URL (for <img src> / <a download>). */
  downloadUrl(path: string): string
  /** GET /download?path= — fetch the file as a Blob. */
  downloadBlob(path: string): Promise<Blob>
  /** POST /upload?path= — simple fetch-based upload (no progress). */
  upload(path: string, file: File): Promise<void>
  /** POST /upload?path= — XHR upload with progress + cancellation. */
  uploadWithProgress(
    path: string,
    file: File,
    onProgress?: (percent: number) => void,
    abortSignal?: AbortSignal
  ): Promise<void>
  /** Every subfolder under `path`, recursively (for a move dialog); throws when any level fails to list. */
  listFoldersRecursive(path: string): Promise<string[]>
}

/**
 * Directories and byte total implied by a set of relative upload paths.
 *
 * Pure and DOM-free so both frontends share it; the browser-specific traversal
 * (webkitGetAsEntry / webkitRelativePath) stays in the apps. `folders` is deduped
 * and ordered parents-before-children, relative to the upload destination, so a
 * caller can iterate it straight into ensureDir()/mkdir().
 */
export function planUpload(items: UploadItem[]): UploadPlan {
  const folders: string[] = []
  const seen = new Set<string>()
  let totalBytes = 0
  for (const item of items) {
    totalBytes += item.size
    const parts = item.relativePath.split('/').filter((p) => p.length > 0)
    parts.pop() // the file name itself
    let prefix = ''
    for (const part of parts) {
      prefix = prefix === '' ? part : `${prefix}/${part}`
      if (seen.has(prefix)) continue
      seen.add(prefix)
      folders.push(prefix)
    }
  }
  return { folders, totalBytes }
}

// Upload responses: an empty or non-JSON body means older firmware that always
// answered a bare 200 — no verdict, so the caller keeps trusting the status.
function parseEnvelope(body: string): FileApiResponse | null {
  if (body === '') return null
  try {
    return JSON.parse(body) as FileApiResponse
  } catch {
    return null
  }
}

// The device reports every failure as the envelope AND a non-2xx status (400, or
// 404 for a missing path), so the body has to be read before the status is judged
// — otherwise the reason is discarded and the user gets a bare status code.
// Returns the parsed body; null for an empty or non-JSON one.
async function readJson(res: Response, failMsg: string): Promise<unknown> {
  const data = parseEnvelope(await res.text())
  if (data !== null && !Array.isArray(data) && data.success === false) {
    throw new Error(data.error ?? failMsg)
  }
  if (!res.ok) throw new Error(`${failMsg}: ${res.status}`)
  return data
}

export function createFileBrowserApi(options: FileBrowserApiOptions): FileBrowserApi {
  const base = options.base
  const doFetch: FetchImpl = options.fetchImpl ?? ((url, init) => fetch(url, init))

  // POST an application/x-www-form-urlencoded body (delete / mkdir / rename / copy).
  async function form(fields: Record<string, string>, endpoint: string, failMsg: string): Promise<void> {
    const params = new URLSearchParams()
    for (const [k, v] of Object.entries(fields)) params.append(k, v)
    const res = await doFetch(`${base}/${endpoint}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: params.toString()
    })
    const data = (await readJson(res, failMsg)) as FileApiResponse | null
    if (data === null || !data.success) throw new Error(data?.error ?? 'Unknown error')
  }

  const api: FileBrowserApi = {
    async list(path) {
      const res = await doFetch(`${base}/list?path=${encodeURIComponent(path)}`)
      const data = await readJson(res, 'Failed to list')
      if (!Array.isArray(data)) throw new Error('Invalid response')
      return data as FileEntry[]
    },

    async info() {
      const res = await doFetch(`${base}/info`)
      return (await readJson(res, 'Failed to fetch storage info')) as StorageInfo
    },

    async read(path) {
      const res = await doFetch(`${base}/read?path=${encodeURIComponent(path)}`)
      const data = (await readJson(res, 'Failed to read file')) as FileApiResponse | null
      if (data === null || !data.success) throw new Error(data?.error ?? 'Unknown error')
      return data.content ?? ''
    },

    async write(path, content) {
      // RAW body — do NOT urlencode (consumed by the generic POST handler) or
      // JSON-encode (quotes written into the file verbatim).
      const res = await doFetch(`${base}/write?path=${encodeURIComponent(path)}`, {
        method: 'POST',
        body: content
      })
      const data = (await readJson(res, 'Failed to save file')) as FileApiResponse | null
      if (data === null || !data.success) throw new Error(data?.error ?? 'Unknown error')
    },

    remove(path) {
      return form({ path }, 'delete', 'Failed to delete')
    },

    mkdir(path) {
      return form({ path }, 'mkdir', 'Failed to create directory')
    },

    async ensureDir(path) {
      const parts = path.split('/').filter((p) => p.length > 0)
      let current = ''
      for (let i = 0; i < parts.length; i++) {
        current += `/${parts[i]}`
        try {
          await api.mkdir(current)
        } catch (error) {
          // Older firmware errors on an existing directory instead of being
          // idempotent, and the parents normally DO exist — so an intermediate
          // level may fail; a real problem shows up on the last level or on the
          // upload that follows.
          if (i === parts.length - 1) throw error
        }
      }
    },

    rename(oldPath, newPath) {
      return form({ old_path: oldPath, new_path: newPath }, 'rename', 'Failed to rename')
    },

    copy(oldPath, newPath) {
      return form({ old_path: oldPath, new_path: newPath }, 'copy', 'Failed to copy')
    },

    async uniqueName(destDir, name) {
      // A failed listing must propagate: /upload and /write both truncate, so
      // falling back to `name` would silently overwrite whatever is already there.
      const entries = await api.list(destDir)
      const taken = new Set(entries.map((entry) => entry.name))
      const clashIsDir = entries.some((entry) => entry.name === name && entry.type === 'directory')
      if (!taken.has(name)) return name
      // Split on the LAST dot, and never on a leading one: "a.txt" -> "a (1).txt",
      // "archive.tar.gz" -> "archive.tar (1).gz", ".gitignore" -> ".gitignore (1)".
      // A directory has no extension, so "v1.2" must become "v1.2 (1)", not "v1 (1).2".
      const dot = clashIsDir ? -1 : name.lastIndexOf('.')
      const stem = dot > 0 ? name.slice(0, dot) : name
      const ext = dot > 0 ? name.slice(dot) : ''
      for (let n = 1; ; n++) {
        const candidate = `${stem} (${n})${ext}`
        if (!taken.has(candidate)) return candidate
      }
    },

    planUpload,

    downloadUrl(path) {
      return `${base}/download?path=${encodeURIComponent(path)}`
    },

    async downloadBlob(path) {
      const res = await doFetch(`${base}/download?path=${encodeURIComponent(path)}`)
      if (!res.ok) throw new Error(`Download failed: ${res.status}`)
      return res.blob()
    },

    async upload(path, file) {
      // The device's multipart reader skips zero-length parts, so an empty file
      // would never reach its upload handler. /write creates it for real.
      if (file.size === 0) return api.write(path, '')
      const formData = new FormData()
      formData.append('file', file)
      const res = await doFetch(`${base}/upload?path=${encodeURIComponent(path)}`, {
        method: 'POST',
        body: formData
      })
      // Body before status: the device answers a write failure with 400 AND the
      // envelope, so checking the status first would throw away the reason.
      const data = parseEnvelope(await res.text())
      if (data !== null && !data.success) throw new Error(data.error ?? 'Upload failed')
      if (!res.ok) throw new Error(`Upload failed: ${res.status}`)
    },

    uploadWithProgress(path, file, onProgress, abortSignal) {
      return new Promise((resolve, reject) => {
        if (abortSignal?.aborted) {
          reject(new UploadCancelledError())
          return
        }
        // Empty file: no multipart part would survive the device's reader — see upload().
        if (file.size === 0) {
          api.write(path, '').then(() => {
            onProgress?.(100)
            resolve()
          }, reject)
          return
        }
        const formData = new FormData()
        formData.append('file', file)
        const xhr = new XMLHttpRequest()
        xhr.upload.addEventListener('progress', (e: ProgressEvent) => {
          if (e.lengthComputable && onProgress) onProgress(Math.round((e.loaded / e.total) * 100))
        })
        xhr.addEventListener('load', () => {
          // Body before status, same reason as upload() above.
          const data = parseEnvelope(xhr.responseText)
          if (data !== null && !data.success) {
            reject(new Error(data.error ?? 'Upload failed'))
            return
          }
          if (xhr.status < 200 || xhr.status >= 300) {
            reject(new Error('Upload failed'))
            return
          }
          resolve()
        })
        xhr.addEventListener('error', () => reject(new Error('Upload failed')))
        xhr.addEventListener('abort', () => reject(new UploadCancelledError()))
        abortSignal?.addEventListener('abort', () => xhr.abort())
        xhr.open('POST', `${base}/upload?path=${encodeURIComponent(path)}`)
        xhr.send(formData)
      })
    },

    async listFoldersRecursive(path) {
      // A level that fails to list fails the walk: a partial tree would offer
      // move targets that may not exist and hide ones that do.
      const folders: string[] = []
      for (const file of await api.list(path)) {
        if (file.type !== 'directory') continue
        const folderPath = path === '/' ? '/' + file.name : path + '/' + file.name
        folders.push(folderPath, ...(await api.listFoldersRecursive(folderPath)))
      }
      return folders
    }
  }

  return api
}
