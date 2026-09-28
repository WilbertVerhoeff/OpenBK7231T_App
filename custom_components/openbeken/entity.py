"""Shared OpenBeken entity helpers."""

from __future__ import annotations

from collections.abc import Callable

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import OpenBekenCoordinator


@callback
def async_setup_dynamic_platform(
    coordinator: OpenBekenCoordinator,
    entry: ConfigEntry,
    platform: str,
    factory: Callable,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Add newly advertised entities, keeping existing IDs across disappearance."""
    known_ids: set[str] = set()

    @callback
    def update_entities() -> None:
        added = []
        for entity_id, description in coordinator.entities.items():
            if description.get("platform") != platform or entity_id in known_ids:
                continue
            added.append(factory(coordinator, description))
            known_ids.add(entity_id)
        if added:
            async_add_entities(added)

    entry.async_on_unload(coordinator.async_add_listener(update_entities))
    update_entities()


class OpenBekenEntity(CoordinatorEntity[OpenBekenCoordinator]):
    """Base class with stable device and entity identifiers."""

    _attr_has_entity_name = True

    def __init__(self, coordinator: OpenBekenCoordinator, entity: dict) -> None:
        super().__init__(coordinator)
        self.obk_id = entity["id"]
        self.obk_platform = entity["platform"]
        self._attr_unique_id = f"{coordinator.device_id.replace(':', '').replace('-', '').lower()}_{self.obk_id}"
        self._apply_description(entity)

    def _apply_description(self, entity: dict) -> None:
        """Update entity metadata without replacing the entity or its unique ID."""
        self._description = dict(entity)
        self._attr_name = entity.get("name") or self.obk_id

    @callback
    def _handle_coordinator_update(self) -> None:
        description = self.coordinator.entities.get(self.obk_id)
        if description and description.get("platform") == self.obk_platform and description != self._description:
            self._apply_description(description)
        super()._handle_coordinator_update()

    @property
    def description_available(self) -> bool:
        description = self.coordinator.entities.get(self.obk_id)
        return bool(description and description.get("platform") == self.obk_platform)

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
        return self.coordinator.connected and self.description_available and self.obk_id in self.coordinator.states
