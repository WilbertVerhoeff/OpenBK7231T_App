"""Persistent push connection to an OpenBeken device."""

from __future__ import annotations

import asyncio
import json
import logging
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .const import DOMAIN, PROTOCOL_VERSION

_LOGGER = logging.getLogger(__name__)
# Entity metadata and full snapshots can contain many channels.
MAX_LINE = 8192
CONNECT_TIMEOUT = 15


async def _read_json_line(reader: asyncio.StreamReader, timeout: float = 75) -> dict[str, Any]:
    raw = await asyncio.wait_for(reader.readline(), timeout=timeout)
    if not raw:
        raise ConnectionError("Device closed the connection")
    if len(raw) > MAX_LINE or not raw.endswith(b"\n"):
        raise ValueError("Invalid or oversized OpenBeken packet")
    data = json.loads(raw)
    if not isinstance(data, dict):
        raise ValueError("Expected a JSON object")
    return data


async def async_probe_device(host: str, port: int) -> dict[str, Any]:
    reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=5)
    try:
        hello = await _read_json_line(reader, timeout=8)
        if hello.get("type") != "hello" or hello.get("protocol") != PROTOCOL_VERSION or not hello.get("device_id"):
            raise ValueError("The device does not speak OpenBeken API protocol 1")
        writer.write(b'{"type":"hello","protocol":1,"client":"home-assistant"}\n')
        await writer.drain()
        return hello
    finally:
        writer.close()
        await writer.wait_closed()


class OpenBekenCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Own the connection, protocol state and reconnect lifecycle."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, host: str, port: int, name: str, device_id: str) -> None:
        super().__init__(hass, _LOGGER, name=f"{DOMAIN} {name}", update_interval=None)
        self.host = host
        self.config_entry = entry
        self.port = port
        self.device_name = name
        self.device_id = device_id
        self.firmware: str | None = None
        self.entities: dict[str, dict[str, Any]] = {}
        self.states: dict[str, dict[str, Any]] = {}
        self._seq: int | None = None
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._runner: asyncio.Task | None = None
        self._stop = asyncio.Event()
        self._connected = False
        self._next_id = 1
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}

    async def _async_update_data(self) -> dict[str, Any]:
        await self._connect_once()
        return self.data or {"entities": self.entities, "states": self.states}

    async def _connect_once(self) -> None:
        try:
            async with asyncio.timeout(CONNECT_TIMEOUT):
                await self._handshake()
            self._connected = True
            self._async_update_device_info()
            self.async_set_updated_data({"entities": self.entities, "states": self.states})
            self._runner = self.config_entry.async_create_background_task(
                self.hass,
                self._reader_loop(),
                f"{DOMAIN} device reader",
            )
        except asyncio.CancelledError:
            await self._close_connection()
            raise
        except (OSError, asyncio.TimeoutError, ConnectionError, ValueError, json.JSONDecodeError, UpdateFailed) as err:
            await self._close_connection()
            raise UpdateFailed(f"Could not connect to OpenBeken device: {err}") from err

    async def _handshake(self) -> None:
        """Finish the entire initial exchange within one overall deadline."""
        self._reader, self._writer = await asyncio.wait_for(asyncio.open_connection(self.host, self.port), timeout=5)
        hello = await _read_json_line(self._reader, timeout=8)
        if hello.get("type") != "hello" or hello.get("protocol") != PROTOCOL_VERSION or hello.get("device_id", "").replace(":", "").replace("-", "").lower() != self.device_id.replace(":", "").replace("-", "").lower():
            raise ValueError("OpenBeken device identity or protocol changed")
        self.firmware = hello.get("firmware")
        self._writer.write(b'{"type":"hello","protocol":1,"client":"home-assistant"}\n')
        await asyncio.wait_for(self._writer.drain(), timeout=5)
        self.entities = {}
        self.states = {}
        self._seq = None
        got_entities = False
        while True:
            message = await _read_json_line(self._reader, timeout=8)
            got_entities |= message.get("type") == "entities"
            self._handle_message(message)
            if got_entities and message.get("type") == "state" and message.get("full"):
                break

    @callback
    def _async_update_device_info(self) -> None:
        """Refresh an existing device's metadata after a successful connection."""
        if not isinstance(self.firmware, str) or not self.firmware:
            return
        registry = dr.async_get(self.hass)
        device = registry.async_get_device_by_identifier(
            (DOMAIN, self.device_id.replace(":", "").replace("-", "").lower()),
            self.config_entry.entry_id,
        )
        if device is not None:
            registry.async_update_device(
                device.id,
                sw_version=self.firmware,
                name=self.device_name,
                configuration_url=f"http://{self.host}",
            )

    async def _reader_loop(self) -> None:
        try:
            assert self._reader is not None
            while not self._stop.is_set():
                message = await _read_json_line(self._reader)
                self._handle_message(message)
        except (OSError, asyncio.TimeoutError, ConnectionError, ValueError, json.JSONDecodeError, UpdateFailed) as err:
            if not self._stop.is_set():
                _LOGGER.debug("OpenBeken connection lost (%s); reconnecting", err)
                self._connected = False
                self.async_update_listeners()
                await self._close_connection(from_reader=True)
                self.config_entry.async_create_background_task(
                    self.hass,
                    self._reconnect_loop(),
                    f"{DOMAIN} device reconnect",
                )

    async def _reconnect_loop(self) -> None:
        while not self._stop.is_set():
            try:
                await self._connect_once()
                return
            except UpdateFailed:
                await asyncio.sleep(5)

    @callback
    def _handle_message(self, message: dict[str, Any]) -> None:
        msg_type = message.get("type")
        if msg_type == "entities":
            items = message.get("entities", [])
            if isinstance(items, list):
                device = message.get("device")
                if isinstance(device, dict):
                    if isinstance(device.get("name"), str) and device["name"]:
                        self.device_name = device["name"]
                    if isinstance(device.get("firmware"), str) and device["firmware"]:
                        self.firmware = device["firmware"]
                    self._async_update_device_info()
                self.entities = {item["id"]: dict(item) for item in items if isinstance(item, dict) and isinstance(item.get("id"), str)}
                self.states = {key: value for key, value in self.states.items() if key in self.entities}
                self.async_set_updated_data({"entities": self.entities, "states": self.states})
        elif msg_type == "state" and isinstance(message.get("entities"), dict):
            self.states = {key: dict(value) for key, value in message["entities"].items() if isinstance(value, dict)}
            self._seq = message.get("seq")
            self.async_set_updated_data({"entities": self.entities, "states": self.states})
        elif msg_type == "state_changed" and isinstance(message.get("state"), dict):
            seq = message.get("seq")
            if self._seq is not None and isinstance(seq, int) and seq != self._seq + 1:
                self.hass.async_create_task(self.async_request_state())
            if isinstance(seq, int):
                self._seq = seq
            entity = message.get("entity")
            if isinstance(entity, str):
                self.states[entity] = dict(message["state"])
                self.async_set_updated_data({"entities": self.entities, "states": self.states})
        elif msg_type == "result" and isinstance(message.get("id"), int):
            future = self._pending.pop(message["id"], None)
            if future and not future.done():
                future.set_result(message)
        elif msg_type == "ping":
            self.hass.async_create_task(self.async_send_heartbeat())

    async def _send(self, message: dict[str, Any]) -> None:
        if self._writer is None or self._writer.is_closing():
            raise ConnectionError("OpenBeken device is disconnected")
        self._writer.write(json.dumps(message, separators=(",", ":")).encode() + b"\n")
        await asyncio.wait_for(self._writer.drain(), timeout=5)

    async def async_request_state(self) -> None:
        if self._connected:
            await self._send({"type": "get_state"})

    async def async_send_heartbeat(self) -> None:
        if self._connected:
            await self._send({"type": "ping"})

    @property
    def connected(self) -> bool:
        return self._connected

    async def async_set_state(self, entity_id: str, state: dict[str, Any]) -> None:
        request_id = self._next_id
        self._next_id += 1
        future = self.hass.loop.create_future()
        self._pending[request_id] = future
        await self._send({"type": "set_state", "id": request_id, "entity": entity_id, "state": state})
        try:
            result = await asyncio.wait_for(future, timeout=5)
        finally:
            self._pending.pop(request_id, None)
        if not result.get("success"):
            raise ValueError(f"OpenBeken rejected the command: {result.get('error', 'unknown error')}")
        await self.async_request_state()

    async def async_restart(self) -> None:
        request_id = self._next_id
        self._next_id += 1
        future = self.hass.loop.create_future()
        self._pending[request_id] = future
        await self._send({"type": "restart", "id": request_id})
        try:
            result = await asyncio.wait_for(future, timeout=5)
        finally:
            self._pending.pop(request_id, None)
        if not result.get("success"):
            raise ValueError("OpenBeken rejected the restart request")

    async def _close_connection(self, from_reader: bool = False) -> None:
        writer, self._writer = self._writer, None
        self._reader = None
        self._connected = False
        for future in self._pending.values():
            if not future.done():
                future.set_exception(ConnectionError("OpenBeken disconnected"))
        self._pending.clear()
        if writer:
            writer.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), timeout=2)
            except (OSError, asyncio.TimeoutError):
                pass
        if not from_reader and self._runner and self._runner is not asyncio.current_task():
            self._runner.cancel()
            self._runner = None

    async def async_shutdown(self) -> None:
        self._stop.set()
        await self._close_connection()
        if self._runner:
            self._runner.cancel()
            self._runner = None
