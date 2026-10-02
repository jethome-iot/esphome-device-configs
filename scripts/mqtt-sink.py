#!/usr/bin/env python3
"""A throwaway MQTT 3.1.1 broker for trying a device's MQTT side without a real one.

`scripts/qemu.sh` boots real firmware with working outbound networking, but its MQTT client
has nowhere to talk to: no broker is a dependency of this repository. This is one, in one file
with no dependencies, and it prints what it receives:

    scripts/mqtt-sink.py serve --port 18831
    scripts/qemu.sh run jxd-r6-e1eth-lcd --daemon --wait-http 240
    curl --digest -u admin:admin -H 'Content-Type: application/json' \\
      -d '{"enabled": true, "broker": "10.0.2.2", "port": 18831}' \\
      http://127.0.0.1:8080/api/device/mqtt

`10.0.2.2` is the host as the emulated device sees it (doc/QEMU.md, MQTT). The first enable
connects at once; a later change waits for a restart.

It is a broker only as deep as those checks go: it routes publishes to subscribers (`+` and `#`
included), keeps retained messages and replays them on subscribe, and delivers a client's will
when the connection drops without a DISCONNECT, including when the client goes silent, since
the keep-alive is enforced. Everything is delivered at QoS 0 and SUBACK grants 0 to say so; an
inbound QoS 2 publish is dispatched at once, so at least once rather than exactly once; there
are no persistent sessions. It listens on the loopback only, which the emulator reaches; do not
`--bind` it to a network you do not own.

`serve --login USER:PASSWORD` refuses any other credentials with CONNACK 5, as mosquitto 2
does under MQTT 3.1.1; `--refuse CODE` refuses every connection with that code; `--stall`
stops reading from a client once it is connected, so its socket fills up.

`pub` publishes one message, `sub` prints what a filter receives, retained messages first:

    scripts/mqtt-sink.py pub --port 18831 jxd-r6-e1eth-lcd-qemu/switch/relay_1/command ON
    scripts/mqtt-sink.py sub --port 18831 'homeassistant/#' --wait 2
"""

from __future__ import annotations

import argparse
import socket
import socketserver
import sys
import threading
import time
from pathlib import Path

CONNECT, CONNACK, PUBLISH, PUBACK = 1, 2, 3, 4
PUBREC, PUBREL, PUBCOMP = 5, 6, 7
SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK = 8, 9, 10, 11
PINGREQ, PINGRESP, DISCONNECT = 12, 13, 14

# MQTT 3.1.1, 3.2.2.3.
REFUSALS = {
    1: "unacceptable protocol version",
    2: "identifier rejected",
    3: "server unavailable",
    4: "bad user name or password",
    5: "not authorized",
}
# What a broker refuses a wrong login with: mosquitto 2 answers 5 for a wrong password, an
# unknown user and missing credentials alike.
LOGIN_REFUSAL = 5
# A stalled client's socket buffer: small, so the client's writes back up within seconds.
STALL_RCVBUF = 4096
# How much of a payload a log line shows.
SHOWN_BYTES = 200

STARTED = time.monotonic()
PRINT_LOCK = threading.Lock()


def log(kind, message):
    with PRINT_LOCK:
        print(f"[{time.monotonic() - STARTED:8.2f}] {kind:<4} {message}", flush=True)


def shown(payload):
    text = payload[:SHOWN_BYTES].decode("utf-8", "replace")
    if len(payload) > SHOWN_BYTES:
        text += f"… ({len(payload)} bytes)"
    return text


# --- wire format ---------------------------------------------------------------------------


def encode_len(length):
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        out.append(byte | 0x80 if length else byte)
        if not length:
            return bytes(out)


def encode_str(value):
    raw = value.encode()
    return len(raw).to_bytes(2, "big") + raw


def read_len(sock):
    """A remaining-length varint; None at the end of the stream."""
    multiplier, value = 1, 0
    for _ in range(4):
        byte = sock.recv(1)
        if not byte:
            return None
        value += (byte[0] & 0x7F) * multiplier
        if not byte[0] & 0x80:
            return value
        multiplier *= 128
    raise ValueError("malformed remaining length")


def read_exact(sock, count):
    buf = bytearray()
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def read_packet(sock):
    """(type, flags, body), or None at the end of the stream."""
    header = sock.recv(1)
    if not header:
        return None
    length = read_len(sock)
    if length is None:
        return None
    body = read_exact(sock, length) if length else b""
    if body is None:
        return None
    return header[0] >> 4, header[0] & 0x0F, body


def take_str(body, offset):
    length = int.from_bytes(body[offset : offset + 2], "big")
    start = offset + 2
    return body[start : start + length].decode("utf-8", "replace"), start + length


def take_bytes(body, offset):
    length = int.from_bytes(body[offset : offset + 2], "big")
    start = offset + 2
    return body[start : start + length], start + length


def publish_packet(topic, payload, retain=False):
    body = encode_str(topic) + payload
    return bytes([PUBLISH << 4 | int(retain)]) + encode_len(len(body)) + body


def topic_matches(pattern, topic):
    if topic.startswith("$") and pattern[:1] in ("+", "#"):
        return False
    parts, segments = pattern.split("/"), topic.split("/")
    for index, part in enumerate(parts):
        if part == "#":
            return index == len(parts) - 1
        if index >= len(segments) or (part not in ("+", segments[index])):
            return False
    return len(parts) == len(segments)


# --- broker --------------------------------------------------------------------------------


class Broker:
    def __init__(self, args):
        self.watched = args.topic
        self.login = args.login
        self.refuse = args.refuse
        self.stall = args.stall
        self.retained = {}
        self.clients = []
        self.lock = threading.Lock()

    def watching(self, topic):
        return not self.watched or any(topic_matches(f, topic) for f in self.watched)

    def add(self, client):
        with self.lock:
            self.clients.append(client)

    def remove(self, client):
        with self.lock:
            if client in self.clients:
                self.clients.remove(client)

    def refusal(self, username, password):
        """The CONNACK return code for these credentials; 0 lets the client in."""
        if self.refuse:
            return self.refuse
        if self.login is not None and (username, password) != self.login:
            return LOGIN_REFUSAL
        return 0

    def dispatch(self, topic, payload, retain):
        with self.lock:
            if retain:
                if payload:
                    self.retained[topic] = payload
                else:
                    self.retained.pop(topic, None)
            targets = list(self.clients)
        packet = publish_packet(topic, payload)
        for client in targets:
            if client.subscribed_to(topic):
                client.send(packet)

    def replay_retained(self, client, pattern):
        with self.lock:
            matching = [
                (t, p) for t, p in self.retained.items() if topic_matches(pattern, t)
            ]
        for topic, payload in matching:
            client.send(publish_packet(topic, payload, retain=True))


class Session(socketserver.BaseRequestHandler):
    def setup(self):
        self.broker = self.server.broker
        self.filters = []
        self.will = None
        self.client_id = "?"
        self.send_lock = threading.Lock()
        self.broker.add(self)

    def finish(self):
        self.broker.remove(self)
        if self.will:
            topic, payload, retain = self.will
            log("WILL", f"{self.client_id} -> {topic} {shown(payload)}")
            self.broker.dispatch(topic, payload, retain)

    def send(self, packet):
        try:
            with self.send_lock:
                self.request.sendall(packet)
        except OSError:
            pass

    def subscribed_to(self, topic):
        return any(topic_matches(f, topic) for f in self.filters)

    def handle(self):
        while True:
            try:
                packet = read_packet(self.request)
            except (OSError, ValueError):
                # A reset peer is how an emulator usually goes away; the will still fires from
                # finish(). A malformed packet ends the session.
                return
            if packet is None or not self.handle_packet(*packet):
                return

    def handle_packet(self, packet_type, flags, body):
        if packet_type == CONNECT:
            return self.on_connect(body)
        if packet_type == PUBLISH:
            self.on_publish(flags, body)
        elif packet_type == SUBSCRIBE:
            self.on_subscribe(body)
        elif packet_type == UNSUBSCRIBE:
            self.on_unsubscribe(body)
        elif packet_type == PUBREL:
            self.send(bytes([PUBCOMP << 4, 2]) + body[0:2])
        elif packet_type == PINGREQ:
            self.send(bytes([PINGRESP << 4, 0]))
        elif packet_type == DISCONNECT:
            self.will = None  # a clean disconnect discards the will
            log("BYE", self.client_id)
            return False
        return True

    def on_connect(self, body):
        protocol, offset = take_str(body, 0)
        level, connect_flags = body[offset], body[offset + 1]
        keep_alive = int.from_bytes(body[offset + 2 : offset + 4], "big")
        self.client_id, offset = take_str(body, offset + 4)
        will = None
        if connect_flags & 0x04:
            will_topic, offset = take_str(body, offset)
            will_payload, offset = take_bytes(body, offset)
            will = (will_topic, will_payload, bool(connect_flags & 0x20))
        username = password = None
        if connect_flags & 0x80:
            username, offset = take_str(body, offset)
        if connect_flags & 0x40:
            password, offset = take_str(body, offset)
        described = (
            f"{self.client_id} {protocol} level={level} keepalive={keep_alive}s"
            f" clean={int(bool(connect_flags & 0x02))}"
            f" will={will[0] if will else '-'}"
            f" user={username if username is not None else '-'}"
            f" password={'set' if password is not None else '-'}"
        )
        code = self.broker.refusal(username, password)
        if code:
            self.send(bytes([CONNACK << 4, 2, 0, code]))
            log(
                "DENY",
                f"{described} -> CONNACK {code} ({REFUSALS.get(code, 'refused')})",
            )
            return False
        self.will = will
        # Without it a half-open connection, which is what QEMU leaves when it is killed,
        # would hold the session open for good and the will would never fire.
        if keep_alive:
            self.request.settimeout(keep_alive * 1.5)
        self.send(bytes([CONNACK << 4, 2, 0, 0]))
        log("CONN", described)
        if self.broker.stall:
            log("STAL", f"{self.client_id}: no longer read")
            while True:
                time.sleep(3600)
        return True

    def on_publish(self, flags, body):
        topic, offset = take_str(body, 0)
        qos, retain = (flags >> 1) & 0x03, flags & 0x01
        packet_id = None
        if qos:
            packet_id = int.from_bytes(body[offset : offset + 2], "big")
            offset += 2
        payload = body[offset:]
        if self.broker.watching(topic):
            log("PUB", f"retain={retain} qos={qos} {topic} {shown(payload)}")
        if qos == 1:
            self.send(bytes([PUBACK << 4, 2]) + packet_id.to_bytes(2, "big"))
        elif qos == 2:
            # PUBREC now and PUBCOMP on the PUBREL, but dispatched here: at least once.
            self.send(bytes([PUBREC << 4, 2]) + packet_id.to_bytes(2, "big"))
        self.broker.dispatch(topic, payload, bool(retain))

    def on_subscribe(self, body):
        packet_id, offset = int.from_bytes(body[0:2], "big"), 2
        granted = bytearray()
        patterns = []
        while offset < len(body):
            pattern, offset = take_str(body, offset)
            offset += 1  # the requested QoS; everything goes out at 0
            self.filters.append(pattern)
            patterns.append(pattern)
            granted.append(0)
            log("SUB", f"{self.client_id} {pattern}")
        self.send(
            bytes([SUBACK << 4])
            + encode_len(2 + len(granted))
            + packet_id.to_bytes(2, "big")
            + granted
        )
        for pattern in patterns:
            self.broker.replay_retained(self, pattern)

    def on_unsubscribe(self, body):
        packet_id, offset = int.from_bytes(body[0:2], "big"), 2
        while offset < len(body):
            pattern, offset = take_str(body, offset)
            if pattern in self.filters:
                self.filters.remove(pattern)
        self.send(bytes([UNSUBACK << 4, 2]) + packet_id.to_bytes(2, "big"))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, broker):
        self.broker = broker
        super().__init__(address, Session)

    def server_bind(self):
        # On the listening socket, so an accepted one starts with the small window.
        if self.broker.stall:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, STALL_RCVBUF)
        super().server_bind()


def serve(args):
    broker = Broker(args)
    try:
        server = Server((args.bind, args.port), broker)
    except OSError as e:
        sys.exit(f"cannot listen on {args.bind}:{args.port}: {e}")
    with server:
        watched = ", ".join(args.topic) if args.topic else "everything"
        mode = []
        if args.refuse:
            mode.append(f"refusing every connection with CONNACK {args.refuse}")
        elif args.login:
            mode.append(f"letting in {args.login[0]} only")
        if args.stall:
            mode.append("stalling every client once connected")
        log(
            "UP",
            f"{args.bind}:{args.port}, logging {watched}"
            + "".join(f"; {m}" for m in mode),
        )
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            log("DOWN", "interrupted")


# --- one-shot clients ----------------------------------------------------------------------


def connect(args, client_id):
    sock = socket.create_connection((args.host, args.port), timeout=5)
    flags = 0x02  # clean session
    payload = encode_str(client_id)
    if args.username is not None:
        flags |= 0x80
        payload += encode_str(args.username)
    if args.password is not None:
        flags |= 0x40
        payload += encode_str(args.password)
    body = encode_str("MQTT") + bytes([4, flags]) + (60).to_bytes(2, "big") + payload
    sock.sendall(bytes([CONNECT << 4]) + encode_len(len(body)) + body)
    connack = read_exact(sock, 4)
    if not connack:
        sys.exit("the broker closed the connection during CONNECT")
    if connack[3]:
        sys.exit(
            f"the broker refused the connection: CONNACK {connack[3]} ({REFUSALS.get(connack[3], 'refused')})"
        )
    return sock


def publish(args):
    if args.file is not None:
        payload = Path(args.file).read_bytes()
    elif args.size is not None:
        payload = b"x" * args.size
    elif args.payload is not None:
        payload = args.payload.encode()
    else:
        sys.exit("give a payload, --size or --file")
    with connect(args, "mqtt-sink-pub") as sock:
        sock.sendall(publish_packet(args.topic, payload, retain=args.retain))
        sock.sendall(bytes([DISCONNECT << 4, 0]))
    print(
        f"published {len(payload)} bytes to {args.topic}{' (retained)' if args.retain else ''}"
    )


def subscribe(args):
    with connect(args, "mqtt-sink-sub") as sock:
        body = (1).to_bytes(2, "big") + encode_str(args.filter) + bytes([0])
        sock.sendall(bytes([SUBSCRIBE << 4 | 0x02]) + encode_len(len(body)) + body)
        deadline = time.monotonic() + args.wait
        received = 0
        while args.count is None or received < args.count:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            sock.settimeout(left)
            try:
                packet = read_packet(sock)
            except TimeoutError:
                break
            if packet is None:
                break
            packet_type, flags, data = packet
            if packet_type != PUBLISH:
                continue
            topic, offset = take_str(data, 0)
            if (flags >> 1) & 0x03:
                offset += 2
            print(
                f"{'retained ' if flags & 0x01 else ''}{topic} {shown(data[offset:])}",
                flush=True,
            )
            received += 1
        sock.sendall(bytes([DISCONNECT << 4, 0]))
    print(f"{received} message(s) on {args.filter}", file=sys.stderr)


def login(value):
    user, sep, password = value.partition(":")
    if not sep or not user:
        raise argparse.ArgumentTypeError("expected USER:PASSWORD")
    return user, password


def refusal_code(value):
    code = int(value)
    if not 1 <= code <= 255:
        raise argparse.ArgumentTypeError("a CONNACK return code is 1-255")
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)

    run = commands.add_parser("serve", help="run the broker and log what arrives")
    run.add_argument(
        "--bind", default="127.0.0.1", help="listen address (default: 127.0.0.1)"
    )
    run.add_argument(
        "--port", type=int, default=1883, help="listen port (default: 1883)"
    )
    run.add_argument(
        "--topic",
        action="append",
        default=[],
        metavar="FILTER",
        help="log only publishes matching this filter (repeatable, + and # allowed)",
    )
    run.add_argument(
        "--login",
        type=login,
        metavar="USER:PASSWORD",
        help=f"refuse any other credentials with CONNACK {LOGIN_REFUSAL}",
    )
    run.add_argument(
        "--refuse",
        type=refusal_code,
        metavar="CODE",
        help="refuse every connection with this CONNACK return code (4: bad user name or password, 5: not authorized)",
    )
    run.add_argument(
        "--stall",
        action="store_true",
        help="stop reading from a client once it is connected",
    )
    run.set_defaults(func=serve)

    for name, func, text in (
        ("pub", publish, "publish one message and exit"),
        ("sub", subscribe, "print what a filter receives, retained messages first"),
    ):
        client = commands.add_parser(name, help=text)
        client.add_argument("--host", default="127.0.0.1")
        client.add_argument("--port", type=int, default=1883)
        client.add_argument("--username")
        client.add_argument("--password")
        client.set_defaults(func=func)
        if name == "pub":
            client.add_argument("topic")
            client.add_argument("payload", nargs="?")
            client.add_argument("--retain", action="store_true")
            client.add_argument(
                "--size", type=int, metavar="N", help="a payload of N bytes"
            )
            client.add_argument(
                "--file", metavar="PATH", help="the payload is this file"
            )
        else:
            client.add_argument("filter")
            client.add_argument(
                "--wait",
                type=float,
                default=2.0,
                metavar="SECONDS",
                help="how long to listen",
            )
            client.add_argument(
                "--count", type=int, metavar="N", help="stop after N messages"
            )

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
