"""Config flow for OpenBeken devices."""

from __future__ import annotations

import asyncio
import logging
from typing import Any

import voluptuous as vol
from homeassistant import config_entries
from homeassistant.const import CONF_HOST, CONF_NAME
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResult
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .const import DEFAULT_PORT, DOMAIN, PROTOCOL_VERSION
from .coordinator import async_probe_device

_LOGGER = logging.getLogger(__name__)


class OpenBekenConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Add OpenBeken by discovery or host address."""

    VERSION = 1

    def __init__(self) -> None:
        self._discovery_info: dict[str, Any] | None = None

    async def _async_add_device(self, host: str, port: int, name: str, device_id: str) -> FlowResult:
        await self.async_set_unique_id(device_id.replace(":", "").replace("-", "").lower())
        self._abort_if_unique_id_configured({CONF_HOST: host, "port": port})
        return self.async_create_entry(
            title=name or device_id,
            data={CONF_HOST: host, "port": port, CONF_NAME: name or device_id, "device_id": device_id},
        )

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo) -> FlowResult:
        properties = discovery_info.properties
        if properties.get("api") != str(PROTOCOL_VERSION):
            return self.async_abort(reason="unsupported_protocol")
        device_id = properties.get("id")
        if not device_id:
            return self.async_abort(reason="no_device_id")
        await self.async_set_unique_id(device_id.replace(":", "").replace("-", "").lower())
        self._abort_if_unique_id_configured({CONF_HOST: discovery_info.host})
        self._discovery_info = {
            CONF_HOST: discovery_info.host,
            "port": discovery_info.port or DEFAULT_PORT,
            CONF_NAME: properties.get("name", discovery_info.name.removesuffix("._openbeken._tcp.local.")),
            "device_id": device_id,
        }
        self.context["title_placeholders"] = {"name": self._discovery_info[CONF_NAME]}
        return await self.async_step_discovery_confirm()

    async def async_step_discovery_confirm(self, user_input: dict[str, Any] | None = None) -> FlowResult:
        if user_input is not None:
            info = self._discovery_info
            assert info is not None
            return await self._async_add_device(info[CONF_HOST], info["port"], info[CONF_NAME], info["device_id"])
        return self.async_show_form(step_id="discovery_confirm", description_placeholders={"name": self.context["title_placeholders"]["name"]})

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> FlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                hello = await async_probe_device(user_input[CONF_HOST], user_input["port"])
                device_id = hello["device_id"]
                return await self._async_add_device(user_input[CONF_HOST], user_input["port"], hello.get("name", "OpenBeken"), device_id)
            except (OSError, asyncio.TimeoutError, ValueError, KeyError) as err:
                _LOGGER.debug("Could not connect to OpenBeken device: %s", err)
                errors["base"] = "cannot_connect"
        schema = vol.Schema({vol.Required(CONF_HOST): str, vol.Optional("port", default=DEFAULT_PORT): vol.All(int, vol.Range(min=1, max=65535))})
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)
