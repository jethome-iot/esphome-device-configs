// Device identity and status from the web_device_dashboard backend. Reuse via the @da
// alias; inject base (`${basePath}/api/device`) and a fetch wrapper.
import { createHttp, type HttpOptions } from './http'
import type {
  AssignSlotPayload,
  Capabilities,
  ConfirmPayload,
  DeviceInfo,
  DeviceStatus,
  ForgetSlotsPayload,
  MutationResponse,
  SlotLabelPayload,
  SlotOffsetPayload,
  TemperatureSlotChangeResult,
  TemperatureSlotLabelResult,
  TemperatureSlotOffsetResult,
  TemperatureSlots
} from './types'

// A class, so a value export: a consumer needs it for `instanceof`, not only for types.
export { ApiError } from './http'
export type { FetchImpl, HttpOptions } from './http'

export interface DeviceApi {
  /** GET /info — device identity. */
  info(): Promise<DeviceInfo>
  /** GET /status — runtime status. */
  status(): Promise<DeviceStatus>
  /** GET /capabilities — what this firmware has. Read on load, not polled. */
  capabilities(): Promise<Capabilities>
  /** POST /system/reboot — requires a confirmation. */
  reboot(confirm: ConfirmPayload): Promise<MutationResponse>
  /** POST /system/factory-reset — requires a confirmation. Clears the settings, and the
   *  user partition when `capabilities.factory_reset.clears_storage`. */
  factoryReset(confirm: ConfirmPayload): Promise<MutationResponse>
  /** POST /system/rollback — requires a confirmation. Boots the other app slot, the one
   *  `capabilities.rollback` describes; `503` without one or when the device is too busy to
   *  take it (`Device busy`, nothing selected), `500` when that slot turns out not to hold a
   *  whole image or stopped being one to go back to. */
  rollback(confirm: ConfirmPayload): Promise<MutationResponse>
  /** GET /temperature-slots — the `dallas_scan` slots; `404` without
   *  `capabilities.temperature_slots`, `503` when the loop task does not take it. */
  temperatureSlots(): Promise<TemperatureSlots>
  /** POST /temperature-slots/forget — requires a confirmation. Empties one slot or every
   *  unlisted one, applied after a reboot (every one also loses its offset and label, at once);
   *  `409` when that would change nothing, `503` when the table cannot be written or the loop
   *  task is busy (the error says which), `500` when the write fails. */
  forgetTemperatureSlots(payload: ForgetSlotsPayload): Promise<TemperatureSlotChangeResult>
  /** POST /temperature-slots/assign — requires a confirmation. Puts a device into a slot,
   *  swapping or displacing, applied after a reboot; `400` for a bad address, `409` when the
   *  YAML decides that slot or device, or nothing would change, `503` when the table cannot be
   *  written or the loop task is busy (the error says which), `500` when the write fails. */
  assignTemperatureSlot(payload: AssignSlotPayload): Promise<TemperatureSlotChangeResult>
  /** POST /temperature-slots/offset — sets a slot's offset, in force at once, no confirmation;
   *  `400` for a slot or offset out of range, `409` for a listed slot, `503` when the table
   *  cannot be written, its file did not load at boot or the loop task is busy (the error says
   *  which), `500` when the write fails. */
  setTemperatureSlotOffset(payload: SlotOffsetPayload): Promise<TemperatureSlotOffsetResult>
  /** POST /temperature-slots/label — sets or clears a slot's label, shown at once, no
   *  confirmation; `400` for a slot out of range or a label the rules refuse, `404` without
   *  `max_label_length`, `409` for a listed slot, `503` when the table cannot be written, its
   *  file did not load at boot or the loop task is busy (the error says which), `500` when the
   *  write fails. */
  setTemperatureSlotLabel(payload: SlotLabelPayload): Promise<TemperatureSlotLabelResult>
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
    },
    temperatureSlots() {
      return http.jget<TemperatureSlots>('/temperature-slots')
    },
    forgetTemperatureSlots(payload) {
      return http.jpost<TemperatureSlotChangeResult>('/temperature-slots/forget', payload)
    },
    assignTemperatureSlot(payload) {
      return http.jpost<TemperatureSlotChangeResult>('/temperature-slots/assign', payload)
    },
    setTemperatureSlotOffset(payload) {
      return http.jpost<TemperatureSlotOffsetResult>('/temperature-slots/offset', payload)
    },
    setTemperatureSlotLabel(payload) {
      return http.jpost<TemperatureSlotLabelResult>('/temperature-slots/label', payload)
    }
  }
}
