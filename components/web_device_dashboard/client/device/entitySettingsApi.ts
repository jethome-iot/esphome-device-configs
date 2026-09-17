// Per-entity settings of the web_device_dashboard backend. Reuse via the @da
// alias; inject base (`${basePath}/api/device`) and a fetch wrapper.
import { createHttp, type HttpOptions } from './http'
import type { EntityIndexResponse, EntitySettingsMetaResponse, EntitySettingsGetResponse } from './types'

export interface EntitySettingsApi {
  /** GET /entities — object_id and name of every settable entity, per type. */
  index(): Promise<EntityIndexResponse>
  /** GET /entity-settings-meta — field definitions per entity type. */
  meta(): Promise<EntitySettingsMetaResponse>
  /** GET /entity-settings?type= — every stored record of one type. */
  list(type: string): Promise<EntitySettingsGetResponse>
  /** GET /entity-settings?type=&source_name= — saved settings records. */
  get(type: string, sourceName: string): Promise<EntitySettingsGetResponse>
  /** POST /entity-settings — save settings for one entity. */
  save(type: string, sourceName: string, settings: Record<string, unknown>): Promise<void>
  /** POST /entity-settings — delete settings for one entity. */
  remove(type: string, sourceName: string): Promise<void>
}

export function createEntitySettingsApi(options: HttpOptions): EntitySettingsApi {
  const http = createHttp(options)
  return {
    index() {
      return http.jget<EntityIndexResponse>('/entities')
    },
    meta() {
      return http.jget<EntitySettingsMetaResponse>('/entity-settings-meta')
    },
    list(type) {
      return http.jget<EntitySettingsGetResponse>(`/entity-settings?type=${encodeURIComponent(type)}`)
    },
    get(type, sourceName) {
      const q = `type=${encodeURIComponent(type)}&source_name=${encodeURIComponent(sourceName)}`
      return http.jget<EntitySettingsGetResponse>(`/entity-settings?${q}`)
    },
    save(type, sourceName, settings) {
      return http.jpost<void>('/entity-settings', { type, source_name: sourceName, settings })
    },
    remove(type, sourceName) {
      return http.jpost<void>('/entity-settings', { type, source_name: sourceName, action: 'delete' })
    }
  }
}
