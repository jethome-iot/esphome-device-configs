// The MQTT client's settings and state, on a firmware with mqtt_config. Reuse via the @da
// alias; inject base (`${basePath}/api/device`) and a fetch wrapper.
import { createHttp, type HttpOptions } from './http'
import type { MqttSaveResult, MqttSettings, MqttSettingsUpdate } from './types'

export interface MqttApi {
  /** GET /mqtt — the stored settings (never the password) and what runs this boot. */
  get(): Promise<MqttSettings>
  /**
   * POST /mqtt — a partial update: send only what changed; `password: ''` clears it. The
   * first enable in a boot connects at once; later changes wait for a reboot, which the
   * answer's `reboot_required` and its message say. Refusals carry the device's message,
   * the same as mqttRules.ts gives.
   */
  set(update: MqttSettingsUpdate): Promise<MqttSaveResult>
}

export function createMqttApi(options: HttpOptions): MqttApi {
  const http = createHttp(options)
  return {
    get() {
      return http.jget<MqttSettings>('/mqtt')
    },
    set(update) {
      return http.jpost<MqttSaveResult>('/mqtt', update)
    }
  }
}
