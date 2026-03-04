"""Serial bridge: manages serial port lifecycle and WebSocket broadcasting."""

import asyncio
import json
import logging
import threading
from typing import Any

import serial
import serial.tools.list_ports

logger = logging.getLogger(__name__)

ESP32_S3_VID = 0x303A


class SerialBridge:
    """Manages a serial connection and broadcasts messages to WebSocket clients."""

    def __init__(self) -> None:
        self._serial: serial.Serial | None = None
        self._port: str = ""
        self._lock = threading.Lock()
        self._reader_thread: threading.Thread | None = None
        self._running = False
        self._clients: set[Any] = set()
        self._loop: asyncio.AbstractEventLoop | None = None

    # -- Port enumeration --------------------------------------------------

    @staticmethod
    def list_ports() -> list[dict]:
        """Return available serial ports with ESP32-S3 detection."""
        ports = []
        for p in serial.tools.list_ports.comports():
            ports.append(
                {
                    "port": p.device,
                    "description": p.description,
                    "is_esp32": (p.vid == ESP32_S3_VID) if p.vid else False,
                }
            )
        return ports

    # -- Connection management ---------------------------------------------

    def connect(self, port: str, baud: int = 115200) -> dict:
        """Open serial connection and start background reader."""
        with self._lock:
            if self._serial and self._serial.is_open:
                return {"ok": False, "error": "Already connected"}
            try:
                self._serial = serial.Serial(port, baud, timeout=1)
                self._port = port
            except serial.SerialException as e:
                return {"ok": False, "error": str(e)}

        self._running = True
        self._reader_thread = threading.Thread(
            target=self._reader_loop, daemon=True
        )
        self._reader_thread.start()
        self._broadcast_sync({"type": "status", "data": self.status()})
        logger.info("Connected to %s @ %d", port, baud)
        return {"ok": True}

    def disconnect(self) -> dict:
        """Close serial connection and stop reader thread."""
        self._running = False
        with self._lock:
            if self._serial and self._serial.is_open:
                try:
                    self._serial.close()
                except Exception:
                    pass
                self._serial = None
                self._port = ""
        if self._reader_thread:
            self._reader_thread.join(timeout=3)
            self._reader_thread = None
        self._broadcast_sync({"type": "status", "data": self.status()})
        logger.info("Disconnected")
        return {"ok": True}

    def status(self) -> dict:
        """Return current connection status."""
        with self._lock:
            connected = self._serial is not None and self._serial.is_open
        return {"connected": connected, "port": self._port if connected else ""}

    @property
    def connected(self) -> bool:
        with self._lock:
            return self._serial is not None and self._serial.is_open

    # -- Serial I/O --------------------------------------------------------

    def send_to_serial(self, msg: str) -> bool:
        """Write a protocol message to the serial port (adds @@ prefix)."""
        with self._lock:
            if not self._serial or not self._serial.is_open:
                return False
            try:
                line = f"@@{msg}\n"
                self._serial.write(line.encode("utf-8"))
                self._serial.flush()
                return True
            except serial.SerialException as e:
                logger.error("Serial write error: %s", e)
                return False

    def _reader_loop(self) -> None:
        """Background thread: read lines from serial, classify, broadcast."""
        while self._running:
            with self._lock:
                ser = self._serial
            if not ser or not ser.is_open:
                break
            try:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                if not line:
                    continue

                if line.startswith("@@"):
                    # Protocol message
                    payload = line[2:]
                    try:
                        data = json.loads(payload)
                    except json.JSONDecodeError:
                        data = payload
                    self._broadcast_sync({"type": "response", "data": data})
                else:
                    # Debug log
                    self._broadcast_sync({"type": "log", "data": line})

            except serial.SerialException as e:
                logger.error("Serial read error: %s", e)
                break
            except Exception as e:
                logger.error("Reader error: %s", e)
                continue

        # If we exit the loop unexpectedly, clean up
        if self._running:
            self._running = False
            with self._lock:
                if self._serial and self._serial.is_open:
                    try:
                        self._serial.close()
                    except Exception:
                        pass
                    self._serial = None
                    self._port = ""
            self._broadcast_sync({"type": "status", "data": self.status()})

    # -- WebSocket client management ---------------------------------------

    def set_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        """Set the asyncio event loop for broadcasting."""
        self._loop = loop

    def add_client(self, ws: Any) -> None:
        self._clients.add(ws)

    def remove_client(self, ws: Any) -> None:
        self._clients.discard(ws)

    def _broadcast_sync(self, msg: dict) -> None:
        """Thread-safe broadcast to all WebSocket clients."""
        text = json.dumps(msg)
        if self._loop is None:
            return
        for ws in list(self._clients):
            try:
                asyncio.run_coroutine_threadsafe(
                    ws.send_text(text), self._loop
                )
            except Exception:
                self._clients.discard(ws)


# Module-level singleton
bridge = SerialBridge()
