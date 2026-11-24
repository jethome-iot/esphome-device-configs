#!/usr/bin/env python3
"""Minimal Modbus client for probing a JXD device over RS485 (RTU) or the network (TCP).

Speaks just enough Modbus to exercise the register map that
`devices/JXD/packages/features/modbus-server.yaml` exposes. Needs only pyserial, which ESPHome
already pulls in, so `.venv/bin/python scripts/modbus_probe.py` works as is.

  .venv/bin/python scripts/modbus_probe.py --port /dev/serial/by-id/<adapter> probe
  .venv/bin/python scripts/modbus_probe.py --port /dev/serial/by-id/<adapter> read-coils 0x0000 6
  .venv/bin/python scripts/modbus_probe.py --tcp 192.168.1.50 write-coil 0x0000 1
  .venv/bin/python scripts/modbus_probe.py --tcp 127.0.0.1:5020 probe     # QEMU
"""

from __future__ import annotations

import argparse
import socket
import sys
import time

import serial

# JXD map, see devices/JXD/packages/features/modbus-server.yaml. Coils and discrete inputs share
# one bit address space in modbus_server, so the two blocks are disjoint.
COILS_BASE = 0x0000  # relays, FC 0x01/0x05/0x0F
DISCRETE_BASE = 0x0010  # digital inputs, FC 0x02
TEMP_BASE = 0x0000  # temperature slots, FC 0x03/0x04, signed 0.1 °C
TEMP_COUNT = 16
TEMP_NONE = 0x8000  # slot has no reading

EXCEPTIONS = {
    0x01: "ILLEGAL_FUNCTION",
    0x02: "ILLEGAL_DATA_ADDRESS",
    0x03: "ILLEGAL_DATA_VALUE",
    0x04: "SERVER_DEVICE_FAILURE",
    0x05: "ACKNOWLEDGE",
    0x06: "SERVER_DEVICE_BUSY",
    0x08: "MEMORY_PARITY_ERROR",
    0x0A: "GATEWAY_PATH_UNAVAILABLE",
    0x0B: "GATEWAY_TARGET_NO_RESPONSE",
}


class ModbusError(Exception):
    pass


class ModbusException(ModbusError):
    def __init__(self, function: int, code: int) -> None:
        name = EXCEPTIONS.get(code, "UNKNOWN")
        super().__init__(f"FC 0x{function:02X} -> exception 0x{code:02X} {name}")
        self.code = code


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


class Modbus:
    """The requests the probe makes; a transport supplies request()."""

    unit: int

    def describe(self) -> str:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError

    def request(self, function: int, payload: bytes) -> bytes:
        """The response PDU after the function code; raises ModbusException on an exception."""
        raise NotImplementedError

    def _read_bits(self, function: int, address: int, count: int) -> list[bool]:
        body = self.request(
            function, address.to_bytes(2, "big") + count.to_bytes(2, "big")
        )
        packed = body[1:]
        return [bool(packed[i // 8] & (1 << (i % 8))) for i in range(count)]

    def _read_registers(self, function: int, address: int, count: int) -> list[int]:
        body = self.request(
            function, address.to_bytes(2, "big") + count.to_bytes(2, "big")
        )
        raw = body[1:]
        return [int.from_bytes(raw[i : i + 2], "big") for i in range(0, len(raw), 2)]

    def read_coils(self, address: int, count: int) -> list[bool]:
        return self._read_bits(0x01, address, count)

    def read_discrete_inputs(self, address: int, count: int) -> list[bool]:
        return self._read_bits(0x02, address, count)

    def read_holding_registers(self, address: int, count: int) -> list[int]:
        return self._read_registers(0x03, address, count)

    def read_input_registers(self, address: int, count: int) -> list[int]:
        return self._read_registers(0x04, address, count)

    def write_coil(self, address: int, value: bool) -> None:
        self.request(
            0x05, address.to_bytes(2, "big") + (b"\xff\x00" if value else b"\x00\x00")
        )

    def write_coils(self, address: int, values: list[bool]) -> None:
        packed = bytearray((len(values) + 7) // 8)
        for i, value in enumerate(values):
            if value:
                packed[i // 8] |= 1 << (i % 8)
        payload = (
            address.to_bytes(2, "big")
            + len(values).to_bytes(2, "big")
            + bytes([len(packed)])
            + bytes(packed)
        )
        self.request(0x0F, payload)


class ModbusRTU(Modbus):
    def __init__(
        self,
        port: str,
        baud: int,
        unit: int,
        timeout: float,
        parity: str = "N",
        stop_bits: int = 1,
    ) -> None:
        self.unit = unit
        self.verbose = False
        self.framing = f"8{parity}{stop_bits}"
        self.ser = serial.Serial(
            port, baud, bytesize=8, parity=parity, stopbits=stop_bits, timeout=timeout
        )
        # Silence between frames: 3.5 characters, floored at the 1.75 ms the spec
        # fixes for baud rates above 19200.
        char_bits = 9 + (parity != "N") + stop_bits
        self.frame_gap = max(3.5 * char_bits / baud, 0.00175)

    def describe(self) -> str:
        return f"unit 0x{self.unit:02X} on {self.ser.port} @ {self.ser.baudrate} {self.framing}"

    def close(self) -> None:
        self.ser.close()

    def _read(self, count: int, what: str) -> bytes:
        data = self.ser.read(count)
        if len(data) < count:
            raise ModbusError(
                f"timeout reading {what}: expected {count} bytes, got {len(data)}"
            )
        return data

    def request(self, function: int, payload: bytes) -> bytes:
        frame = bytes([self.unit, function]) + payload
        frame += crc16(frame).to_bytes(2, "little")

        time.sleep(self.frame_gap)
        self.ser.reset_input_buffer()
        if self.verbose:
            print(f"  -> {frame.hex(' ')}", file=sys.stderr)
        self.ser.write(frame)
        self.ser.flush()

        head = self._read(2, "response header")
        addr, func = head[0], head[1]

        if func & 0x80:
            body = self._read(3, "exception response")
            self._check(head + body)
            raise ModbusException(function, body[0])

        if function in (0x01, 0x02, 0x03, 0x04):
            count = self._read(1, "byte count")
            body = count + self._read(count[0] + 2, "response body")
        else:  # 0x05, 0x06, 0x0F, 0x10 all echo a fixed 4-byte body
            body = self._read(6, "response body")

        response = head + body
        if self.verbose:
            print(f"  <- {response.hex(' ')}", file=sys.stderr)
        self._check(response)
        if addr != self.unit:
            raise ModbusError(f"response from unit {addr}, expected {self.unit}")
        if func != function:
            raise ModbusError(f"response FC 0x{func:02X}, expected 0x{function:02X}")
        return body[:-2]

    @staticmethod
    def _check(frame: bytes) -> None:
        if crc16(frame) != 0:
            raise ModbusError(f"CRC mismatch in {frame.hex(' ')}")


class ModbusTCP(Modbus):
    def __init__(self, host: str, port: int, unit: int, timeout: float) -> None:
        self.unit = unit
        self.verbose = False
        self.address = f"{host}:{port}"
        self.transaction = 0
        self.sock = socket.create_connection((host, port), timeout=timeout)

    def describe(self) -> str:
        return f"unit 0x{self.unit:02X} at {self.address} (Modbus TCP)"

    def close(self) -> None:
        self.sock.close()

    def _read(self, count: int, what: str) -> bytes:
        data = b""
        try:
            while len(data) < count:
                chunk = self.sock.recv(count - len(data))
                if not chunk:
                    raise ModbusError(
                        f"connection closed reading {what} after {len(data)} bytes"
                    )
                data += chunk
        except TimeoutError:
            raise ModbusError(
                f"timeout reading {what}: expected {count} bytes, got {len(data)}"
            ) from None
        except OSError as err:
            raise ModbusError(f"connection lost reading {what}: {err}") from None
        return data

    def request(self, function: int, payload: bytes) -> bytes:
        self.transaction = (self.transaction + 1) & 0xFFFF
        pdu = bytes([function]) + payload
        adu = (
            self.transaction.to_bytes(2, "big")
            + b"\x00\x00"
            + (len(pdu) + 1).to_bytes(2, "big")
            + bytes([self.unit])
            + pdu
        )
        if self.verbose:
            print(f"  -> {adu.hex(' ')}", file=sys.stderr)
        try:
            self.sock.sendall(adu)
        except OSError as err:
            raise ModbusError(f"connection lost sending the request: {err}") from None

        head = self._read(7, "MBAP header")
        transaction = int.from_bytes(head[0:2], "big")
        protocol = int.from_bytes(head[2:4], "big")
        length = int.from_bytes(head[4:6], "big")
        if protocol != 0 or length < 3:
            raise ModbusError(f"malformed MBAP header {head.hex(' ')}")
        body = self._read(length - 1, "response PDU")
        if self.verbose:
            print(f"  <- {(head + body).hex(' ')}", file=sys.stderr)
        if transaction != self.transaction:
            raise ModbusError(
                f"response to transaction {transaction}, expected {self.transaction}"
            )
        if head[6] != self.unit:
            raise ModbusError(f"response from unit {head[6]}, expected {self.unit}")
        func = body[0]
        if func & 0x80:
            raise ModbusException(function, body[1])
        if func != function:
            raise ModbusError(f"response FC 0x{func:02X}, expected 0x{function:02X}")
        return body[1:]


def fmt_bits(bits: list[bool]) -> str:
    return " ".join("1" if b else "0" for b in bits)


def fmt_temps(registers: list[int]) -> str:
    def one(raw: int) -> str:
        if raw == TEMP_NONE:
            return "--.-"
        signed = raw - 0x10000 if raw & 0x8000 else raw
        return f"{signed / 10:.1f}"

    return " ".join(one(raw) for raw in registers)


def probe(bus: Modbus) -> int:
    """Walk the whole documented map, reporting each step independently."""
    failures = 0

    def step(label: str, fn, exception: int | None = None):
        nonlocal failures
        try:
            result = fn()
        except ModbusException as err:
            if err.code == exception:
                print(f"{label:<44} exception 0x{err.code:02X}, as expected")
                return
            failures += 1
            print(f"{label:<44} FAIL: {err}")
            return
        except ModbusError as err:
            failures += 1
            print(f"{label:<44} FAIL: {err}")
            return
        if exception is not None:
            failures += 1
            print(f"{label:<44} FAIL: {result}, expected exception 0x{exception:02X}")
            return
        print(f"{label:<44} {result}")

    print(bus.describe() + "\n")

    step(
        f"relays           FC 0x01 @ 0x{COILS_BASE:04X} x6",
        lambda: fmt_bits(bus.read_coils(COILS_BASE, 6)),
    )
    step(
        f"digital inputs   FC 0x02 @ 0x{DISCRETE_BASE:04X} x6",
        lambda: fmt_bits(bus.read_discrete_inputs(DISCRETE_BASE, 6)),
    )
    step(
        f"temperatures     FC 0x03 @ 0x{TEMP_BASE:04X} x{TEMP_COUNT}",
        lambda: fmt_temps(bus.read_holding_registers(TEMP_BASE, TEMP_COUNT)),
    )
    step(
        f"temperatures     FC 0x04 @ 0x{TEMP_BASE:04X} x{TEMP_COUNT}",
        lambda: fmt_temps(bus.read_input_registers(TEMP_BASE, TEMP_COUNT)),
    )
    step(
        "courtesy regs    FC 0x03 @ 0x0100 x2",
        lambda: bus.read_holding_registers(0x0100, 2),
    )
    # A bit outside the map answers exception 0x02, unlike a register.
    step(
        "unmapped bit     FC 0x01 @ 0x0100",
        lambda: fmt_bits(bus.read_coils(0x0100, 1)),
        exception=0x02,
    )
    return failures


def toggle(bus: Modbus, index: int) -> int:
    """Flip one relay and read it back, then restore it."""
    address = COILS_BASE + index
    before = bus.read_coils(address, 1)[0]
    print(f"relay {index + 1} @ 0x{address:04X}: {int(before)} (initial)")

    bus.write_coil(address, not before)
    time.sleep(0.2)
    after = bus.read_coils(address, 1)[0]
    print(f"relay {index + 1} @ 0x{address:04X}: {int(after)} (after write)")

    bus.write_coil(address, before)
    time.sleep(0.2)
    restored = bus.read_coils(address, 1)[0]
    print(f"relay {index + 1} @ 0x{address:04X}: {int(restored)} (restored)")

    if after == before:
        print("FAIL: coil did not change")
        return 1
    if restored != before:
        print("FAIL: coil was not restored")
        return 1
    print("OK")
    return 0


def auto_int(value: str) -> int:
    return int(value, 0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial port for Modbus RTU")
    parser.add_argument(
        "--tcp", metavar="HOST[:PORT]", help="Modbus TCP server instead (port 502)"
    )
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--parity", choices=["N", "E", "O"], default="N")
    parser.add_argument("--stop-bits", type=int, choices=[1, 2], default=1)
    parser.add_argument("--unit", type=auto_int, default=1)
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("-v", "--verbose", action="store_true", help="dump frames")

    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("probe", help="walk the whole JXD map")

    p = sub.add_parser("toggle", help="flip one relay and restore it")
    p.add_argument("index", type=int, help="relay number, 1-based")

    for name in ("read-coils", "read-discrete", "read-holding", "read-input"):
        p = sub.add_parser(name)
        p.add_argument("address", type=auto_int)
        p.add_argument("count", type=auto_int, nargs="?", default=1)

    p = sub.add_parser("write-coil")
    p.add_argument("address", type=auto_int)
    p.add_argument("value", type=auto_int)

    p = sub.add_parser(
        "write-coils", help="FC 0x0F, e.g. write-coils 0x0000 1 0 1 0 1 0"
    )
    p.add_argument("address", type=auto_int)
    p.add_argument("values", type=auto_int, nargs="+")

    args = parser.parse_args()

    if (args.port is None) == (args.tcp is None):
        parser.error("give one of --port and --tcp")
    bus: Modbus
    if args.tcp is not None:
        host, _, port = (
            args.tcp.rpartition(":") if ":" in args.tcp else (args.tcp, "", "502")
        )
        try:
            bus = ModbusTCP(host, int(port), args.unit, args.timeout)
        except (OSError, ValueError) as err:
            print(f"cannot connect to {args.tcp}: {err}", file=sys.stderr)
            return 2
    else:
        try:
            bus = ModbusRTU(
                args.port,
                args.baud,
                args.unit,
                args.timeout,
                args.parity,
                args.stop_bits,
            )
        except serial.SerialException as err:
            print(f"cannot open {args.port}: {err}", file=sys.stderr)
            return 2
    bus.verbose = args.verbose

    try:
        if args.command == "probe":
            return 1 if probe(bus) else 0
        if args.command == "toggle":
            return toggle(bus, args.index - 1)
        if args.command == "read-coils":
            print(fmt_bits(bus.read_coils(args.address, args.count)))
        elif args.command == "read-discrete":
            print(fmt_bits(bus.read_discrete_inputs(args.address, args.count)))
        elif args.command == "read-holding":
            print(bus.read_holding_registers(args.address, args.count))
        elif args.command == "read-input":
            print(bus.read_input_registers(args.address, args.count))
        elif args.command == "write-coil":
            bus.write_coil(args.address, bool(args.value))
            print("OK")
        elif args.command == "write-coils":
            bus.write_coils(args.address, [bool(v) for v in args.values])
            print("OK")
        return 0
    except ModbusError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    finally:
        bus.close()


if __name__ == "__main__":
    sys.exit(main())
