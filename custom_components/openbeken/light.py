"""OpenBeken light platform."""

from __future__ import annotations

from typing import Any

from homeassistant.components.light import (
    ATTR_COLOR_TEMP_KELVIN,
    ATTR_EFFECT,
    ATTR_RGB_COLOR,
    ATTR_WHITE,
    DEFAULT_MAX_KELVIN,
    DEFAULT_MIN_KELVIN,
    EFFECT_OFF,
    ColorMode,
    LightEntity,
    LightEntityFeature,
)
from homeassistant.components.light import ATTR_BRIGHTNESS
from homeassistant.core import HomeAssistant
from homeassistant.config_entries import ConfigEntry
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .entity import OpenBekenEntity, async_setup_dynamic_platform
from .coordinator import OpenBekenCoordinator


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: OpenBekenCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_setup_dynamic_platform(coordinator, entry, "light", OpenBekenLight, async_add_entities)


class OpenBekenLight(OpenBekenEntity, LightEntity):
    """One OpenBeken logical light with WLED-like effect selection."""

    def _apply_description(self, entity: dict) -> None:
        super()._apply_description(entity)
        self.features = set(entity.get("features", []))
        modes = set()
        if "rgb" in self.features:
            modes.add(ColorMode.RGB)
        if "color_temp" in self.features:
            modes.add(ColorMode.COLOR_TEMP)
        elif "white_level" in self.features and "rgb" in self.features:
            modes.add(ColorMode.WHITE)
        if not modes:
            modes.add(ColorMode.BRIGHTNESS)
        self._attr_supported_color_modes = modes
        self._attr_effect_list = [EFFECT_OFF, *entity.get("effects", [])] if "effects" in self.features else None
        self._attr_supported_features = LightEntityFeature.EFFECT if "effects" in self.features else 0
        if "color_temp" in self.features:
            self._attr_min_color_temp_kelvin = round(1000000 / entity["max_mireds"])
            self._attr_max_color_temp_kelvin = round(1000000 / entity["min_mireds"])
        else:
            self._attr_min_color_temp_kelvin = DEFAULT_MIN_KELVIN
            self._attr_max_color_temp_kelvin = DEFAULT_MAX_KELVIN

    @property
    def is_on(self) -> bool | None:
        return self.obk_state.get("on")

    @property
    def brightness(self) -> int | None:
        return self.obk_state.get("brightness")

    @property
    def rgb_color(self) -> tuple[int, int, int] | None:
        rgb = self.obk_state.get("rgb")
        return tuple(rgb) if isinstance(rgb, list) and len(rgb) == 3 else None

    @property
    def color_temp_kelvin(self) -> int | None:
        mireds = self.obk_state.get("color_temp")
        return round(1000000 / mireds) if mireds else None

    @property
    def effect(self) -> str | None:
        return self.obk_state.get("effect") or EFFECT_OFF if "effects" in self.features else None

    @property
    def color_mode(self) -> ColorMode:
        mode = self.obk_state.get("mode")
        if mode == "effect":
            return ColorMode.RGB if "rgb" in self.features else next(iter(self._attr_supported_color_modes))
        if mode == "white" and "color_temp" in self.features:
            return ColorMode.COLOR_TEMP
        if mode == "white" and "white_level" in self.features and "rgb" in self.features:
            return ColorMode.WHITE
        if mode == "rgb" and "rgb" in self.features:
            return ColorMode.RGB
        return next(iter(self._attr_supported_color_modes))

    async def async_turn_on(self, **kwargs: Any) -> None:
        state: dict[str, Any] = {"on": True}
        if ATTR_BRIGHTNESS in kwargs and "brightness" in self.features:
            state["brightness"] = kwargs[ATTR_BRIGHTNESS]
        if ATTR_RGB_COLOR in kwargs and "rgb" in self.features:
            state["rgb"] = list(kwargs[ATTR_RGB_COLOR])
            state["mode"] = "rgb"
        if ATTR_WHITE in kwargs and "white_level" in self.features:
            state.pop("brightness", None)
            state["white_level"] = kwargs[ATTR_WHITE]
            state["mode"] = "white"
        if ATTR_COLOR_TEMP_KELVIN in kwargs and "color_temp" in self.features:
            state["color_temp"] = round(1000000 / kwargs[ATTR_COLOR_TEMP_KELVIN])
            state["mode"] = "white"
        if ATTR_EFFECT in kwargs and "effects" in self.features:
            if kwargs[ATTR_EFFECT] == EFFECT_OFF:
                state["mode"] = "rgb" if "rgb" in self.features else "white"
            else:
                state["effect"] = kwargs[ATTR_EFFECT]
                state["mode"] = "effect"
        await self.coordinator.async_set_state(self.obk_id, state)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self.coordinator.async_set_state(self.obk_id, {"on": False})
