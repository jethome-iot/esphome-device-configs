// The web server's HTTP credentials, on a firmware with a web_auth. Reuse via the @da
// alias; inject base (`${basePath}/api/device`) and a fetch wrapper.
import { createHttp, type HttpOptions } from './http'
import type { AuthStatus, MutationResponse } from './types'

export interface AuthApi {
  /** GET /auth — the username, the password's length, and whether it is still the factory pair. */
  get(): Promise<AuthStatus>
  /**
   * POST /auth — replace both. The device applies them from its main loop, so this call
   * answers under the old pair and the next request needs the new one.
   */
  set(username: string, password: string): Promise<MutationResponse>
}

export function createAuthApi(options: HttpOptions): AuthApi {
  const http = createHttp(options)
  return {
    get() {
      return http.jget<AuthStatus>('/auth')
    },
    set(username, password) {
      return http.jpost<MutationResponse>('/auth', { username, password })
    }
  }
}
