// Device identity and status from the web_device_dashboard backend. Reuse via the @da
// alias; inject base (`${basePath}/api/device`) and a fetch wrapper.
import { createHttp, type HttpOptions } from './http'
import type { Capabilities, ConfirmPayload, DeviceInfo, DeviceStatus, MutationResponse } from './types'

// A class, so a value export: a consumer needs it for `instanceof`, not only for types.
export { ApiError } from './http'
export type { FetchImpl, HttpOptions } from './http'

export interface DeviceApi {
  /** GET /info — device identity. */
  info(): Promise<DeviceInfo>
  /** GET /status — runtime status. */
  status(): Promise<DeviceStatus>
  /** GET /capabilities — what this firmware has. Read once, on load. */
  capabilities(): Promise<Capabilities>
  /** POST /system/reboot — requires a confirmation. */
  reboot(confirm: ConfirmPayload): Promise<MutationResponse>
  /** POST /system/factory-reset — requires a confirmation. Clears the settings, and the
   *  user partition when `capabilities.factory_reset.clears_storage`. */
  factoryReset(confirm: ConfirmPayload): Promise<MutationResponse>
  /** POST /system/rollback — requires a confirmation. Boots the other app slot, the one
   *  `capabilities.rollback` describes; `503` without one, `500` when that slot turns out
   *  not to hold a whole image. */
  rollback(confirm: ConfirmPayload): Promise<MutationResponse>
}

export function createDeviceApi(options: HttpOptions): DeviceApi {
  const http = createHttp(options)
  return {
    info() {
      return http.jget<DeviceInfo>('/info')
    },
    status() {
      return http.jget<DeviceStatus>('/status')
    },
    capabilities() {
      return http.jget<Capabilities>('/capabilities')
    },
    reboot(confirm) {
      return http.jpost<MutationResponse>('/system/reboot', confirm)
    },
    factoryReset(confirm) {
      return http.jpost<MutationResponse>('/system/factory-reset', confirm)
    },
    rollback(confirm) {
      return http.jpost<MutationResponse>('/system/rollback', confirm)
    }
  }
}
