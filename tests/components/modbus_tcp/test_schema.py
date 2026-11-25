"""The component's schema: its defaults, every refusal's message, and the sockets it declares."""

from pathlib import Path
import unittest

from esphome import loader
import esphome.config_validation as cv
from esphome.const import KEY_CORE, KEY_TARGET_PLATFORM, PLATFORM_HOST
from esphome.core import CORE

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import modbus_tcp, socket  # noqa: E402


def setUpModule():
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST


def validate(**options):
    return modbus_tcp.CONFIG_SCHEMA({"modbus_server_id": "modbus_server1", **options})


class Schema(unittest.TestCase):
    def setUp(self):
        for key in (
            socket.KEY_SOCKET_CONSUMERS_TCP,
            socket.KEY_SOCKET_CONSUMERS_TCP_LISTEN,
        ):
            CORE.data.pop(key, None)

    def assertRefused(self, message, **options):
        with self.assertRaises(cv.Invalid) as raised:
            validate(**options)
        self.assertIn(message, str(raised.exception))

    def test_it_is_off_on_502_as_unit_1_by_default(self):
        config = validate()
        self.assertEqual(
            {
                key: config[key]
                for key in (
                    "enabled",
                    "port",
                    "unit_id",
                    "max_clients",
                    "serve_wifi_ap",
                )
            },
            {
                "enabled": False,
                "port": 502,
                "unit_id": 1,
                "max_clients": 2,
                "serve_wifi_ap": False,
            },
        )
        self.assertEqual(config["idle_timeout"].total_milliseconds, 60000)

    def test_it_needs_a_server(self):
        with self.assertRaises(cv.Invalid) as raised:
            modbus_tcp.CONFIG_SCHEMA({})
        self.assertIn("required key not provided", str(raised.exception))

    # 0 and 255 always reach the device, so neither can be its own unit.
    def test_it_refuses_the_units_that_always_reach_it(self):
        for unit_id in (0, 255):
            with self.subTest(unit_id=unit_id):
                self.assertRefused(
                    f"unit {unit_id} always reaches the device; give its own unit id, 1-247",
                    unit_id=unit_id,
                )

    def test_it_refuses_a_unit_id_past_247(self):
        self.assertRefused("value must be at most 247", unit_id=248)

    def test_it_refuses_a_client_count_outside_1_to_8(self):
        self.assertRefused("value must be at least 1", max_clients=0)
        self.assertRefused("value must be at most 8", max_clients=9)

    def test_it_refuses_port_0(self):
        self.assertRefused("value must be at least 1", port=0)

    # One more than the clients: a connection past the limit is accepted to be closed.
    def test_it_declares_a_socket_per_client_one_to_refuse_and_the_listener(self):
        validate(max_clients=3)
        counts = socket.get_socket_counts()
        self.assertEqual(CORE.data[socket.KEY_SOCKET_CONSUMERS_TCP], {"modbus_tcp": 4})
        self.assertEqual(
            CORE.data[socket.KEY_SOCKET_CONSUMERS_TCP_LISTEN], {"modbus_tcp": 1}
        )
        self.assertEqual((counts.tcp, counts.tcp_listen), (4, 1))


if __name__ == "__main__":
    unittest.main()
