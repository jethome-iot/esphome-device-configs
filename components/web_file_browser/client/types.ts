// Wire types of the web_file_browser API, mirrored from the C++ backend; the
// dashboard's Files screen consumes them through the @fb alias.

export interface FileEntry {
  name: string
  type: 'file' | 'directory'
  size: number
  mtime: number
}

export interface StorageInfo {
  valid: boolean
  total: number
  used: number
  free: number
  filesystem: string
}

/** Generic {success,error?,message?,content?} envelope for read/write/delete/rename/mkdir. */
export interface FileApiResponse {
  success: boolean
  error?: string
  message?: string
  content?: string
}
