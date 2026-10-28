// virtual_inputs for a dev server's mock device, behind GET /virtual-inputs and POST
// /virtual-inputs/save and /delete: the slots in use and the inputs they may follow, with the
// firmware's refusals, in its order, with its codes and messages. Lives with the client it mocks
// so the two change together. No Vue, no app aliases and no timers, so node's test runner takes it
// as it is: a slot's state is its source's, as if every delay had passed.
import type {
  VirtualInput,
  VirtualInputSavePayload,
  VirtualInputSource,
  VirtualInputs
} from '../device/types'

export interface MockReply {
  status: number
  body: Record<string, unknown>
}

/** A slot in use at power-on; a delay left out is 0. */
export interface VirtualInputSeed {
  slot: number
  source: string
  on_delay?: number
  off_delay?: number
}

export interface VirtualInputsMockOptions {
  /** `virtual_inputs`' `max_inputs`; 16, as the JXD package sets it, when left out. */
  maxInputs?: number
  /** The physical inputs a slot may follow, in the device's order. */
  sources: ReadonlyArray<{ object_id: string; name: string }>
  /** The slots in use at power-on. */
  inputs?: readonly VirtualInputSeed[]
  /** The entity settings' label of an object id, `''` for none: the device keeps the labels there. */
  labelOf?: (objectId: string) => string
  /** A source's state, `null` before it has reported; off when left out. */
  stateOf?: (objectId: string) => boolean | null
  /** What a delete does to the entity settings: the device drops the slot's label with it. */
  onDelete?: (objectId: string) => void
  /** `nostorage` (the partition did not mount), `badfile` (the file is there but did not load),
   *  `writefail` (a write fails) or `busy` (the loop task misses every other request); anything
   *  else is a working device. */
  mode?: string
}

/** The longest on or off delay, in seconds: a day. */
export const MAX_DELAY = 86400

/** What the mock device boots with: someone is here until 10 min after the motion sensor last
 *  saw them. The automation mock's "Comfort while someone is home" rule follows this input. */
export const SOMEONE_HOME: VirtualInputSeed = { slot: 1, source: 'motion', off_delay: 600 }

/** The object id a slot's binary sensor has for good, whatever its label. */
export const virtualInputObjectId = (slot: number): string => `virtual_input_${slot}`

const NOT_ALLOWED: MockReply = { status: 405, body: { success: false, error: 'Method not allowed' } }

export function createVirtualInputsMock(options: VirtualInputsMockOptions) {
  const maxInputs = options.maxInputs ?? 16
  const mode = options.mode ?? ''
  const labelOf = options.labelOf ?? (() => '')
  const stateOf = options.stateOf ?? (() => false)
  const storage = mode !== 'nostorage'
  const fileLoaded = storage && mode !== 'badfile'

  interface Slot {
    source: string
    on_delay: number
    off_delay: number
  }
  const slots: Array<Slot | null> = Array.from({ length: maxInputs }, () => null)
  // Neither a partition that did not mount nor a file that did not load gives the boot any slot.
  if (fileLoaded) {
    for (const seed of options.inputs ?? []) {
      if (seed.slot >= 1 && seed.slot <= maxInputs)
        slots[seed.slot - 1] = { source: seed.source, on_delay: seed.on_delay ?? 0, off_delay: seed.off_delay ?? 0 }
    }
  }

  const isVirtual = (objectId: string) =>
    slots.some((_, i) => virtualInputObjectId(i + 1) === objectId)
  const sourceOf = (objectId: string) => options.sources.find((s) => s.object_id === objectId)

  const missed = { read: false, write: false }
  const busy = (kind: keyof typeof missed) => {
    if (mode !== 'busy') return false
    missed[kind] = !missed[kind]
    return missed[kind]
  }
  const refuse = (status: number, error: string): MockReply => ({ status, body: { success: false, error } })

  function row(index: number): VirtualInput {
    const slot = slots[index] as Slot
    const objectId = virtualInputObjectId(index + 1)
    const known = isVirtual(slot.source) ? null : sourceOf(slot.source)
    const level = known ? stateOf(slot.source) : false
    const problem = isVirtual(slot.source)
      ? `Source '${slot.source}' is a virtual input`
      : !known
        ? `Source '${slot.source}' not found`
        : level === null
          ? `Waiting for '${slot.source}' to report`
          : ''
    return {
      slot: index + 1,
      object_id: objectId,
      name: `Virtual input ${index + 1}`,
      label: labelOf(objectId),
      source: slot.source,
      on_delay: slot.on_delay,
      off_delay: slot.off_delay,
      state: level,
      problem
    }
  }

  /** GET /virtual-inputs */
  function list(): MockReply {
    if (busy('read')) return refuse(503, 'Device busy')
    const sources: VirtualInputSource[] = options.sources.map((s) => ({ ...s, label: labelOf(s.object_id) }))
    const body: VirtualInputs = {
      max_inputs: maxInputs,
      max_delay: MAX_DELAY,
      can_save: storage && fileLoaded,
      inputs: slots.flatMap((slot, i) => (slot ? [row(i)] : [])),
      sources
    }
    return { status: 200, body: body as unknown as Record<string, unknown> }
  }

  /** The body as the firmware's read_json_body_ takes it: an object, or `Invalid JSON`. */
  function parse(raw: string): Record<string, unknown> | null {
    try {
      const value: unknown = JSON.parse(raw)
      return value !== null && typeof value === 'object' && !Array.isArray(value)
        ? (value as Record<string, unknown>)
        : null
    } catch {
      return null
    }
  }

  const writable = (): MockReply | null =>
    !storage
      ? refuse(503, 'Virtual input storage unavailable')
      : !fileLoaded
        ? refuse(503, 'The virtual inputs file did not load; fix it and reboot')
        : null

  // JSON.parse cannot tell 1.0 from 1, which the device refuses: the mock takes both.
  const slotOk = (value: unknown): value is number =>
    typeof value === 'number' && Number.isInteger(value) && value >= 1 && value <= maxInputs
  const badSlot = () => refuse(400, `'slot' must be a number from 1 to ${maxInputs}`)
  const delayOk = (value: unknown): value is number =>
    typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= MAX_DELAY

  /** POST /virtual-inputs/save, from its raw body. */
  function save(raw: string): MockReply {
    const body = parse(raw)
    if (!body) return refuse(400, 'Invalid JSON')
    const unwritable = writable()
    if (unwritable) return unwritable
    const hasSlot = 'slot' in body
    if (hasSlot && !slotOk(body.slot)) return badSlot()
    const source = body.source
    if (typeof source !== 'string' || source === '') return refuse(400, "'source' must be the object id of an input")
    for (const key of ['on_delay', 'off_delay'] as const) {
      if (key in body && !delayOk(body[key]))
        return refuse(400, `'${key}' must be a whole number of seconds from 0 to ${MAX_DELAY}`)
    }
    if (busy('write')) return refuse(503, 'Device busy')
    if (isVirtual(source)) return refuse(400, `'${source}' is a virtual input; the source must be a physical one`)
    if (!sourceOf(source)) return refuse(400, `'${source}' is not an input`)
    const index = hasSlot ? (body.slot as number) - 1 : slots.findIndex((slot) => slot === null)
    if (index < 0) return refuse(409, `All ${maxInputs} virtual inputs are in use`)
    const was = slots[index]
    const payload = body as unknown as VirtualInputSavePayload
    const next: Slot = {
      source,
      on_delay: payload.on_delay ?? was?.on_delay ?? 0,
      off_delay: payload.off_delay ?? was?.off_delay ?? 0
    }
    const unchanged =
      was !== null && was.source === next.source && was.on_delay === next.on_delay && was.off_delay === next.off_delay
    if (!unchanged && mode === 'writefail') return refuse(500, 'The virtual inputs were not written')
    slots[index] = next
    return {
      status: 200,
      body: {
        success: true,
        message: `Virtual input ${index + 1} ${was === null ? 'created' : 'saved'}`,
        input: row(index)
      }
    }
  }

  /** POST /virtual-inputs/delete, from its raw body. */
  function remove(raw: string): MockReply {
    const body = parse(raw)
    if (!body) return refuse(400, 'Invalid JSON')
    const unwritable = writable()
    if (unwritable) return unwritable
    if (!slotOk(body.slot)) return badSlot()
    if (busy('write')) return refuse(503, 'Device busy')
    const index = body.slot - 1
    if (slots[index] === null) return refuse(409, `Virtual input ${index + 1} is free`)
    if (mode === 'writefail') return refuse(500, 'The virtual inputs were not written')
    slots[index] = null
    options.onDelete?.(virtualInputObjectId(index + 1))
    return { status: 200, body: { success: true, message: `Virtual input ${index + 1} deleted` } }
  }

  /** One request below `/api/device/`; `null` for a route that is not one of these three. */
  function handle(method: string, route: string, raw = ''): MockReply | null {
    if (route === 'virtual-inputs') return method === 'GET' ? list() : NOT_ALLOWED
    if (route === 'virtual-inputs/save') return method === 'POST' ? save(raw) : NOT_ALLOWED
    if (route === 'virtual-inputs/delete') return method === 'POST' ? remove(raw) : NOT_ALLOWED
    return null
  }

  /** The visible binary sensors the slots in use add to the device, in slot order: for its entity
   *  index (with `virtual: true`), web_server's entity list and the rule editor's inputs. */
  function entities(): Array<{ object_id: string; name: string }> {
    return slots.flatMap((slot, i) =>
      slot ? [{ object_id: virtualInputObjectId(i + 1), name: `Virtual input ${i + 1}` }] : []
    )
  }

  return { list, save, remove, handle, entities }
}
