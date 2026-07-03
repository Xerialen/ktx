#!/usr/bin/env python3
"""Minimal QuakeWorld client used to drive KTX bot commands in lab runs.

This is intentionally small and protocol-narrow. It completes enough of the
QuakeWorld challenge/connect/sign-on flow to send reliable string commands such
as `botcmd addbot` from a connected client context.

Run it on the same host as MVDSV, or point `--host` at a reachable server:

    python experiments/qw_min_client.py 28599 --local-port 28630 --bot-count 2
"""

from __future__ import annotations

import argparse
import re
import socket
import struct
import time

CL_NOP = 1
CL_STRINGCMD = 4
SVC_STUFFTEXT = 9
SVC_SERVERDATA = 11
S2C_CONNECTION = ord("j")
USERINFO_VALUE_RE = re.compile(r"^[ -~]{1,32}$")


def userinfo_value(value: str, label: str) -> str:
    if not isinstance(value, str) or not USERINFO_VALUE_RE.match(value):
        raise ValueError(f"invalid {label}")
    if "\\" in value or '"' in value:
        raise ValueError(f"invalid {label}")
    return value


def build_userinfo(name: str, *, team: str | None = None, spectator: bool = False) -> str:
    pairs = [
        ("name", userinfo_value(name, "name")),
        ("rate", "25000"),
        ("topcolor", "0"),
        ("bottomcolor", "0"),
        ("pmodel", "0"),
        ("emodel", "0"),
        ("Qizmo", "1"),
        ("*z_ext", "0"),
    ]
    if team:
        pairs.append(("team", userinfo_value(team, "team")))
    if spectator:
        pairs.append(("spectator", "1"))
    return "".join(f"\\{key}\\{value}" for key, value in pairs)


def signon_botcmds(bot_count: int, botcmds: list[str]) -> list[str]:
    """botcmd lines sent once right after sign-on.

    The first auto `botcmd addbot` (when --bot-count > 0, remaining ones are
    paced by --bot-spacing as before) plus one `botcmd <arg>` per --botcmd,
    e.g. --botcmd removebot / --botcmd removeall (control bridge, LD-F2 #96).
    """
    lines: list[str] = []
    if bot_count > 0:
        lines.append("botcmd addbot")
    lines.extend(f"botcmd {cmd}" for cmd in botcmds)
    return lines


def signon_commands(bot_count: int, botcmds: list[str], commands: list[str]) -> list[str]:
    """All reliable string commands sent once right after sign-on."""
    return [*signon_botcmds(bot_count, botcmds), *commands]


def safe_server_commands_from_stufftext(text: str) -> list[str]:
    """Return the tiny allowlisted subset of stuffed client commands we execute.

    KTX stuffs `cmd ack infoset`/`cmd ack noinfoset` after sign-on and expects
    the client to send the `ack ...` server command back. Without this, a
    spectator shim can remain in a connecting-ish state and be dropped by the
    server login timeout during long bot-only recordings.
    """
    commands: list[str] = []
    for part in re.split(r"[;\n]+", text):
        part = part.strip()
        if not part.startswith("cmd "):
            continue
        server_command = part[4:].strip()
        if server_command in ("ack infoset", "ack noinfoset"):
            commands.append(server_command)
    return commands


class QWMinClient:
    def __init__(
        self,
        host: str,
        port: int,
        local_port: int,
        run_for: float,
        bot_count: int,
        bot_spacing: float,
        name: str,
        team: str | None,
        spectator: bool,
        verbose: bool,
        botcmds: list[str] | None = None,
        commands: list[str] | None = None,
        botcmd_delay: float = 0.0,
    ) -> None:
        self.host = host
        self.port = port
        self.run_for = run_for
        self.bot_count = bot_count
        self.bot_spacing = bot_spacing
        self.name = name
        self.team = team
        self.spectator = spectator
        self.verbose = verbose
        self.botcmds = list(botcmds or [])
        self.commands = list(commands or [])
        self.botcmd_delay = botcmd_delay

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", local_port))
        self.sock.settimeout(0.25)
        self.addr = (host, port)
        self.qport = self.sock.getsockname()[1] & 0xFFFF

        self.out_seq = 1
        self.last_server_seq = 0
        self.server_rel = 0
        self.spawncount: int | None = None
        self.sent_stuff_acks: set[str] = set()

    def log(self, message: str) -> None:
        if self.verbose:
            print(message, flush=True)

    def recv_until(self, predicate, deadline: float, label: str) -> bytes:
        while time.time() < deadline:
            try:
                data, src = self.sock.recvfrom(8192)
            except socket.timeout:
                continue
            self.log(f"recv {len(data)} from {src}: {data[:80]!r}")
            if predicate(data):
                return data
        raise TimeoutError(label)

    def send_oob(self, text: str) -> None:
        packet = b"\xff\xff\xff\xff" + text.encode("ascii") + b"\n"
        self.log(f"send_oob {packet!r}")
        self.sock.sendto(packet, self.addr)

    def send_reliable(self, commands: list[str]) -> None:
        # The lab uses localhost on the remote server, so this intentionally
        # omits retransmit-until-ack machinery from a full QuakeWorld client.
        payload = bytearray()
        for command in commands:
            self.log(f"send_cmd seq={self.out_seq} {command}")
            payload.append(CL_STRINGCMD)
            payload.extend(command.encode("ascii"))
            payload.append(0)

        w1 = self.out_seq | 0x80000000
        w2 = self.last_server_seq | ((self.server_rel & 1) << 31)
        packet = struct.pack("<IIH", w1, w2, self.qport) + payload
        self.sock.sendto(packet, self.addr)
        self.out_seq += 1

    def send_nop(self) -> None:
        w1 = self.out_seq
        w2 = self.last_server_seq | ((self.server_rel & 1) << 31)
        packet = struct.pack("<IIH", w1, w2, self.qport) + bytes([CL_NOP])
        self.sock.sendto(packet, self.addr)
        self.out_seq += 1

    @staticmethod
    def read_cstr(data: bytes, pos: int) -> tuple[str, int]:
        end = data.find(b"\x00", pos)
        if end < 0:
            return "", len(data)
        return data[pos:end].decode("latin1", "replace"), end + 1

    def parse_payload(self, payload: bytes) -> None:
        i = 0
        while i < len(payload):
            byte = payload[i]
            if byte == SVC_SERVERDATA and i + 9 <= len(payload):
                proto = struct.unpack_from("<i", payload, i + 1)[0]
                spawncount = struct.unpack_from("<i", payload, i + 5)[0]
                self.spawncount = spawncount
                self.log(f"svc_serverdata proto={proto} spawncount={spawncount}")
                i += 9
            elif byte == SVC_STUFFTEXT:
                text, next_i = self.read_cstr(payload, i + 1)
                self.log(f"svc_stufftext {text!r}")
                for command in safe_server_commands_from_stufftext(text):
                    if command not in self.sent_stuff_acks:
                        self.send_reliable([command])
                        self.sent_stuff_acks.add(command)
                i = next_i
            else:
                i += 1

    def process_packet(self, data: bytes) -> None:
        if data.startswith(b"\xff\xff\xff\xff"):
            self.log(f"oob {data!r}")
            return
        if len(data) < 8:
            self.log(f"short packet {data!r}")
            return

        w1, w2 = struct.unpack_from("<II", data, 0)
        reliable = (w1 >> 31) & 1
        seq = w1 & 0x7FFFFFFF
        if seq <= self.last_server_seq:
            self.log(f"stale server seq={seq} last={self.last_server_seq}")
            return

        self.last_server_seq = seq
        if reliable:
            self.server_rel ^= 1

        payload = data[8:]
        self.log(f"server seq={seq} rel={reliable} ack={w2 & 0x7FFFFFFF} payload_len={len(payload)}")
        self.parse_payload(payload)

    def connect(self) -> None:
        self.send_oob("getchallenge")
        challenge_packet = self.recv_until(
            lambda data: data.startswith(b"\xff\xff\xff\xffc"),
            time.time() + 3,
            "challenge",
        )
        challenge_text = challenge_packet[5:].split(b"\x00", 1)[0].decode("ascii", "replace")
        match = re.match(r"[-0-9]+", challenge_text)
        if not match:
            raise RuntimeError(f"could not parse challenge from {challenge_text!r}")

        challenge = int(match.group(0))
        self.log(f"challenge={challenge}")
        userinfo = build_userinfo(self.name, team=self.team, spectator=self.spectator)
        self.send_oob(f'connect 28 {self.qport} {challenge} "{userinfo}"')
        connection_packet = self.recv_until(
            lambda data: data.startswith(b"\xff\xff\xff\xff")
            and len(data) > 4
            and data[4] == S2C_CONNECTION,
            time.time() + 3,
            "connection",
        )
        self.log(f"connected_oob {connection_packet!r}")
        self.send_reliable(["new"])

    def run(self) -> None:
        self.connect()

        end = time.time() + self.run_for
        next_nop = 0.0
        signed_on = False
        bots_sent = 0
        next_bot_time = 0.0
        deferred_botcmds_at = None   # when to send --botcmd extras (delay mode)

        while time.time() < end:
            try:
                data, src = self.sock.recvfrom(8192)
                self.log(f"recv {len(data)} from {src}")
                self.process_packet(data)
            except socket.timeout:
                pass

            now = time.time()
            if self.spawncount is not None and not signed_on:
                commands = [
                    f"prespawn {self.spawncount} 0 0",
                    f"spawn {self.spawncount} 0",
                    f"begin {self.spawncount}",
                ]
                if self.bot_count > 0:
                    bots_sent = 1
                    next_bot_time = now + self.bot_spacing
                # E1 lab fix: with --botcmd-delay > 0, the botcmd extras are
                # NOT bundled with `begin`. KTX's ClientCommand drops commands
                # while the just-entered client is not yet fully accepted
                # (k_accepted / spectator-entry completes a beat after begin),
                # so a bundled `botcmd addkbot ...` is silently discarded.
                # Sending it as a SEPARATE reliable packet a couple seconds
                # later lands it after entry. Delay 0 = legacy bundled behavior.
                if self.botcmd_delay > 0:
                    commands.extend(self.commands)   # raw --cmd still at signon
                    deferred_botcmds_at = now + self.botcmd_delay
                else:
                    commands.extend(signon_commands(self.bot_count, self.botcmds, self.commands))
                self.send_reliable(commands)
                signed_on = True
                next_nop = now + 0.5

            if deferred_botcmds_at is not None and now >= deferred_botcmds_at:
                self.send_reliable(signon_botcmds(0, self.botcmds))
                deferred_botcmds_at = None

            if signed_on and bots_sent < self.bot_count and now >= next_bot_time:
                self.send_reliable(["botcmd addbot"])
                bots_sent += 1
                next_bot_time = now + self.bot_spacing

            if signed_on and now >= next_nop:
                self.send_nop()
                next_nop = now + 0.75

        print(f"done bots_sent={bots_sent}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Minimal QuakeWorld client for KTX bot lab control.")
    parser.add_argument("port", type=int, help="MVDSV UDP port to connect to.")
    parser.add_argument("--host", default="127.0.0.1", help="Server host. Defaults to 127.0.0.1.")
    parser.add_argument("--local-port", type=int, default=0, help="Local UDP port/qport. Defaults to an OS-chosen port.")
    parser.add_argument("--run-for", type=float, default=45.0, help="Seconds to stay connected. Defaults to 45.")
    parser.add_argument("--bot-count", type=int, default=2, help="Number of botcmd addbot commands to send. Defaults to 2.")
    parser.add_argument("--bot-spacing", type=float, default=8.0, help="Seconds between addbot commands. Defaults to 8.")
    parser.add_argument("--name", default="KomodoPy", help="Client name. Defaults to KomodoPy.")
    parser.add_argument("--team", help="Optional team userinfo value, needed for KTX team-mode player connects.")
    parser.add_argument("--spectator", action="store_true", help="Connect as a spectator so control does not occupy a player slot.")
    parser.add_argument("--quiet", action="store_true", help="Only print the final completion line.")
    parser.add_argument(
        "--botcmd",
        action="append",
        default=[],
        help="Extra 'botcmd <arg>' sent once after sign-on (repeatable), e.g. --botcmd removebot.",
    )
    parser.add_argument(
        "--cmd",
        action="append",
        default=[],
        help="Extra raw client command sent once after sign-on (repeatable). Intended for allowlisted lab control only.",
    )
    parser.add_argument(
        "--botcmd-delay",
        type=float,
        default=0.0,
        help="Seconds after sign-on to send --botcmd extras as a SEPARATE packet "
             "(not bundled with begin). >0 fixes KTX dropping bot adds issued before "
             "client entry completes. Default 0 = legacy bundled behavior.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    client = QWMinClient(
        host=args.host,
        port=args.port,
        local_port=args.local_port,
        run_for=args.run_for,
        bot_count=args.bot_count,
        bot_spacing=args.bot_spacing,
        name=args.name,
        team=args.team,
        spectator=args.spectator,
        verbose=not args.quiet,
        botcmds=args.botcmd,
        commands=args.cmd,
        botcmd_delay=args.botcmd_delay,
    )
    client.run()


if __name__ == "__main__":
    main()
