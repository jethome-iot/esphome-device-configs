// The device clock and its timezone from the web_device_dashboard backend. Reuse via the @da
// alias; inject base (`${basePath}/api/device`) and a fetch wrapper. Present when
// `capabilities.time` is; otherwise `/time` answers `404` to GET and POST alike.
import { createHttp, type HttpOptions } from './http'
import type { DeviceTime, DeviceTimeUpdate } from './types'

export interface TimeApi {
  /** GET /time — the clock and the timezone in effect. */
  get(): Promise<DeviceTime>
  /** POST /time — follow Home Assistant's timezone or a chosen one, or set the clock, in force
   *  at once; answers what GET would after it. The refusals are on DeviceTimeUpdate. */
  set(update: DeviceTimeUpdate): Promise<DeviceTime>
}

export function createTimeApi(options: HttpOptions): TimeApi {
  const http = createHttp(options)
  return {
    get() {
      return http.jget<DeviceTime>('/time')
    },
    set(update) {
      return http.jpost<DeviceTime>('/time', update)
    }
  }
}
