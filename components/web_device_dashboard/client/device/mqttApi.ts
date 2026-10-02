// The MQTT client's settings and state, on a firmware with mqtt_config, and its subscription
// slots, on one with mqtt_subscriptions. Reuse via the @da alias; inject base
// (`${basePath}/api/device`) and a fetch wrapper.
import { createHttp, type HttpOptions } from './http'
import type {
  MqttSaveResult,
  MqttSettings,
  MqttSettingsUpdate,
  MqttSlotSave,
  MqttSlotSaveResult,
  MqttSubscriptions
} from './types'

export interface MqttApi {
  /** GET /mqtt — the stored settings (never the password) and what runs this boot. */
  get(): Promise<MqttSettings>
  /**
   * POST /mqtt — a partial update: send only what changed; `password: ''` clears it. The
   * first enable in a boot connects at once, unless the effective topic prefix differs from
   * the one this boot started with; later changes wait for a reboot, which the answer's
   * `reboot_required` and its message say. Refusals carry the device's message, the same as
   * mqttRules.ts gives.
   */
  set(update: MqttSettingsUpdate): Promise<MqttSaveResult>
  /** GET /mqtt/subscriptions — every slot as saved, with what runs (`capabilities.mqtt_subscriptions`). */
  getSubscriptions(): Promise<MqttSubscriptions>
  /** POST /mqtt/subscriptions — saves one slot whole; it applies after a reboot. Refusals carry
   *  the device's message, the same as mqttRules.ts gives where it can tell. */
  saveSlot(slot: MqttSlotSave): Promise<MqttSlotSaveResult>
  /** POST /mqtt/subscriptions `{slot, action: 'clear'}` — empties one slot (1-based). */
  clearSlot(slot: number): Promise<MqttSlotSaveResult>
}

export function createMqttApi(options: HttpOptions): MqttApi {
  const http = createHttp(options)
  return {
    get() {
      return http.jget<MqttSettings>('/mqtt')
    },
    set(update) {
      return http.jpost<MqttSaveResult>('/mqtt', update)
    },
    getSubscriptions() {
      return http.jget<MqttSubscriptions>('/mqtt/subscriptions')
    },
    saveSlot(slot) {
      return http.jpost<MqttSlotSaveResult>('/mqtt/subscriptions', slot)
    },
    clearSlot(slot) {
      return http.jpost<MqttSlotSaveResult>('/mqtt/subscriptions', { slot, action: 'clear' })
    }
  }
}
