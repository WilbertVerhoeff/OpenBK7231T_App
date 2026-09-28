"""OpenBeken binary sensor platform."""

from homeassistant.components.binary_sensor import BinarySensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .coordinator import OpenBekenCoordinator
from .entity import OpenBekenEntity, async_setup_dynamic_platform


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: OpenBekenCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_setup_dynamic_platform(coordinator, entry, "binary_sensor", OpenBekenBinarySensor, async_add_entities)


class OpenBekenBinarySensor(OpenBekenEntity, BinarySensorEntity):
    """Movement or contact state from an OpenBeken channel."""

    def _apply_description(self, entity: dict) -> None:
        super()._apply_description(entity)
        self._attr_device_class = entity.get("device_class") or None

    @property
    def is_on(self) -> bool | None:
        return self.obk_state.get("on")
