"""OpenBeken device buttons."""

from homeassistant.components.button import ButtonEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .coordinator import OpenBekenCoordinator
from .entity import OpenBekenEntity, async_setup_dynamic_platform


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    coordinator: OpenBekenCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_setup_dynamic_platform(coordinator, entry, "button", OpenBekenRestartButton, async_add_entities)


class OpenBekenRestartButton(OpenBekenEntity, ButtonEntity):
    """Button that restarts the OpenBeken device."""

    @property
    def available(self) -> bool:
        return self.coordinator.connected and self.description_available

    async def async_press(self) -> None:
        await self.coordinator.async_restart()
