// Device identity and status from the web_device_dashboard backend. Reuse via the @da
// alias; inject base (`${basePath}/api/device`) and a fetch wrapper.
import { createHttp, type HttpOptions } from './http'
import type { DeviceInfo, DeviceStatus } from './types'

// A class, so a value export: a consumer needs it for `instanceof`, not only for types.
export { ApiError } from './http'
export type { FetchImpl, HttpOptions } from './http'

export interface DeviceApi {
  /** GET /info — device identity. */
  info(): Promise<DeviceInfo>
  /** GET /status — runtime status. */
  status(): Promise<DeviceStatus>
}

export function createDeviceApi(options: HttpOptions): DeviceApi {
  const http = createHttp(options)
  return {
    info() {
      return http.jget<DeviceInfo>('/info')
    },
    status() {
      return http.jget<DeviceStatus>('/status')
    }
  }
}
