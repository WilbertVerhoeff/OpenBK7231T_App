"""Shared OpenBeken entity helpers."""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import OpenBekenCoordinator


class OpenBekenEntity(CoordinatorEntity[OpenBekenCoordinator]):
    """Base class with stable device and entity identifiers."""

    _attr_has_entity_name = True

    def __init__(self, coordinator: OpenBekenCoordinator, entity: dict) -> None:
        super().__init__(coordinator)
        self.obk_id = entity["id"]
        self._attr_unique_id = f"{coordinator.device_id.replace(':', '').replace('-', '').lower()}_{self.obk_id}"
        self._attr_name = entity.get("name") or self.obk_id

    @property
    def device_info(self) -> DeviceInfo:
        """Use current metadata, including firmware learned on reconnect."""
        return DeviceInfo(
            identifiers={(DOMAIN, self.coordinator.device_id.replace(":", "").replace("-", "").lower())},
            name=self.coordinator.device_name,
            manufacturer="OpenBeken",
            model="OpenBeken device",
            sw_version=self.coordinator.firmware,
            configuration_url=f"http://{self.coordinator.host}",
        )

    @property
    def obk_state(self) -> dict:
        return self.coordinator.states.get(self.obk_id, {})

    @property
    def available(self) -> bool:
        return self.coordinator.connected and self.obk_id in self.coordinator.states
