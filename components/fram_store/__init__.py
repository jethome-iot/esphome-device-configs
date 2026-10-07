"""Crash-safe slots and rings on a byte-addressable non-volatile part, laid out at build time."""

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_CAPACITY, CONF_ID, CONF_SIZE, CONF_TYPE
from esphome.core import CORE
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@jethome-iot"]
MULTI_CONF = True

CONF_BASE_OFFSET = "base_offset"
CONF_EEPROM_ID = "eeprom_id"
CONF_REGIONS = "regions"
CONF_RECORD_SIZE = "record_size"
CONF_RAM_BACKEND_ID = "ram_backend_id"
CONF_I2C_BACKEND_ID = "i2c_backend_id"

# Mirror SUPERBLOCK_SIZE and the header sizes in the C++ headers; the suite checks they agree.
SUPERBLOCK_SIZE = 32
SLOT_HEADER_SIZE = 8
RING_HEADER_SIZE = 6
ALIGNMENT = 4
DEFAULT_DEVICE_SIZE = 8192

TYPE_SLOT = "slot"
TYPE_RING = "ring"

# What i2c_eeprom reaches with its one address byte; 4 to 16 Kbit parts hold more than that.
ONE_BYTE_REACH = 256

fram_store_ns = cg.esphome_ns.namespace("fram_store")
FramStore = fram_store_ns.class_("FramStore", cg.Component)
FramRegion = fram_store_ns.class_("FramRegion")
FramSlot = fram_store_ns.class_("FramSlot", FramRegion)
FramRing = fram_store_ns.class_("FramRing", FramRegion)
NvramBackend = fram_store_ns.class_("NvramBackend")
RamBackend = fram_store_ns.class_("RamBackend", NvramBackend)
I2CEepromBackend = fram_store_ns.class_("I2CEepromBackend", NvramBackend)

# Declared, not imported: the part is optional here, and cv.use_id matches on the class name.
i2c_eeprom_ns = cg.esphome_ns.namespace("i2c_eeprom")
I2CEeprom = i2c_eeprom_ns.class_("I2CEeprom", cg.Component)

REGION_SCHEMA = cv.typed_schema(
    {
        TYPE_SLOT: cv.Schema(
            {
                cv.Required(CONF_ID): cv.declare_id(FramSlot),
                cv.Required(CONF_SIZE): cv.int_range(min=1, max=4096),
            }
        ),
        TYPE_RING: cv.Schema(
            {
                cv.Required(CONF_ID): cv.declare_id(FramRing),
                cv.Required(CONF_RECORD_SIZE): cv.int_range(min=1, max=1024),
                cv.Required(CONF_CAPACITY): cv.int_range(min=1, max=4096),
            }
        ),
    },
    key=CONF_TYPE,
    lower=True,
)


def _base_offset(value):
    value = cv.int_range(min=0, max=65535)(value)
    if value % ALIGNMENT:
        raise cv.Invalid(f"base_offset must be a multiple of {ALIGNMENT}")
    return value


def _region_size(region):
    if region[CONF_TYPE] == TYPE_SLOT:
        return 2 * (SLOT_HEADER_SIZE + region[CONF_SIZE])
    return (RING_HEADER_SIZE + region[CONF_RECORD_SIZE]) * region[CONF_CAPACITY]


def _region_signature(region):
    if region[CONF_TYPE] == TYPE_SLOT:
        return f"{region[CONF_ID]}:slot:{region[CONF_SIZE]}"
    return f"{region[CONF_ID]}:ring:{region[CONF_RECORD_SIZE]}:{region[CONF_CAPACITY]}"


def _fnv1a_32(text):
    value = 0x811C9DC5
    for byte in text.encode():
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def layout(config):
    """Where each region starts, where the last one ends, and the hash the superblock keeps.

    Every id, kind and size is in the hash, so changing any of them reformats the store
    instead of reading it at the old addresses.
    """
    offsets = []
    cursor = config[CONF_BASE_OFFSET] + SUPERBLOCK_SIZE
    for region in config[CONF_REGIONS]:
        offsets.append(cursor)
        cursor += _region_size(region)
        cursor += -cursor % ALIGNMENT

    # The constants are in the signature too: changing one moves every region without touching an id.
    signature = (
        f"v1|{config[CONF_BASE_OFFSET]}|{SUPERBLOCK_SIZE}|{ALIGNMENT}|{SLOT_HEADER_SIZE}|"
        f"{RING_HEADER_SIZE}|{config[CONF_SIZE]}|"
        + "|".join(_region_signature(region) for region in config[CONF_REGIONS])
    )
    return offsets, cursor, _fnv1a_32(signature)


def _validate_layout(config):
    base = config[CONF_BASE_OFFSET]
    _, end, _ = layout(config)
    if end > config[CONF_SIZE]:
        room = max(config[CONF_SIZE] - base, 0)
        raise cv.Invalid(
            f"The regions need {end - base} bytes including the {SUPERBLOCK_SIZE}-byte superblock, "
            f"but the device has {room} from base_offset {base:#06x} up",
            path=[CONF_REGIONS],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FramStore),
            cv.GenerateID(CONF_RAM_BACKEND_ID): cv.declare_id(RamBackend),
            cv.GenerateID(CONF_I2C_BACKEND_ID): cv.declare_id(I2CEepromBackend),
            cv.Optional(CONF_EEPROM_ID): cv.use_id(I2CEeprom),
            cv.Optional(CONF_SIZE, default=DEFAULT_DEVICE_SIZE): cv.int_range(
                min=SUPERBLOCK_SIZE, max=65536
            ),
            cv.Optional(CONF_BASE_OFFSET, default=0): _base_offset,
            cv.Required(CONF_REGIONS): cv.All(
                cv.ensure_list(REGION_SCHEMA), cv.Length(min=1)
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_layout,
)


def _part(full_config, eeprom_id):
    """The i2c_eeprom: entry eeprom_id names."""
    for entry in full_config.get("i2c_eeprom") or []:
        if (entry_id := entry.get(CONF_ID)) is not None and entry_id.id == eeprom_id.id:
            return entry
    return None


def _span(config):
    base = config[CONF_BASE_OFFSET]
    return f"0x{base:04X}-0x{layout(config)[1] - 1:04X}"


# Each store formats only its own range, so two that overlap would wipe each other at every boot.
# Reported on the later of the two, once per pair.
def _check_overlaps(config, full_config):
    name = config[CONF_ID].id
    eeprom = config[CONF_EEPROM_ID].id
    base = config[CONF_BASE_OFFSET]
    end = layout(config)[1]
    for other in full_config.get("fram_store") or []:
        if other[CONF_ID].id == name:
            return
        if CONF_EEPROM_ID not in other or other[CONF_EEPROM_ID].id != eeprom:
            continue
        if base < layout(other)[1] and other[CONF_BASE_OFFSET] < end:
            raise cv.Invalid(
                f"{name} ({_span(config)}) overlaps {other[CONF_ID].id} ({_span(other)}) on {eeprom}",
                path=[CONF_BASE_OFFSET],
            )


# The store rewrites a record on every save, which an EEPROM's write cycles and endurance cannot
# take, and it is laid out for the size it names.
def _final_validate(config):
    if CONF_EEPROM_ID not in config:
        if not CORE.is_host:
            _LOGGER.warning(
                "fram_store %s names no eeprom_id: its regions live in RAM and are lost at every reboot",
                config[CONF_ID].id,
            )
        return config
    full_config = fv.full_config.get()
    _check_overlaps(config, full_config)
    part = _part(full_config, config[CONF_EEPROM_ID])
    if part is None:
        return config
    name = config[CONF_EEPROM_ID].id
    if part.get(CONF_TYPE) != "fram":
        raise cv.Invalid(
            f"{name} is not declared type: fram; the store rewrites its records far more often "
            "than an EEPROM endures",
            path=[CONF_EEPROM_ID],
        )
    part_size = getattr(part.get(CONF_SIZE), "enum_value", None)
    if part_size is not None and ONE_BYTE_REACH < part_size <= 2048:
        raise cv.Invalid(
            f"{name} is a {part[CONF_SIZE]} part, of which i2c_eeprom reaches only the first "
            f"{ONE_BYTE_REACH} bytes; the store needs a part it can address whole",
            path=[CONF_EEPROM_ID],
        )
    if part_size is not None and part_size != config[CONF_SIZE]:
        raise cv.Invalid(
            f"size is {config[CONF_SIZE]} bytes, but {name} holds {part_size}",
            path=[CONF_SIZE],
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    cg.add_define("USE_FRAM_STORE")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_EEPROM_ID in config:
        cg.add_define("USE_FRAM_STORE_I2C")
        eeprom = await cg.get_variable(config[CONF_EEPROM_ID])
        backend = cg.new_Pvariable(config[CONF_I2C_BACKEND_ID], eeprom)
    else:
        backend = cg.new_Pvariable(config[CONF_RAM_BACKEND_ID], config[CONF_SIZE])

    offsets, end, layout_hash = layout(config)
    cg.add(var.set_backend(backend))
    cg.add(var.set_expected_size(config[CONF_SIZE]))
    cg.add(var.set_base_offset(config[CONF_BASE_OFFSET]))
    cg.add(var.set_end(end))
    cg.add(var.set_layout_hash(layout_hash))

    for region, offset in zip(config[CONF_REGIONS], offsets):
        if region[CONF_TYPE] == TYPE_SLOT:
            obj = cg.new_Pvariable(region[CONF_ID], var, offset, region[CONF_SIZE])
        else:
            obj = cg.new_Pvariable(
                region[CONF_ID],
                var,
                offset,
                region[CONF_RECORD_SIZE],
                region[CONF_CAPACITY],
            )
        cg.add(var.add_region(obj))
