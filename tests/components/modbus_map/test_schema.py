"""The component's schema and the ranges it derives from the server, with every refusal's message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader
from esphome.const import KEY_CORE, KEY_TARGET_PLATFORM, PLATFORM_HOST
from esphome.core import CORE, Lambda

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import modbus_map, modbus_server  # noqa: E402

SERVER_ID = "modbus_server1"


def setUpModule():
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST


def bit(address, writable=False):
    entry = {"address": address, "read_lambda": Lambda("return true;")}
    if writable:
        entry["write_lambda"] = Lambda("return true;")
    return entry


def register(address, value_type="S_WORD", writable=False):
    entry = {
        "address": address,
        "value_type": value_type,
        "read_lambda": Lambda("return 0;"),
    }
    if writable:
        entry["write_lambda"] = Lambda("return true;")
    return entry


def server(bits=(), registers=(), courtesy=None, server_id=SERVER_ID):
    """A modbus_server: entry as upstream's own schema leaves it."""
    config = {"id": server_id}
    if bits:
        config["bits"] = list(bits)
    if registers:
        config["registers"] = list(registers)
    if courtesy is not None:
        config["courtesy_response"] = courtesy
    return modbus_server.CONFIG_SCHEMA(config)


# The JXD relay board's server, as features/modbus-server.yaml configures it.
JXD_SERVER = {
    "bits": [bit(a, writable=True) for a in range(0x0000, 0x0006)]
    + [bit(a) for a in range(0x0010, 0x0016)],
    "registers": [register(a) for a in range(0x0000, 0x0010)],
    "courtesy": {"enabled": True},
}

JXD_MAP = {
    "modbus_server_id": SERVER_ID,
    "bits": [
        {"address": 0x0000, "name": "Relays"},
        {"address": 0x0010, "name": "Inputs"},
    ],
    "registers": [
        {
            "address": 0x0000,
            "name": "Temperature slots",
            "scale": 0.1,
            "unit": "°C",
            "no_value": 0x8000,
        }
    ],
}


def validate(config):
    return modbus_map.CONFIG_SCHEMA(config)


def derive(config, *servers):
    return modbus_map.derive_map(validate(config), {"modbus_server": list(servers)})


def final_validate(config, full_config):
    token = fv.full_config.set(full_config)
    try:
        return modbus_map.FINAL_VALIDATE_SCHEMA(validate(config))
    finally:
        fv.full_config.reset(token)


def named(table, *entries):
    return {
        "modbus_server_id": SERVER_ID,
        table: [{"address": a, "name": n} for a, n in entries],
    }


class Jxd(unittest.TestCase):
    def test_the_relay_board_derives_three_ranges_and_the_courtesy_response(self):
        derived = derive(JXD_MAP, server(**JXD_SERVER))
        self.assertEqual(
            derived,
            {
                "bits": [
                    {
                        "address": 0x0000,
                        "last_address": 0x0005,
                        "count": 6,
                        "writable": True,
                        "name": "Relays",
                    },
                    {
                        "address": 0x0010,
                        "last_address": 0x0015,
                        "count": 6,
                        "writable": False,
                        "name": "Inputs",
                    },
                ],
                "registers": [
                    {
                        "address": 0x0000,
                        "last_address": 0x000F,
                        "count": 16,
                        "writable": False,
                        "name": "Temperature slots",
                        "value_type": "S_WORD",
                        "scale": 0.1,
                        "unit": "°C",
                        "no_value": 0x8000,
                    }
                ],
                "courtesy_response": {"last_address": 0xFFFF, "value": 0},
            },
        )

    def test_final_validation_passes_it_and_keeps_the_config(self):
        config = final_validate(JXD_MAP, {"modbus_server": [server(**JXD_SERVER)]})
        self.assertEqual(config["modbus_server_id"].id, SERVER_ID)

    def test_the_server_is_found_by_id_among_several(self):
        other = server(bits=[bit(0x0100)], server_id="other")
        derived = derive(JXD_MAP, other, server(**JXD_SERVER))
        self.assertEqual([r["name"] for r in derived["bits"]], ["Relays", "Inputs"])

    def test_the_order_of_the_server_entries_does_not_matter(self):
        shuffled = {**JXD_SERVER, "bits": list(reversed(JXD_SERVER["bits"]))}
        self.assertEqual(
            derive(JXD_MAP, server(**shuffled)),
            derive(JXD_MAP, server(**JXD_SERVER)),
        )


class Splitting(unittest.TestCase):
    def test_a_named_address_splits_a_run(self):
        derived = derive(
            named("bits", (0, "Pumps"), (2, "Valves")),
            server(bits=[bit(a, writable=True) for a in range(4)]),
        )
        self.assertEqual(
            [(r["address"], r["last_address"], r["count"]) for r in derived["bits"]],
            [(0, 1, 2), (2, 3, 2)],
        )

    def test_a_gap_splits_a_run(self):
        derived = derive(
            named("bits", (0, "A"), (3, "B")),
            server(bits=[bit(0), bit(1), bit(3), bit(4)]),
        )
        self.assertEqual(
            [(r["address"], r["last_address"]) for r in derived["bits"]],
            [(0, 1), (3, 4)],
        )

    def test_a_change_of_writability_splits_a_run(self):
        bits = [bit(0, writable=True), bit(1, writable=True), bit(2), bit(3)]
        derived = derive(named("bits", (0, "Out"), (2, "In")), server(bits=bits))
        self.assertEqual(
            [(r["address"], r["count"], r["writable"]) for r in derived["bits"]],
            [(0, 2, True), (2, 2, False)],
        )

    def test_writability_splits_registers_too(self):
        registers = [register(0, writable=True), register(1)]
        derived = derive(
            named("registers", (0, "Setpoint"), (1, "Reading")),
            server(registers=registers),
        )
        self.assertEqual([r["writable"] for r in derived["registers"]], [True, False])

    def test_a_change_of_value_type_splits_a_run(self):
        registers = [register(0), register(1), register(2, "U_DWORD")]
        derived = derive(
            named("registers", (0, "Words"), (2, "Counter")),
            server(registers=registers),
        )
        self.assertEqual(
            [
                (r["address"], r["last_address"], r["count"], r["value_type"])
                for r in derived["registers"]
            ],
            [(0, 1, 2, "S_WORD"), (2, 3, 1, "U_DWORD")],
        )

    def test_a_dword_run_ends_on_the_last_word_of_its_last_value(self):
        registers = [register(a, "FP32_R") for a in (0x0020, 0x0022, 0x0024)]
        derived = derive(
            named("registers", (0x0020, "Power")), server(registers=registers)
        )
        self.assertEqual(
            derived["registers"][0],
            {
                "address": 0x0020,
                "last_address": 0x0025,
                "count": 3,
                "writable": False,
                "name": "Power",
                "value_type": "FP32_R",
            },
        )

    def test_a_qword_is_four_words(self):
        registers = [register(0, "U_QWORD"), register(4, "U_QWORD")]
        derived = derive(named("registers", (0, "Energy")), server(registers=registers))
        self.assertEqual(derived["registers"][0]["last_address"], 7)
        self.assertEqual(derived["registers"][0]["count"], 2)

    def test_a_run_can_end_at_the_top_of_the_address_space(self):
        derived = derive(
            named("registers", (0xFFFE, "Last")),
            server(registers=[register(0xFFFE, "U_DWORD")]),
        )
        self.assertEqual(derived["registers"][0]["last_address"], 0xFFFF)

    def test_what_is_not_given_is_left_out(self):
        derived = derive(
            named("registers", (0, "Raw")), server(registers=[register(0)])
        )
        for key in ("scale", "unit", "no_value"):
            with self.subTest(key=key):
                self.assertNotIn(key, derived["registers"][0])

    def test_a_table_the_server_does_not_serve_is_empty(self):
        derived = derive(named("bits", (0, "Relay")), server(bits=[bit(0)]))
        self.assertEqual(derived["registers"], [])


class CourtesyResponse(unittest.TestCase):
    def test_it_is_left_out_unless_enabled(self):
        for courtesy in (None, {"enabled": False}):
            with self.subTest(courtesy=courtesy):
                derived = derive(
                    named("bits", (0, "Relay")),
                    server(bits=[bit(0)], courtesy=courtesy),
                )
                self.assertIsNone(derived["courtesy_response"])

    def test_it_carries_the_servers_own_values(self):
        courtesy = {
            "enabled": True,
            "register_last_address": 0x00FF,
            "register_value": 0xFFFF,
        }
        derived = derive(
            named("bits", (0, "Relay")), server(bits=[bit(0)], courtesy=courtesy)
        )
        self.assertEqual(
            derived["courtesy_response"], {"last_address": 0x00FF, "value": 0xFFFF}
        )


class Refusals(unittest.TestCase):
    """What the map and the server disagree on, with the fix in the message."""

    def assert_refused(self, config, servers, message, path):
        with self.assertRaises(cv.Invalid) as raised:
            derive(config, *servers)
        self.assertEqual(str(raised.exception.msg), message)
        self.assertEqual(raised.exception.path, path)

    def test_a_served_run_with_no_name(self):
        config = {**JXD_MAP, "bits": [{"address": 0x0000, "name": "Relays"}]}
        self.assert_refused(
            config,
            [server(**JXD_SERVER)],
            "modbus_server1 serves bits 0x0010-0x0015 that modbus_map does not name: "
            "add an entry under bits with address: 0x0010",
            ["bits"],
        )

    def test_an_unnamed_run_of_one_value(self):
        self.assert_refused(
            named("registers", (0, "Words")),
            [server(registers=[register(0), register(1, "U_DWORD")])],
            "modbus_server1 serves registers 0x0001-0x0002 that modbus_map does not "
            "name: add an entry under registers with address: 0x0001",
            ["registers"],
        )
        self.assert_refused(
            named("bits", (0, "Out")),
            [server(bits=[bit(0, writable=True), bit(1)])],
            "modbus_server1 serves bit 0x0001 that modbus_map does not name: "
            "add an entry under bits with address: 0x0001",
            ["bits"],
        )

    def test_the_values_before_the_first_named_address(self):
        # Naming the second half of a run leaves the first half to name.
        self.assert_refused(
            named("bits", (2, "Second half")),
            [server(bits=[bit(a) for a in range(4)])],
            "modbus_server1 serves bits 0x0000-0x0001 that modbus_map does not name: "
            "add an entry under bits with address: 0x0000",
            ["bits"],
        )

    def test_a_whole_table_left_out(self):
        config = {key: value for key, value in JXD_MAP.items() if key != "registers"}
        self.assert_refused(
            config,
            [server(**JXD_SERVER)],
            "modbus_server1 serves registers 0x0000-0x000F that modbus_map does not "
            "name: add an entry under registers with address: 0x0000",
            [],
        )

    def test_a_named_address_the_server_does_not_serve(self):
        config = named("bits", (0, "Relays"), (6, "More relays"))
        self.assert_refused(
            config,
            [server(bits=[bit(a) for a in range(6)])],
            "modbus_server1 serves no bit at 0x0006: remove the entry or fix its address",
            ["bits", 1, "address"],
        )

    def test_a_named_register_the_server_does_not_serve(self):
        # Just past a value's last word is outside it, not inside.
        for label, registers in (
            ("after a dword", [register(0, "U_DWORD")]),
            ("in a gap", [register(0), register(3)]),
        ):
            with self.subTest(label):
                self.assert_refused(
                    named("registers", (0, "First"), (2, "Missing")),
                    [server(registers=registers)],
                    "modbus_server1 serves no register at 0x0002: "
                    "remove the entry or fix its address",
                    ["registers", 1, "address"],
                )

    def test_a_named_register_inside_a_value(self):
        self.assert_refused(
            named("registers", (0, "Counter"), (1, "Half")),
            [server(registers=[register(0, "U_DWORD")])],
            "0x0001 is inside the U_DWORD modbus_server1 serves at 0x0000: "
            "name 0x0000 instead",
            ["registers", 1, "address"],
        )

    def test_no_value_on_a_value_wider_than_a_register(self):
        config = {
            "modbus_server_id": SERVER_ID,
            "registers": [{"address": 0, "name": "Counter", "no_value": 0xFFFF}],
        }
        self.assert_refused(
            config,
            [server(registers=[register(0, "S_DWORD")])],
            "no_value is one register, but the S_DWORD at 0x0000 spans 2: "
            "remove no_value",
            ["registers", 0, "no_value"],
        )

    def test_no_modbus_server_with_that_id(self):
        for servers in ([], [server(bits=[bit(0)], server_id="other")]):
            with self.subTest(servers=len(servers)):
                self.assert_refused(
                    named("bits", (0, "Relay")),
                    servers,
                    "There is no modbus_server with id modbus_server1",
                    ["modbus_server_id"],
                )

    def test_a_config_with_no_modbus_server_section(self):
        with self.assertRaises(cv.Invalid) as raised:
            modbus_map.derive_map(validate(named("bits", (0, "Relay"))), {})
        self.assertEqual(
            str(raised.exception.msg),
            "There is no modbus_server with id modbus_server1",
        )
        self.assertEqual(raised.exception.path, ["modbus_server_id"])

    def test_final_validation_refuses_the_same(self):
        config = {**JXD_MAP, "bits": [{"address": 0x0000, "name": "Relays"}]}
        with self.assertRaises(cv.Invalid) as raised:
            final_validate(config, {"modbus_server": [server(**JXD_SERVER)]})
        self.assertEqual(
            str(raised.exception.msg),
            "modbus_server1 serves bits 0x0010-0x0015 that modbus_map does not name: "
            "add an entry under bits with address: 0x0010",
        )
        self.assertEqual(raised.exception.path, ["bits"])


class Codegen(unittest.TestCase):
    """What to_code hands the C++ class: one call per derived range, optional fields only when given."""

    def tearDown(self):
        CORE.reset()
        setUpModule()

    def generate(self, config, *servers):
        CORE.reset()
        setUpModule()
        CORE.config = {"modbus_server": list(servers)}
        CORE.add_job(modbus_map.to_code, validate({**config, "id": "jxd_map"}))
        CORE.flush_tasks()
        # One line per statement, so a comparison does not hang on the struct's line breaks.
        return [" ".join(str(s).split()) for s in CORE.main_statements]

    def test_the_relay_board(self):
        self.assertEqual(
            self.generate(JXD_MAP, server(**JXD_SERVER))[1:],
            [
                "jxd_map->add_bits(modbus_map::BitRange{ .address = 0, .last_address = 5, "
                '.count = 6, .writable = true, .name = "Relays", });',
                "jxd_map->add_bits(modbus_map::BitRange{ .address = 16, .last_address = 21, "
                '.count = 6, .writable = false, .name = "Inputs", });',
                "jxd_map->add_registers(modbus_map::RegisterRange{ .address = 0, "
                ".last_address = 15, .count = 16, .writable = false, "
                '.name = "Temperature slots", .value_type = "S_WORD", .scale = 0.1f, '
                '.unit = "\\302\\260C", .no_value = 0x8000, });',
                "jxd_map->set_courtesy_response(0xFFFF, 0x00);",
            ],
        )

    def test_what_is_not_given_keeps_the_structs_default(self):
        statements = self.generate(
            named("registers", (0, "Raw")), server(registers=[register(0)])
        )
        self.assertEqual(
            statements[1:],
            [
                "jxd_map->add_registers(modbus_map::RegisterRange{ .address = 0, "
                '.last_address = 0, .count = 1, .writable = false, .name = "Raw", '
                '.value_type = "S_WORD", });'
            ],
        )

    def test_a_falsy_value_is_still_given(self):
        # A no_value of 0 is a word like any other: only a missing key keeps the -1 default.
        config = {
            "modbus_server_id": SERVER_ID,
            "registers": [{"address": 0, "name": "Level", "scale": 2, "no_value": 0}],
        }
        statements = self.generate(
            config, server(registers=[register(0, "U_WORD", writable=True)])
        )
        self.assertEqual(
            statements[1:],
            [
                "jxd_map->add_registers(modbus_map::RegisterRange{ .address = 0, "
                '.last_address = 0, .count = 1, .writable = true, .name = "Level", '
                '.value_type = "U_WORD", .scale = 2.0f, .no_value = 0x00, });'
            ],
        )

    def test_the_courtesy_response_carries_the_servers_values(self):
        courtesy = {
            "enabled": True,
            "register_last_address": 0x00FF,
            "register_value": 0xFFFF,
        }
        statements = self.generate(
            named("bits", (0, "Relay")), server(bits=[bit(0)], courtesy=courtesy)
        )
        self.assertEqual(
            statements[-1], "jxd_map->set_courtesy_response(0xFF, 0xFFFF);"
        )


class Schema(unittest.TestCase):
    def assert_refused(self, config, message, path):
        with self.assertRaises(cv.Invalid) as raised:
            validate(config)
        self.assertEqual(str(raised.exception.msg), message)
        self.assertEqual(raised.exception.path, path)

    def test_the_server_is_a_reference(self):
        config = validate(JXD_MAP)
        self.assertEqual(config["id"].type, "modbus_map::ModbusMap")
        self.assertTrue(config["id"].is_declaration)
        reference = config["modbus_server_id"]
        self.assertEqual(reference.type, "modbus_server::ModbusServer")
        self.assertFalse(reference.is_declaration)

    def test_it_needs_the_server_component(self):
        self.assertEqual(modbus_map.DEPENDENCIES, ["modbus_server"])

    def test_the_server_must_be_named(self):
        self.assert_refused(
            {"bits": [{"address": 0, "name": "Relay"}]},
            "required key not provided",
            ["modbus_server_id"],
        )

    def test_at_least_one_table(self):
        self.assert_refused(
            {"modbus_server_id": SERVER_ID},
            "Must contain at least one of bits, registers.",
            [],
        )

    def test_a_table_is_not_empty(self):
        for table in ("bits", "registers"):
            with self.subTest(table=table):
                self.assert_refused(
                    {"modbus_server_id": SERVER_ID, table: []},
                    "length of value must be at least 1",
                    [table],
                )

    def test_the_same_address_twice_in_one_table(self):
        for table in ("bits", "registers"):
            with self.subTest(table=table):
                self.assert_refused(
                    named(table, (0x0010, "A"), (0x0010, "B")),
                    f"0x0010 is named twice under {table}",
                    [table, 1, "address"],
                )

    def test_one_address_in_both_tables_is_fine(self):
        config = {
            "modbus_server_id": SERVER_ID,
            "bits": [{"address": 0, "name": "Relay"}],
            "registers": [{"address": 0, "name": "Reading"}],
        }
        self.assertEqual(len(validate(config)["registers"]), 1)

    def test_a_name_is_not_blank(self):
        for name in ("", "   "):
            with self.subTest(name=name):
                self.assert_refused(
                    named("bits", (0, name)), "must not be empty", ["bits", 0, "name"]
                )

    def test_a_name_and_a_unit_are_text(self):
        for key in ("name", "unit"):
            with self.subTest(key=key):
                self.assert_refused(
                    {
                        "modbus_server_id": SERVER_ID,
                        "registers": [{"address": 0, "name": "T", key: 5}],
                    },
                    "Must be string, got <class 'int'>. "
                    "did you forget putting quotes around the value?",
                    ["registers", 0, key],
                )

    def test_a_unit_is_not_blank(self):
        self.assert_refused(
            {
                "modbus_server_id": SERVER_ID,
                "registers": [{"address": 0, "name": "T", "unit": " "}],
            },
            "must not be empty",
            ["registers", 0, "unit"],
        )

    def test_an_address_is_sixteen_bits(self):
        for address, message in (
            (-1, "value must be at least 0"),
            (0x10000, "value must be at most 65535"),
            ("relay", "Expected integer, but cannot parse relay as an integer"),
        ):
            with self.subTest(address=address):
                self.assert_refused(
                    named("bits", (address, "Relay")), message, ["bits", 0, "address"]
                )

    def test_scale_is_positive(self):
        for scale in (0, -0.1):
            with self.subTest(scale=scale):
                self.assert_refused(
                    {
                        "modbus_server_id": SERVER_ID,
                        "registers": [{"address": 0, "name": "T", "scale": scale}],
                    },
                    "value must be higher than 0",
                    ["registers", 0, "scale"],
                )

    def test_no_value_is_one_register(self):
        for no_value, message in (
            (-1, "value must be at least 0"),
            (0x10000, "value must be at most 65535"),
        ):
            with self.subTest(no_value=no_value):
                self.assert_refused(
                    {
                        "modbus_server_id": SERVER_ID,
                        "registers": [
                            {"address": 0, "name": "T", "no_value": no_value}
                        ],
                    },
                    message,
                    ["registers", 0, "no_value"],
                )

    def test_a_bit_has_no_encoding(self):
        for key, value in (("scale", 0.1), ("unit", "°C"), ("no_value", 0)):
            with self.subTest(key=key):
                self.assert_refused(
                    {
                        "modbus_server_id": SERVER_ID,
                        "bits": [{"address": 0, "name": "Relay", key: value}],
                    },
                    "extra keys not allowed",
                    ["bits", 0, key],
                )

    def test_the_count_and_the_access_are_not_described(self):
        # They come from the server, so the map cannot get them wrong.
        for key in ("count", "writable", "value_type", "last_address"):
            with self.subTest(key=key):
                self.assert_refused(
                    {
                        "modbus_server_id": SERVER_ID,
                        "registers": [{"address": 0, "name": "T", key: 1}],
                    },
                    "extra keys not allowed",
                    ["registers", 0, key],
                )


if __name__ == "__main__":
    unittest.main()
