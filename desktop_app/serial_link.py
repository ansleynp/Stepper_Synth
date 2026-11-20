"""Single-owner USB worker. JSON lines for the desktop/firmware interface."""
import base64
import json
import queue
import threading
import time
import uuid

import serial
from serial.tools import list_ports

MAX_FILE = 256 * 1024
CHUNK = 384


def available_ports():
    return [p.device for p in list_ports.comports()]


class SerialLink(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.jobs = queue.Queue()
        self.events = queue.Queue()
        self.stop_event = threading.Event()
        self.port = None
        self.rx = bytearray()
        self.start()

    def frame(self):
        self.rx.extend(self.port.read(self.port.in_waiting or 1))
        if len(self.rx) > 65536:
            raise ValueError("Serial input exceeded the frame limit.")
        while b'\n' in self.rx:
            raw, _, rest = self.rx.partition(b'\n')
            self.rx[:] = rest
            try:
                data = json.loads(raw)
            except (ValueError, UnicodeError):
                continue  # ESP-IDF boot logs can share this port.
            if isinstance(data, dict) and data.get("v") == 1:
                yield data

    def forward(self, msg):
        """Pass unsolicited firmware events (voices, transport) to the GUI."""
        if isinstance(msg.get("event"), str):
            self.events.put(("event", msg))
            return True
        return False

    def request(self, op, **fields):
        token = uuid.uuid4().hex[:12]
        self.port.write((json.dumps(dict(v=1, id=token, op=op, **fields),
                                    separators=(',', ':')) + '\n').encode())
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and not self.stop_event.is_set():
            for msg in self.frame():
                if self.forward(msg):
                    continue
                if msg.get("id") == token:
                    if msg.get("ok") is not True:
                        raise ValueError(str(msg.get("error", "Request declined")))
                    return msg
        raise TimeoutError("No firmware acknowledgement. Check firmware and reconnect.")

    def execute(self, op, arg):
        if op == "open":
            self.port = serial.Serial(timeout=.025, write_timeout=1)
            self.port.port, self.port.baudrate = arg, 115200
            # Keep DTR/RTS released so the dev board's auto-reset circuit doesn't
            # reboot the ESP32 (which would wipe the take stored in SRAM).
            self.port.dtr = self.port.rts = False
            self.port.open()
            self.rx.clear()
            self.events.put(("opened", arg))
            self.stop_event.wait(1.5)  # Some dev boards reset on port open.
            return self.request("hello")
        if op == "map":
            return self.request("set_map", **arg)
        if op == "save":
            reply = self.request("read_begin")
            size, take = reply.get("size"), reply.get("take")
            if type(size) != int or not 0 < size <= MAX_FILE or not isinstance(take, str):
                raise ValueError("Invalid recording header.")
            data = bytearray()
            while len(data) < size:
                length = min(CHUNK, size - len(data))
                part = self.request("read_chunk", take=take, offset=len(data), length=length)
                block = base64.b64decode(part["data"], validate=True)
                if len(block) != length:
                    raise ValueError("Incomplete recording chunk.")
                data.extend(block)
            return bytes(data)
        if op == "load":
            take = self.request("write_begin", size=len(arg))["take"]
            for offset in range(0, len(arg), CHUNK):
                block = base64.b64encode(arg[offset:offset+CHUNK]).decode()
                self.request("write_chunk", take=take, offset=offset, data=block)
            return self.request("write_commit", take=take)
        return self.request(op)

    def run(self):
        next_ping = float('inf')
        try:
            while not self.stop_event.is_set():
                try:
                    op, arg = self.jobs.get(timeout=.02)
                except queue.Empty:
                    if self.port and (self.port.in_waiting or b'\n' in self.rx):
                        for msg in self.frame():
                            self.forward(msg)
                    if time.monotonic() >= next_ping:
                        self.request("ping")
                        next_ping = time.monotonic() + 1
                    continue
                try:
                    result = self.execute(op, arg)
                    if op in ("map", "identify"):
                        next_ping = time.monotonic() + 1
                    elif op == "ordinary":
                        next_ping = float('inf')
                    self.events.put((op, result))
                except ValueError as exc:
                    self.events.put(("error", str(exc)))
        except (OSError, serial.SerialException, TimeoutError, KeyError, TypeError, ValueError) as exc:
            self.events.put(("lost", str(exc)))
        finally:
            if self.port:
                self.port.close()
