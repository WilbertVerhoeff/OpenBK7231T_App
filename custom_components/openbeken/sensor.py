"""OpenBeken read-only sensor platform."""

from homeassistant.components.sensor import SensorEntity
from homeassistant.core import HomeAssistant
from homeassistant.config_entries import ConfigEntry
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .entity import OpenBekenEntity, async_setup_dynamic_platform
from .coordinator import OpenBekenCoordinator


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: OpenBekenCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_setup_dynamic_platform(coordinator, entry, "sensor", OpenBekenSensor, async_add_entities)


class OpenBekenSensor(OpenBekenEntity, SensorEntity):
    """Sensor reported by a firmware version that supports sensor entities."""

    def _apply_description(self, entity: dict) -> None:
        super()._apply_description(entity)
        self._attr_native_unit_of_measurement = entity.get("unit") or None
        self._attr_device_class = entity.get("device_class") or None
        self._attr_state_class = entity.get("state_class")

    @property
    def native_value(self):
        return self.obk_state.get("value")
