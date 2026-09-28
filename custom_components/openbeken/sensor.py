"""OpenBeken read-only sensor platform."""

from homeassistant.components.sensor import SensorEntity
from homeassistant.core import HomeAssistant
from homeassistant.config_entries import ConfigEntry
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .entity import OpenBekenEntity
from .coordinator import OpenBekenCoordinator


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: OpenBekenCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(OpenBekenSensor(coordinator, item) for item in coordinator.entities.values() if item.get("platform") == "sensor")


class OpenBekenSensor(OpenBekenEntity, SensorEntity):
    """Sensor reported by a firmware version that supports sensor entities."""

    def __init__(self, coordinator: OpenBekenCoordinator, entity: dict) -> None:
        super().__init__(coordinator, entity)
        self._attr_native_unit_of_measurement = entity.get("unit") or None
        self._attr_device_class = entity.get("device_class") or None
        self._attr_state_class = entity.get("state_class")

    @property
    def native_value(self):
        return self.obk_state.get("value")
