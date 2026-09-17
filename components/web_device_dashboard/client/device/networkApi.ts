// Live network status of the web_device_dashboard backend. Reuse via the @da
// alias; inject base (`${basePath}/api/device`) and a fetch wrapper.
import { createHttp, type HttpOptions } from './http'
import type { NetworkLiveStatus } from './types'

export interface NetworkApi {
  /** GET /network — live connection status. */
  liveStatus(): Promise<NetworkLiveStatus>
}

export function createNetworkApi(options: HttpOptions): NetworkApi {
  const http = createHttp(options)
  return {
    liveStatus() {
      return http.jget<NetworkLiveStatus>('/network')
    }
  }
}
