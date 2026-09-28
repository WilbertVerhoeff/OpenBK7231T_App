"""OpenBeken relay platform."""

from homeassistant.components.switch import SwitchEntity
from homeassistant.core import HomeAssistant
from homeassistant.config_entries import ConfigEntry
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .entity import OpenBekenEntity
from .coordinator import OpenBekenCoordinator


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: OpenBekenCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(OpenBekenSwitch(coordinator, item) for item in coordinator.entities.values() if item.get("platform") == "switch")


class OpenBekenSwitch(OpenBekenEntity, SwitchEntity):
    """OpenBeken relay switch."""

    @property
    def is_on(self) -> bool | None:
        return self.obk_state.get("on")

    async def async_turn_on(self, **kwargs) -> None:
        await self.coordinator.async_set_state(self.obk_id, {"on": True})

    async def async_turn_off(self, **kwargs) -> None:
        await self.coordinator.async_set_state(self.obk_id, {"on": False})
