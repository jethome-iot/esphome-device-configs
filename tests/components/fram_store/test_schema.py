"""The component's YAML schema: where it places regions, what it refuses, and the shipped layout."""

import re
import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader
from esphome.const import KEY_CORE, KEY_TARGET_PLATFORM, PLATFORM_HOST
from esphome.core import CORE, ID
import yaml

REPO = Path(__file__).resolve().parents[3]
loader.install_meta_finder(REPO / "components")

from esphome.components import fram_store, i2c_eeprom  # noqa: E402


def setUpModule():
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST


def validate(config):
    return fram_store.CONFIG_SCHEMA(config)


def final_validate(config, full_config):
    token = fv.full_config.set(full_config)
    try:
        return fram_store.FINAL_VALIDATE_SCHEMA(validate(config))
    finally:
        fv.full_config.reset(token)


def part(name, **options):
    """A validated i2c_eeprom: entry, as the full config holds it."""
    config = i2c_eeprom.CONFIG_SCHEMA({"size": "64KB", "address": 0x52, **options})
    config["id"] = ID(name, is_declaration=True)
    return config


def slot(name, size):
    return {"id": name, "type": "slot", "size": size}


def ring(name, record_size, capacity):
    return {
        "id": name,
        "type": "ring",
        "record_size": record_size,
        "capacity": capacity,
    }


class Placement(unittest.TestCase):
    def test_regions_follow_the_superblock_four_byte_aligned(self):
        config = validate({"regions": [slot("a", 5), ring("b", 3, 3), slot("c", 8)]})
        offsets, end, _ = fram_store.layout(config)
        # 32 + 2 * (8 + 5) = 58, aligned to 60; 60 + 3 * (6 + 3) = 87, aligned to 88.
        self.assertEqual(offsets, [32, 60, 88])
        self.assertEqual(end, 88 + 2 * (8 + 8))

    def test_base_offset_moves_the_superblock_and_every_region(self):
        config = validate({"base_offset": 0x200, "regions": [slot("a", 8)]})
        offsets, _, _ = fram_store.layout(config)
        self.assertEqual(offsets, [0x200 + 32])

    def test_the_defaults_are_the_8_kib_part_from_address_0(self):
        config = validate({"regions": [slot("a", 8)]})
        self.assertEqual(config["size"], 8192)
        self.assertEqual(config["base_offset"], 0)
        self.assertNotIn("eeprom_id", config)

    def test_the_hash_follows_every_id_kind_size_and_the_base(self):
        base = {"size": 4096, "regions": [slot("a", 8), ring("b", 8, 4)]}
        variants = [
            {**base, "regions": [slot("renamed", 8), ring("b", 8, 4)]},
            {**base, "regions": [slot("a", 9), ring("b", 8, 4)]},
            {**base, "regions": [slot("a", 8), ring("b", 8, 5)]},
            {**base, "regions": [slot("a", 8), ring("b", 9, 4)]},
            {**base, "regions": [slot("a", 8), slot("b", 8)]},
            {**base, "regions": [slot("a", 8)]},
            {**base, "size": 8192},
            {**base, "base_offset": 0x100},
        ]
        _, _, reference = fram_store.layout(validate(base))
        self.assertEqual(reference, fram_store.layout(validate(dict(base)))[2])
        for variant in variants:
            with self.subTest(variant=variant):
                self.assertNotEqual(fram_store.layout(validate(variant))[2], reference)


class Refusals(unittest.TestCase):
    def test_a_layout_that_does_not_fit_is_refused(self):
        with self.assertRaisesRegex(
            cv.Invalid,
            r"The regions need 4136 bytes including the 32-byte superblock, "
            r"but the device has 4096 from base_offset 0x0000 up",
        ):
            validate({"size": 4096, "regions": [slot("a", 2044)]})

    def test_the_base_offset_counts_against_the_room(self):
        # Fits from 0, not from 0x200.
        validate({"size": 1024, "regions": [slot("a", 400)]})
        with self.assertRaisesRegex(
            cv.Invalid,
            r"The regions need 848 bytes including the 32-byte superblock, "
            r"but the device has 512 from base_offset 0x0200 up",
        ):
            validate({"size": 1024, "base_offset": 0x200, "regions": [slot("a", 400)]})

    def test_a_base_offset_past_the_device_is_refused(self):
        with self.assertRaisesRegex(
            cv.Invalid, r"but the device has 0 from base_offset 0x0800 up"
        ):
            validate({"size": 1024, "base_offset": 0x800, "regions": [slot("a", 4)]})

    def test_a_base_offset_off_the_alignment_is_refused(self):
        for value in (1, 0x1FE, 0x201):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "base_offset must be a multiple of 4"
                ),
            ):
                validate({"base_offset": value, "regions": [slot("a", 8)]})

    def test_a_base_offset_outside_an_address_is_refused(self):
        for value in (-4, 65536):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "value must be at (least|most)"),
            ):
                validate({"base_offset": value, "regions": [slot("a", 8)]})

    def test_a_store_needs_a_region(self):
        with self.assertRaisesRegex(cv.Invalid, "length of value must be at least 1"):
            validate({"regions": []})

    def test_region_sizes_have_bounds(self):
        for region, message in (
            (slot("a", 0), "value must be at least 1"),
            (slot("a", 4097), "value must be at most 4096"),
            (ring("a", 0, 4), "value must be at least 1"),
            (ring("a", 1025, 4), "value must be at most 1024"),
            (ring("a", 8, 0), "value must be at least 1"),
            (ring("a", 8, 4097), "value must be at most 4096"),
        ):
            with (
                self.subTest(region=region),
                self.assertRaisesRegex(cv.Invalid, message),
            ):
                validate({"size": 65536, "regions": [region]})

    def test_an_unknown_region_type_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Unknown value 'log'"):
            validate({"regions": [{"id": "a", "type": "log", "size": 8}]})


class ThePart(unittest.TestCase):
    def test_a_fram_part_of_the_same_size_is_taken(self):
        config = {"eeprom_id": "fram_cpu", "regions": [slot("a", 8)]}
        final_validate(config, {"i2c_eeprom": [part("fram_cpu", type="fram")]})

    def test_an_eeprom_is_refused(self):
        config = {"eeprom_id": "eeprom_cpu", "regions": [slot("a", 8)]}
        with self.assertRaisesRegex(
            cv.Invalid,
            "eeprom_cpu is not declared type: fram; the store rewrites its records far "
            "more often than an EEPROM endures",
        ):
            final_validate(config, {"i2c_eeprom": [part("eeprom_cpu")]})

    def test_a_size_the_part_does_not_have_is_refused(self):
        config = {"eeprom_id": "fram_cpu", "size": 4096, "regions": [slot("a", 8)]}
        with self.assertRaisesRegex(
            cv.Invalid, "size is 4096 bytes, but fram_cpu holds 8192"
        ):
            final_validate(config, {"i2c_eeprom": [part("fram_cpu", type="fram")]})

    def test_a_store_in_ram_names_no_part(self):
        final_validate({"regions": [slot("a", 8)]}, {})


class Constants(unittest.TestCase):
    """The placement here and the format in C++ have to agree byte for byte."""

    def constant(self, header, name):
        text = (REPO / "components" / "fram_store" / header).read_text()
        match = re.search(rf"\b{name} = (\d+);", text)
        self.assertIsNotNone(match, f"{name} not found in {header}")
        return int(match.group(1))

    def test_the_sizes_match_the_headers(self):
        self.assertEqual(
            fram_store.SUPERBLOCK_SIZE, self.constant("fram_store.h", "SUPERBLOCK_SIZE")
        )
        self.assertEqual(
            fram_store.SLOT_HEADER_SIZE, self.constant("fram_region.h", "HEADER_SIZE")
        )
        text = (REPO / "components" / "fram_store" / "fram_region.h").read_text()
        ring_part = text[text.index("class FramRing") :]
        self.assertEqual(
            fram_store.RING_HEADER_SIZE,
            int(re.search(r"\bHEADER_SIZE = (\d+);", ring_part).group(1)),
        )


class ShippedLayout(unittest.TestCase):
    """features/fram.yaml as devices carry it. A change here empties every meter's store."""

    @classmethod
    def setUpClass(cls):
        package = REPO / "devices" / "JXD" / "packages" / "features" / "fram.yaml"
        cls.package = yaml.safe_load(package.read_text())
        (store,) = cls.package["fram_store"]
        cls.config = validate(store)

    def test_the_part_is_the_cpu_boards_fram(self):
        (fram,) = self.package["i2c_eeprom"]
        self.assertEqual(fram["id"], "fram_cpu")
        self.assertEqual(fram["address"], 0x52)
        self.assertEqual(fram["type"], "fram")
        self.assertEqual(i2c_eeprom.EEPROM_SIZES[fram["size"]], self.config["size"])

    def test_the_factory_stations_bytes_are_left_alone(self):
        self.assertEqual(self.config["base_offset"], 0x200)

    def test_the_layout_is_v1(self):
        offsets, end, layout_hash = fram_store.layout(self.config)
        names = [str(region["id"]) for region in self.config["regions"]]
        self.assertEqual(
            dict(zip(names, offsets)),
            {
                "fram_meter_board": 0x0220,
                "fram_odometers": 0x02B0,
                **{f"fram_counter_{n:02d}": 0x04C0 + n * 112 for n in range(16)},
                "fram_history": 0x0BC0,
            },
        )
        self.assertEqual(end, 0x1340)
        self.assertEqual(8192 - end, 3264)
        self.assertEqual(
            layout_hash,
            0xF62347B9,
            "the shipped FRAM layout changed: every device that boots this firmware loses "
            "its stored energy",
        )


if __name__ == "__main__":
    unittest.main()
