#!/usr/bin/env python3
"""Command-line file manager for the web_file_browser HTTP API.

Inspects and edits a device's LittleFS user partition without a browser: list,
tree, cat, get/put (recursive with -r), write, rm, mkdir, mv, cp, edit and an
interactive shell. Standard library only.

    scripts/device-files.py --host 192.168.1.50 -u admin:admin ls -l /
    DEVICE_HOST=192.168.1.50 scripts/device-files.py put -r ./www /www
    DEVICE_HOST=192.168.1.50 scripts/device-files.py shell
"""

from __future__ import annotations

import argparse
import base64
import cmd
from collections.abc import Iterator
from datetime import datetime
import hashlib
import http.client
import json
import mimetypes
import os
from pathlib import Path
import posixpath
import shlex
import subprocess
import sys
import tempfile
from urllib.parse import quote, urlencode
import uuid

HOST_ENV = "DEVICE_HOST"
USER_ENV = "DEVICE_USER"


class DeviceError(Exception):
    """A refused or failed request: the device's reason and the HTTP status."""

    def __init__(self, message: str, status: int = 0) -> None:
        super().__init__(message)
        self.status = status


# --- Paths and formatting ----------------------------------------------------


def remote_path(path: str, cwd: str = "/") -> str:
    """Absolute normalized device path; a relative one resolves against cwd."""
    if not path.startswith("/"):
        path = posixpath.join(cwd, path)
    # normpath keeps a leading "//", which POSIX lets mean something else.
    return "/" + posixpath.normpath(path).lstrip("/")


def human(size: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            break
        size /= 1024
    return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"


def fmt_time(mtime: int) -> str:
    # The device reports 0 where LittleFS keeps no timestamp (every directory).
    return datetime.fromtimestamp(mtime).isoformat(" ", "seconds") if mtime > 0 else "-"


def sort_entries(entries: list[dict]) -> list[dict]:
    """Directories first, then files, each by name."""
    return sorted(entries, key=lambda e: (e["type"] != "directory", e["name"]))


def parse_json(raw: bytes) -> object:
    try:
        return json.loads(raw)
    except ValueError:
        return None


# --- HTTP client -------------------------------------------------------------


def parse_challenge(header: str) -> dict[str, str]:
    """The comma-separated parameters of a WWW-Authenticate header, unquoted."""
    scheme, _, rest = header.partition(" ")
    out = {"scheme": scheme.lower()}
    for part in rest.split(","):
        name, _, value = part.strip().partition("=")
        if name:
            out[name.strip().lower()] = value.strip().strip('"')
    return out


class Credentials:
    """Answers a 401 the way the device's web server asks — Digest or Basic.

    Both schemes are stateless here: the challenge is kept so later requests carry an
    Authorization header from the start instead of costing a round trip each.
    """

    def __init__(self, username: str, password: str) -> None:
        self.username = username
        self.password = password
        self.challenge: dict[str, str] | None = None
        self.nonce_count = 0

    def meet(self, header: str) -> None:
        """Take a fresh challenge. The count is scoped to the nonce it answers, so it
        restarts: a server that tracks nonces rejects a reused count."""
        self.challenge = parse_challenge(header)
        self.nonce_count = 0

    def header(self, method: str, target: str) -> str | None:
        c = self.challenge
        if c is None:
            return None
        if c["scheme"] == "basic":
            token = base64.b64encode(
                f"{self.username}:{self.password}".encode()
            ).decode()
            return f"Basic {token}"
        md5 = lambda text: hashlib.md5(text.encode()).hexdigest()  # noqa: E731
        realm, nonce, qop = c.get("realm", ""), c.get("nonce", ""), c.get("qop", "auth")
        self.nonce_count += 1
        nc = f"{self.nonce_count:08x}"
        cnonce = uuid.uuid4().hex[:16]
        ha1 = md5(f"{self.username}:{realm}:{self.password}")
        ha2 = md5(f"{method}:{target}")
        response = md5(f"{ha1}:{nonce}:{nc}:{cnonce}:{qop}:{ha2}")
        fields = [
            f'username="{self.username}"',
            f'realm="{realm}"',
            f'nonce="{nonce}"',
            f'uri="{target}"',
            f"qop={qop}",
            f"nc={nc}",
            f'cnonce="{cnonce}"',
            f'response="{response}"',
        ]
        if "opaque" in c:
            fields.append(f'opaque="{c["opaque"]}"')
        return "Digest " + ", ".join(fields)


class Device:
    """Client for the routes under <prefix>, one connection per request."""

    def __init__(
        self,
        host: str,
        port: int,
        prefix: str,
        timeout: float,
        credentials: Credentials | None = None,
    ) -> None:
        self.host = host
        self.port = port
        self.prefix = "/" + prefix.strip("/")
        self.timeout = timeout
        self.credentials = credentials

    def _open(
        self,
        method: str,
        route: str,
        params: dict[str, str] | None = None,
        body: bytes | None = None,
        content_type: str | None = None,
    ) -> http.client.HTTPResponse:
        target = f"{self.prefix}/{route}"
        if params:
            target += "?" + urlencode(params, quote_via=quote, safe="/")
        # Close after each request: the device's socket pool is small and a
        # kept-alive connection would hold a slot until it times out.
        headers = {"Connection": "close"}
        if content_type:
            headers["Content-Type"] = content_type
        res = self._send(method, target, body, headers)
        # The first request of a session meets the challenge; answer it and retry once. Every
        # body here is bytes already in hand, so replaying one costs nothing.
        if (
            res.status == 401
            and self.credentials is not None
            and (header := res.getheader("WWW-Authenticate"))
        ):
            res.read()
            self.credentials.meet(header)
            res = self._send(method, target, body, headers)
        return res

    def _send(
        self,
        method: str,
        target: str,
        body: bytes | None,
        headers: dict[str, str],
    ) -> http.client.HTTPResponse:
        headers = dict(headers)
        if self.credentials is not None:
            authorization = self.credentials.header(method, target)
            if authorization:
                headers["Authorization"] = authorization
        conn = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        try:
            conn.request(method, target, body, headers)
            return conn.getresponse()
        except (OSError, http.client.HTTPException) as e:
            conn.close()
            raise DeviceError(f"{self.host}:{self.port}: {e}") from None

    @staticmethod
    def _check(data: object, res: http.client.HTTPResponse, what: str) -> None:
        # Body before status: the reason travels in the envelope next to the 400/404.
        if isinstance(data, dict) and data.get("success") is False:
            error = data.get("error") or "unknown error"
            raise DeviceError(f"{what}: {error}", res.status)
        if res.status >= 400:
            allow = res.getheader("Allow")
            hint = f", allows {allow}" if allow else ""
            raise DeviceError(f"{what}: HTTP {res.status}{hint}", res.status)

    def _call(
        self,
        method: str,
        route: str,
        what: str,
        params: dict[str, str] | None = None,
        body: bytes | None = None,
        content_type: str | None = None,
    ) -> object:
        """A JSON route: the parsed answer, or DeviceError with the device's reason."""
        res = self._open(method, route, params, body, content_type)
        with res:
            data = parse_json(res.read())
        self._check(data, res, what)
        return data

    def _form(self, route: str, what: str, **fields: str) -> None:
        body = urlencode(fields, quote_via=quote, safe="/").encode()
        self._call("POST", route, what, None, body, "application/x-www-form-urlencoded")

    def info(self) -> dict:
        data = self._call("GET", "info", "info")
        if not isinstance(data, dict):
            raise DeviceError(f"info: unexpected answer {data!r}")
        return data

    def list(self, path: str) -> list[dict]:
        data = self._call("GET", "list", path, {"path": path})
        if not isinstance(data, list):
            raise DeviceError(f"{path}: unexpected answer {data!r}")
        return data

    def stat(self, path: str) -> dict | None:
        """The parent's entry for path, None when missing; the root is synthesized."""
        if path == "/":
            return {"name": "", "type": "directory", "size": 0, "mtime": 0}
        try:
            entries = self.list(posixpath.dirname(path))
        except DeviceError:
            return None
        name = posixpath.basename(path)
        return next((e for e in entries if e["name"] == name), None)

    def walk(self, path: str) -> Iterator[tuple[str, list[dict]]]:
        """(directory, sorted entries) for path and all below it, parents first."""
        entries = sort_entries(self.list(path))
        yield path, entries
        for e in entries:
            if e["type"] == "directory":
                yield from self.walk(posixpath.join(path, e["name"]))

    def download(self, path: str) -> bytes:
        res = self._open("GET", "download", {"path": path})
        with res:
            raw = res.read()
        if res.status >= 400:
            self._check(parse_json(raw), res, path)
        return raw

    def write(self, path: str, data: bytes) -> None:
        # The raw body is the file; a form or multipart type would be parsed as fields.
        self._call(
            "POST", "write", path, {"path": path}, data, "application/octet-stream"
        )

    def upload(self, path: str, data: bytes) -> None:
        if not data:
            # The multipart reader drops a zero-length part, so nothing would be written.
            self.write(path, b"")
            return
        name = posixpath.basename(path).replace('"', "%22")
        mime = mimetypes.guess_type(name)[0] or "application/octet-stream"
        boundary = uuid.uuid4().hex
        head = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="{name}"\r\n'
            f"Content-Type: {mime}\r\n\r\n"
        )
        body = head.encode() + data + f"\r\n--{boundary}--\r\n".encode()
        content_type = f"multipart/form-data; boundary={boundary}"
        self._call("POST", "upload", path, {"path": path}, body, content_type)

    def delete(self, path: str) -> None:
        self._form("delete", path, path=path)

    def mkdir(self, path: str) -> None:
        self._form("mkdir", path, path=path)

    def ensure_dir(self, path: str) -> None:
        """Every level, top down: the device's mkdir is idempotent but not recursive."""
        parts = [p for p in path.split("/") if p]
        for i in range(len(parts)):
            self.mkdir("/" + "/".join(parts[: i + 1]))

    def rename(self, old: str, new: str) -> None:
        self._form("rename", f"{old} -> {new}", old_path=old, new_path=new)

    def copy(self, old: str, new: str) -> None:
        self._form("copy", f"{old} -> {new}", old_path=old, new_path=new)


# --- Output and transfers ----------------------------------------------------


def print_entries(entries: list[dict], long: bool) -> None:
    for e in sort_entries(entries):
        is_dir = e["type"] == "directory"
        name = e["name"] + ("/" if is_dir else "")
        if long:
            size = "-" if is_dir else str(e["size"])
            kind = "d" if is_dir else "-"
            print(f"{kind} {size:>9} {fmt_time(e['mtime']):19} {name}")
        else:
            print(name)


def print_tree(dev: Device, path: str, indent: str = "") -> tuple[int, int]:
    """Print the subtree under path; returns (directories, files) below it."""
    dirs = files = 0
    entries = sort_entries(dev.list(path))
    for i, e in enumerate(entries):
        last = i == len(entries) - 1
        branch, extend = ("└── ", "    ") if last else ("├── ", "│   ")
        if e["type"] == "directory":
            print(f"{indent}{branch}{e['name']}/")
            d, f = print_tree(dev, posixpath.join(path, e["name"]), indent + extend)
            dirs, files = dirs + 1 + d, files + f
        else:
            print(f"{indent}{branch}{e['name']}")
            files += 1
    return dirs, files


def list_or_file(dev: Device, path: str) -> list[dict]:
    """A directory's entries, or a one-entry listing when path is a file."""
    try:
        return dev.list(path)
    except DeviceError:
        entry = dev.stat(path)
        if entry is None or entry["type"] != "file":
            raise
        return [entry]


def pull_tree(dev: Device, remote: str, local: Path) -> None:
    """The contents of the remote directory into local, created if missing."""
    for directory, entries in dev.walk(remote):
        target = local / posixpath.relpath(directory, remote)
        target.mkdir(parents=True, exist_ok=True)
        for e in entries:
            if e["type"] == "file":
                src = posixpath.join(directory, e["name"])
                (target / e["name"]).write_bytes(dev.download(src))
                print(src)


def push_tree(dev: Device, local: Path, remote: str) -> None:
    """The contents of the local directory into remote, created if missing."""
    dev.ensure_dir(remote)
    for root, dirs, files in os.walk(local):
        rel = Path(root).relative_to(local).as_posix()
        here = remote_path(posixpath.join(remote, rel))
        dirs.sort()
        for d in dirs:
            dev.mkdir(posixpath.join(here, d))
        for f in sorted(files):
            dest = posixpath.join(here, f)
            dev.upload(dest, (Path(root) / f).read_bytes())
            print(dest)


# --- Commands ----------------------------------------------------------------
# Each takes the parsed arguments and the remote directory relative paths resolve
# against: "/" from the command line, the shell's cwd inside it.


def cmd_info(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    info = dev.info()
    if args.json:
        print(json.dumps(info, indent=2))
        return
    if not info.get("valid", True):
        raise DeviceError("storage is not mounted")
    total, used = info["total"], info["used"]
    pct = f" ({used * 100 // total}%)" if total else ""
    print(f"filesystem: {info.get('filesystem', '?')}")
    print(f"total:      {human(total)}")
    print(f"used:       {human(used)}{pct}")
    print(f"free:       {human(info['free'])}")


def cmd_ls(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    path = remote_path(args.path, cwd)
    if args.recursive:
        listing = dict(dev.walk(path))
    else:
        listing = {path: list_or_file(dev, path)}
    if args.json:
        print(json.dumps(listing if args.recursive else listing[path], indent=2))
        return
    for i, (directory, entries) in enumerate(listing.items()):
        if args.recursive:
            print(f"{'' if i == 0 else chr(10)}{directory}:")
        print_entries(entries, args.long)


def cmd_tree(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    path = remote_path(args.path, cwd)
    print(path)
    dirs, files = print_tree(dev, path)
    print(f"\n{dirs} directories, {files} files")


def cmd_cat(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    for path in args.path:
        data = dev.download(remote_path(path, cwd))
        sys.stdout.flush()
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()


def cmd_get(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    remote = remote_path(args.remote, cwd)
    local = Path(args.local or posixpath.basename(remote) or "root")
    entry = dev.stat(remote)
    if entry and entry["type"] == "directory":
        if not args.recursive:
            raise DeviceError(f"{remote}: is a directory (use -r)")
        pull_tree(dev, remote, local)
        return
    if local.is_dir():
        local /= posixpath.basename(remote)
    local.write_bytes(dev.download(remote))


def cmd_put(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    local = Path(args.local)
    # "." and ".." have no name of their own; the absolute path does.
    name = Path(os.path.abspath(local)).name
    if not args.remote and not name:
        raise DeviceError(f"{local}: give the remote path explicitly")
    remote = remote_path(args.remote or name, cwd)
    if local.is_dir():
        if not args.recursive:
            raise DeviceError(f"{local}: is a directory (use -r)")
        push_tree(dev, local, remote)
        return
    entry = dev.stat(remote)
    if entry and entry["type"] == "directory":
        remote = posixpath.join(remote, name)
    dev.upload(remote, local.read_bytes())


def cmd_write(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    data = args.text.encode() if args.text is not None else sys.stdin.buffer.read()
    dev.write(remote_path(args.path, cwd), data)


def cmd_rm(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    for p in args.path:
        path = remote_path(p, cwd)
        entry = dev.stat(path)
        if entry and entry["type"] == "directory" and not args.recursive:
            raise DeviceError(f"{path}: is a directory (use -r)")
        dev.delete(path)


def cmd_mkdir(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    for p in args.path:
        path = remote_path(p, cwd)
        if args.parents:
            dev.ensure_dir(path)
        else:
            dev.mkdir(path)


def cmd_mv_cp(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    old, new = remote_path(args.old, cwd), remote_path(args.new, cwd)
    entry = dev.stat(new)
    if entry and entry["type"] == "directory":
        # Into the directory, as mv and cp do; the device refuses an existing target.
        new = posixpath.join(new, posixpath.basename(old))
    getattr(dev, args.op)(old, new)


def cmd_edit(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    path = remote_path(args.path, cwd)
    try:
        original = dev.download(path)
    except DeviceError as e:
        if e.status != 404:
            raise
        original = b""
        print(f"{path}: new file", file=sys.stderr)
    fd, tmp = tempfile.mkstemp(suffix=posixpath.splitext(path)[1] or ".txt")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(original)
        editor = shlex.split(os.environ.get("EDITOR") or "vi")
        if subprocess.call([*editor, tmp]) != 0:
            raise DeviceError(f"{editor[0]} failed, {path} left untouched")
        edited = Path(tmp).read_bytes()
    finally:
        os.unlink(tmp)
    if edited == original:
        print(f"{path}: unchanged", file=sys.stderr)
    else:
        dev.write(path, edited)


def cmd_shell(dev: Device, args: argparse.Namespace, cwd: str) -> None:
    Shell(dev, build_commands(shell=True)).cmdloop()


# --- Interactive shell -------------------------------------------------------

SHELL_ONLY = {
    "cd": "cd [path]  change the remote directory",
    "pwd": "pwd  print the remote directory",
    "quit": "quit  leave the shell (also exit, Ctrl-D)",
}


class Shell(cmd.Cmd):
    """Interactive session with a device-side current directory."""

    def __init__(self, dev: Device, parser: argparse.ArgumentParser) -> None:
        super().__init__()
        self.dev = dev
        self.parser = parser
        self.cwd = "/"
        self.interactive = sys.stdin.isatty()
        self.intro = "help lists commands, quit leaves" if self.interactive else None
        self.set_prompt()

    def set_prompt(self) -> None:
        self.prompt = f"{self.dev.host}:{self.cwd}> " if self.interactive else ""

    def emptyline(self) -> bool:
        # cmd repeats the last command on an empty line; not with rm around.
        return False

    def default(self, line: str) -> None:
        try:
            args = self.parser.parse_args(shlex.split(line))
            args.func(self.dev, args, self.cwd)
        except SystemExit:
            pass  # argparse has printed its usage or help
        except (DeviceError, OSError, http.client.HTTPException, ValueError) as e:
            print(f"error: {e}", file=sys.stderr)

    def do_cd(self, arg: str) -> None:
        words = shlex.split(arg)
        path = remote_path(words[0], self.cwd) if words else "/"
        try:
            self.dev.list(path)  # the only way to know it is a directory
        except DeviceError as e:
            print(f"error: {e}", file=sys.stderr)
            return
        self.cwd = path
        self.set_prompt()

    def do_pwd(self, arg: str) -> None:
        print(self.cwd)

    def do_quit(self, arg: str) -> bool:
        return True

    do_exit = do_quit

    def do_EOF(self, arg: str) -> bool:
        if self.interactive:
            print()
        return True

    def do_help(self, arg: str) -> None:
        if arg in SHELL_ONLY:
            print(SHELL_ONLY[arg])
        elif arg:
            self.default(f"{arg} -h")
        else:
            self.parser.print_help()
            print("\nshell:")
            for line in SHELL_ONLY.values():
                print(f"  {line}")

    def completenames(self, text: str, *ignored: object) -> list[str]:
        names = [*COMMANDS, *SHELL_ONLY, "exit"]
        return [n for n in names if n.startswith(text)]

    def completedefault(
        self, text: str, line: str, begidx: int, endidx: int
    ) -> list[str]:
        """Remote names for the word at the cursor, resolved against cwd."""
        words = line[:endidx].split()
        word = words[-1] if words and not line[endidx - 1].isspace() else ""
        head, sep, _ = word.rpartition("/")
        directory = remote_path(head or "/", self.cwd) if sep else self.cwd
        try:
            entries = self.dev.list(directory)
        except DeviceError:
            return []
        # readline replaces only `text`, the part after its last delimiter, so
        # every candidate is the matching full name cut at that offset.
        keep = len(word) - len(text)
        out = []
        for e in entries:
            name = head + sep + e["name"] + ("/" if e["type"] == "directory" else "")
            if name.startswith(word):
                out.append(name[keep:])
        return out


# --- Argument parsing --------------------------------------------------------

COMMANDS = "info ls tree cat get put write rm mkdir mv cp edit".split()


def flag(p: argparse.ArgumentParser, name: str, dest: str, help_: str) -> None:
    p.add_argument(name, dest=dest, action="store_true", help=help_)


def build_commands(shell: bool) -> argparse.ArgumentParser:
    """The subcommand parser; inside the shell, without shell and with write --text only."""
    if shell:
        parser = argparse.ArgumentParser(
            prog="shell", usage=argparse.SUPPRESS, add_help=False
        )
    else:
        parser = argparse.ArgumentParser(
            description=__doc__.split("\n\n")[0],
            epilog=f"The host may also come from ${HOST_ENV}.",
        )
        parser.add_argument(
            "--host", default=os.environ.get(HOST_ENV), help="device name or address"
        )
        parser.add_argument("--port", type=int, default=80)
        parser.add_argument(
            "--prefix",
            default="/files",
            help="url_prefix of web_file_browser (default /files)",
        )
        parser.add_argument(
            "--timeout", type=float, default=30, help="seconds per request (default 30)"
        )
        parser.add_argument(
            "-u",
            "--user",
            default=os.environ.get(USER_ENV),
            metavar="USER:PASSWORD",
            help=(
                "credentials for a device whose web_server has an auth: block; "
                f"also ${USER_ENV}"
            ),
        )
    sub = parser.add_subparsers(dest="command", metavar="command", required=True)

    p = sub.add_parser("info", help="capacity and usage")
    flag(p, "--json", "json", "the raw answer")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("ls", help="list a directory")
    p.add_argument("path", nargs="?", default=".")
    flag(p, "-l", "long", "type, size and mtime")
    flag(p, "-R", "recursive", "recurse")
    flag(p, "--json", "json", "the raw answer")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("tree", help="the directory tree")
    p.add_argument("path", nargs="?", default=".")
    p.set_defaults(func=cmd_tree)

    p = sub.add_parser("cat", help="write a file to stdout")
    p.add_argument("path", nargs="+")
    p.set_defaults(func=cmd_cat)

    p = sub.add_parser("get", help="download a file, or a directory with -r")
    p.add_argument("remote")
    p.add_argument("local", nargs="?", help="default: the remote name")
    flag(p, "-r", "recursive", "a directory's contents into local")
    p.set_defaults(func=cmd_get)

    p = sub.add_parser("put", help="upload a file, or a directory with -r")
    p.add_argument("local")
    p.add_argument("remote", nargs="?", help="default: the local name")
    flag(p, "-r", "recursive", "a directory's contents into remote")
    p.set_defaults(func=cmd_put)

    p = sub.add_parser("write", help="write stdin or --text to a file")
    p.add_argument("path")
    p.add_argument("--text", required=shell, help="the content, instead of stdin")
    p.set_defaults(func=cmd_write)

    p = sub.add_parser("rm", help="delete files, or directories with -r")
    p.add_argument("path", nargs="+")
    flag(p, "-r", "recursive", "allow directories")
    p.set_defaults(func=cmd_rm)

    p = sub.add_parser("mkdir", help="create directories")
    p.add_argument("path", nargs="+")
    flag(p, "-p", "parents", "create missing parents too")
    p.set_defaults(func=cmd_mkdir)

    for name, op, help_ in (
        ("mv", "rename", "rename or move"),
        ("cp", "copy", "copy, recursively"),
    ):
        p = sub.add_parser(name, help=help_)
        p.add_argument("old")
        p.add_argument("new", help="an existing directory means into it")
        p.set_defaults(func=cmd_mv_cp, op=op)

    p = sub.add_parser("edit", help="edit a file in $EDITOR (default vi)")
    p.add_argument("path")
    p.set_defaults(func=cmd_edit)

    if shell:
        for name, p in sub.choices.items():
            p.prog = name  # "usage: ls ...", not the parent's prefix
    else:
        p = sub.add_parser("shell", help="interactive session")
        p.set_defaults(func=cmd_shell)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_commands(shell=False)
    args = parser.parse_args(argv)
    if not args.host:
        parser.error(f"--host or ${HOST_ENV} is required")
    credentials = None
    if args.user:
        username, _, password = args.user.partition(":")
        credentials = Credentials(username, password)
    dev = Device(args.host, args.port, args.prefix, args.timeout, credentials)
    try:
        args.func(dev, args, "/")
    except BrokenPipeError:
        # `cat ... | head`: the reader left; keep Python quiet about the flush at exit.
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
    except (DeviceError, OSError, http.client.HTTPException) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
