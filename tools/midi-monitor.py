#!/usr/bin/env python3
"""Print decoded MIDI messages arriving on a USB-serial adapter.

Usage: tools/.venv/bin/python tools/midi-monitor.py [--port PATH] [--baud N]

If --port is omitted the script lists detected serial ports and prompts for a
selection. Baud defaults to 31250 (standard MIDI 1.0). Press Ctrl-C to stop.
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.stderr.write(
        "pyserial not found. Use the project venv:\n"
        "  tools/.venv/bin/python tools/midi-monitor.py\n"
    )
    sys.exit(1)


# --- Parser --------------------------------------------------------------


class MidiParser:
    """Stateful MIDI byte-stream parser. feed(byte) returns 0-or-more strings."""

    def __init__(self) -> None:
        self.running_status: int | None = None
        self.expected: int = 0
        self.collected: list[int] = []
        self.in_sysex: bool = False
        self.sysex_len: int = 0

    def feed(self, b: int) -> list[str]:
        out: list[str] = []

        if b & 0x80:
            # Status byte. Resets data collection.
            self.collected = []

            if b >= 0xF8:
                # System real-time — single-byte messages, do not disturb running status.
                out.append(_realtime_name(b))
                return out
            if b == 0xF0:
                self.in_sysex = True
                self.sysex_len = 0
                self.running_status = None
                return out
            if b == 0xF7:
                if self.in_sysex:
                    out.append(f"SysEx ({self.sysex_len} bytes)")
                    self.in_sysex = False
                return out
            if b >= 0xF1:
                # Other system common (MTC quarter frame / Song Position / Song Select).
                self.running_status = None
                self.expected = 0
                return out

            # Channel voice / mode message.
            self.running_status = b
            kind = b & 0xF0
            self.expected = 1 if kind in (0xC0, 0xD0) else 2
            return out

        # Data byte.
        if self.in_sysex:
            self.sysex_len += 1
            return out

        if self.running_status is None:
            return out  # orphan data byte — ignore

        self.collected.append(b)
        if len(self.collected) >= self.expected:
            msg = _format_channel(self.running_status, self.collected)
            if msg:
                out.append(msg)
            # Reset for running status: same status can recur with new data.
            self.collected = []
        return out


def _format_channel(status: int, data: list[int]) -> str | None:
    kind = status & 0xF0
    ch = (status & 0x0F) + 1
    if kind == 0x80:
        return f"Note Off    ch {ch:>2}  note {data[0]:>3}  vel {data[1]:>3}"
    if kind == 0x90:
        if data[1] == 0:
            return f"Note Off    ch {ch:>2}  note {data[0]:>3}  (note-on vel 0)"
        return f"Note On     ch {ch:>2}  note {data[0]:>3}  vel {data[1]:>3}"
    if kind == 0xA0:
        return f"Poly AT     ch {ch:>2}  note {data[0]:>3}  pressure {data[1]:>3}"
    if kind == 0xB0:
        return f"CC          ch {ch:>2}  controller {data[0]:>3}  value {data[1]:>3}"
    if kind == 0xC0:
        return f"PC          ch {ch:>2}  program {data[0]:>3}"
    if kind == 0xD0:
        return f"Channel AT  ch {ch:>2}  pressure {data[0]:>3}"
    if kind == 0xE0:
        pitch = (data[1] << 7) | data[0]
        return f"Pitch Bend  ch {ch:>2}  value {pitch:>5}  (center 8192)"
    return None


def _realtime_name(b: int) -> str:
    return {
        0xF8: "Timing Clock",
        0xFA: "Start",
        0xFB: "Continue",
        0xFC: "Stop",
        0xFE: "Active Sensing",
        0xFF: "System Reset",
    }.get(b, f"Realtime 0x{b:02X}")


# --- Port selection ------------------------------------------------------


def pick_port_interactive() -> str | None:
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        sys.stderr.write("No serial ports detected. Plug the USB adapter in?\n")
        return None
    print("Available serial ports:")
    for i, p in enumerate(ports):
        desc = (p.description or "").strip() or "(no description)"
        print(f"  [{i}] {p.device}    {desc}")
    raw = input("Select [number or full path]: ").strip()
    if not raw:
        return None
    if raw.isdigit():
        idx = int(raw)
        if 0 <= idx < len(ports):
            return ports[idx].device
        sys.stderr.write(f"Index {idx} out of range.\n")
        return None
    return raw


# --- Main ----------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="Serial device (e.g. /dev/cu.usbserial-XXXX). Prompts if omitted.")
    ap.add_argument("--baud", type=int, default=31250, help="Baud rate (default 31250, standard MIDI)")
    args = ap.parse_args()

    port = args.port or pick_port_interactive()
    if not port:
        return 1

    try:
        ser = serial.Serial(port, args.baud, timeout=0.5)
    except serial.SerialException as e:
        sys.stderr.write(f"Failed to open {port}: {e}\n")
        return 1

    print(f"Listening on {port} @ {args.baud} baud. Ctrl-C to stop.\n")
    parser = MidiParser()
    try:
        while True:
            data = ser.read(64)
            if not data:
                continue
            stamp = time.strftime("%H:%M:%S")
            for b in data:
                for msg in parser.feed(b):
                    print(f"[{stamp}] {msg}")
    except KeyboardInterrupt:
        print("\nBye.")
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
