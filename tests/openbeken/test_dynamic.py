"""Regression tests for native API lifecycle, using isolated HA test doubles.

Run with: python -m unittest discover -s tests/openbeken -v
No running Home Assistant or physical device is required.
"""
import asyncio
from enum import Enum, IntFlag
import importlib
import json
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, Mock

ROOT = Path(__file__).resolve().parents[2]


def module(name, **attrs):
    item = types.ModuleType(name)
    item.__dict__.update(attrs)
    sys.modules[name] = item
    return item


class Coordinator:
    def __class_getitem__(cls, key):
        return cls

    def __init__(self, hass, logger, **kwargs):
        self.hass = hass
        self.data = None
        self.listeners = []

    def async_add_listener(self, listener):
        self.listeners.append(listener)
        return lambda: self.listeners.remove(listener)

    def async_update_listeners(self):
        for listener in list(self.listeners):
            listener()

    def async_set_updated_data(self, data):
        self.data = data
        self.async_update_listeners()


class Entity:
    def __class_getitem__(cls, key):
        return cls

    def __init__(self, coordinator):
        self.coordinator = coordinator

    def _handle_coordinator_update(self):
        pass


class ColorMode(str, Enum):
    RGB = 'rgb'
    WHITE = 'white'
    COLOR_TEMP = 'color_temp'
    BRIGHTNESS = 'brightness'


class LightFeature(IntFlag):
    EFFECT = 4


class LightBase:
    @property
    def supported_color_modes(self):
        return self._attr_supported_color_modes

    @property
    def effect_list(self):
        return self._attr_effect_list


class UpdateFailed(Exception):
    pass


# Import the real integration with only the HA boundary replaced.
module('homeassistant')
module('homeassistant.config_entries', ConfigEntry=object)
module('homeassistant.core', HomeAssistant=object, callback=lambda func: func)
module('homeassistant.const', Platform=types.SimpleNamespace(
    LIGHT='light', SWITCH='switch', SENSOR='sensor',
    BINARY_SENSOR='binary_sensor', BUTTON='button'))
helpers = module('homeassistant.helpers')
registry_api = module('homeassistant.helpers.device_registry', DeviceInfo=dict)
helpers.device_registry = registry_api
module('homeassistant.helpers.entity_platform', AddEntitiesCallback=object)
module('homeassistant.helpers.update_coordinator',
       DataUpdateCoordinator=Coordinator, CoordinatorEntity=Entity, UpdateFailed=UpdateFailed)
module('homeassistant.components')
module('homeassistant.components.light',
       LightEntity=LightBase, LightEntityFeature=LightFeature, ColorMode=ColorMode,
       ATTR_COLOR_TEMP_KELVIN='color_temp_kelvin', ATTR_EFFECT='effect',
       ATTR_RGB_COLOR='rgb_color', ATTR_WHITE='white', ATTR_BRIGHTNESS='brightness',
       DEFAULT_MIN_KELVIN=2000, DEFAULT_MAX_KELVIN=6535, EFFECT_OFF='Off')
for platform, entity_class in [('sensor', 'SensorEntity'), ('binary_sensor', 'BinarySensorEntity'),
                               ('switch', 'SwitchEntity'), ('button', 'ButtonEntity')]:
    module(f'homeassistant.components.{platform}', **{entity_class: type(entity_class, (), {})})
package = module('openbeken_dynamic_test')
package.__path__ = [str(ROOT / 'custom_components' / 'openbeken')]
coordinator_module = importlib.import_module('openbeken_dynamic_test.coordinator')
entity_module = importlib.import_module('openbeken_dynamic_test.entity')
light_module = importlib.import_module('openbeken_dynamic_test.light')
sensor_module = importlib.import_module('openbeken_dynamic_test.sensor')
button_module = importlib.import_module('openbeken_dynamic_test.button')
switch_module = importlib.import_module('openbeken_dynamic_test.switch')
binary_module = importlib.import_module('openbeken_dynamic_test.binary_sensor')


def light(features=('brightness',), **extra):
    return dict(id='light_0', platform='light', name='Light', features=list(features), **extra)


class DynamicEntities(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.registry = Mock()
        self.registry.async_get_device_by_identifier.return_value = types.SimpleNamespace(id='existing-device')
        registry_api.async_get = Mock(return_value=self.registry)
        self.entry = Mock()
        self.entry.async_create_background_task.side_effect = lambda hass, coro, name: asyncio.create_task(coro)
        self.hass = types.SimpleNamespace(loop=None)
        self.coordinator = coordinator_module.OpenBekenCoordinator(
            self.hass, self.entry, 'device.local', 6054, 'OpenBeken', 'AA:BB:CC:DD:EE:FF')
        self.coordinator.firmware = 'old-build'
        self.coordinator._connected = True
        self.added = []

    def descriptions(self, *items, **extra):
        self.coordinator._handle_message(dict(type='entities', entities=list(items), **extra))

    def snapshot(self, states):
        self.coordinator._handle_message(dict(type='state', full=True, seq=1, entities=states))

    def register(self, platform, factory):
        def add(entities):
            self.added.extend(entities)
            for entity in entities:
                self.coordinator.async_add_listener(entity._handle_coordinator_update)
        entity_module.async_setup_dynamic_platform(self.coordinator, self.entry, platform, factory, add)

    def test_new_entities_are_added_once_during_state_updates(self):
        self.register('light', light_module.OpenBekenLight)
        self.descriptions(light())
        entity = self.added[0]
        for brightness in range(20):
            self.snapshot({'light_0': {'on': True, 'brightness': brightness}})
        self.assertEqual(self.added, [entity])
        self.assertTrue(entity.available)

    def test_removed_entity_is_unavailable_and_return_uses_same_identity(self):
        self.register('light', light_module.OpenBekenLight)
        self.descriptions(light())
        self.snapshot({'light_0': {'on': True}})
        entity = self.added[0]
        unique_id = entity._attr_unique_id
        self.descriptions()
        self.assertFalse(entity.available)
        self.assertNotIn('light_0', self.coordinator.states)
        self.descriptions(light())
        self.snapshot({'light_0': {'on': True}})
        self.assertTrue(entity.available)
        self.assertEqual(self.added, [entity])
        self.assertEqual(entity._attr_unique_id, unique_id)

    def test_existing_light_changes_modes_ranges_and_effects(self):
        self.register('light', light_module.OpenBekenLight)
        self.descriptions(light())
        entity = self.added[0]
        self.assertEqual(entity.supported_color_modes, {ColorMode.BRIGHTNESS})
        self.descriptions(light(('brightness', 'rgb', 'color_temp', 'effects'),
                                min_mireds=200, max_mireds=500, effects=['Rainbow']))
        self.assertEqual(entity.supported_color_modes, {ColorMode.RGB, ColorMode.COLOR_TEMP})
        self.assertEqual(entity._attr_min_color_temp_kelvin, 2000)
        self.assertEqual(entity._attr_max_color_temp_kelvin, 5000)
        self.assertEqual(entity.effect_list, ['Off', 'Rainbow'])
        self.descriptions(light(('brightness', 'rgb')))
        self.assertEqual(entity.supported_color_modes, {ColorMode.RGB})
        self.assertIsNone(entity.effect_list)
        self.assertEqual(entity._attr_supported_features, 0)
        self.assertEqual(entity._attr_max_color_temp_kelvin, 6535)
        self.assertEqual(len(self.added), 1)

    def test_effect_color_mode_is_advertised(self):
        self.register('light', light_module.OpenBekenLight)
        self.descriptions(light(('brightness', 'rgb', 'effects'), effects=['Rainbow']))
        self.snapshot({'light_0': {'mode': 'effect', 'effect': 'Rainbow'}})
        entity = self.added[0]
        self.assertIn(entity.color_mode, entity.supported_color_modes)

    def test_sensor_metadata_and_platform_changes(self):
        self.register('sensor', sensor_module.OpenBekenSensor)
        self.register('binary_sensor', binary_module.OpenBekenBinarySensor)
        self.descriptions(dict(id='sensor_2', platform='sensor', unit='V', device_class='voltage'))
        sensor = self.added[0]
        self.descriptions(dict(id='sensor_2', platform='sensor', unit='A', device_class='current', state_class='measurement'))
        self.assertEqual(sensor._attr_native_unit_of_measurement, 'A')
        self.assertEqual(sensor._attr_device_class, 'current')
        self.descriptions(dict(id='binary_sensor_2', platform='binary_sensor', device_class='motion'))
        self.assertFalse(sensor.available)
        self.assertEqual(len(self.added), 2)

    def test_buttons_and_relays_are_dynamic(self):
        self.register('button', button_module.OpenBekenRestartButton)
        self.register('switch', switch_module.OpenBekenSwitch)
        self.descriptions(dict(id='restart_0', platform='button'))
        button = self.added[0]
        self.assertTrue(button.available)
        self.descriptions(dict(id='restart_0', platform='button'), dict(id='switch_4', platform='switch'))
        self.snapshot({'switch_4': {'on': False}})
        self.assertTrue(self.added[1].available)
        self.descriptions()
        self.assertFalse(button.available)
        self.assertFalse(self.added[1].available)

    def test_platform_listener_is_unregistered_on_unload(self):
        self.register('light', light_module.OpenBekenLight)
        unsubscribe = self.entry.async_on_unload.call_args.args[0]
        unsubscribe()
        self.descriptions(light())
        self.assertEqual(self.added, [])

    def test_device_metadata_is_refreshed_without_erasing_version(self):
        self.descriptions(device=dict(name='New name', firmware='ha_api_dynamic'))
        self.registry.async_update_device.assert_called_with(
            'existing-device', sw_version='ha_api_dynamic', name='New name', configuration_url='http://device.local')
        self.descriptions(device=dict(firmware=None))
        self.assertEqual(self.coordinator.firmware, 'ha_api_dynamic')

    async def test_temperature_command_uses_latest_capabilities(self):
        self.register('light', light_module.OpenBekenLight)
        self.descriptions(light(('brightness', 'color_temp'), min_mireds=154, max_mireds=500))
        self.coordinator.async_set_state = AsyncMock()
        await self.added[0].async_turn_on(color_temp_kelvin=4000)
        self.coordinator.async_set_state.assert_awaited_once_with(
            'light_0', {'on': True, 'color_temp': 250, 'mode': 'white'})

    async def test_tcp_push_adds_entities_and_refreshes_existing_light(self):
        changed = asyncio.Event()
        server_done = asyncio.Event()

        async def serve(reader, writer):
            async def send(message):
                writer.write((json.dumps(message) + '\n').encode())
                await writer.drain()
            try:
                await send(dict(type='hello', protocol=1, device_id='AABBCCDDEEFF', firmware='ha_api_dynamic'))
                self.assertEqual(json.loads(await reader.readline())['type'], 'hello')
                await send(dict(type='entities', entities=[light()]))
                await send(dict(type='state', full=True, seq=1, entities={'light_0': {'on': True}}))
                await changed.wait()
                await send(dict(type='entities', entities=[light(('brightness', 'color_temp'), min_mireds=154, max_mireds=500),
                                                          dict(id='sensor_3', platform='sensor', unit='V')]))
                await send(dict(type='state', full=True, seq=2, entities={'light_0': {'on': True, 'mode': 'white', 'color_temp': 250},
                                                                        'sensor_3': {'value': 230}}))
                await reader.read()
            finally:
                writer.close()
                await writer.wait_closed()
                server_done.set()

        server = await asyncio.start_server(serve, '127.0.0.1', 0)
        self.coordinator.host = '127.0.0.1'
        self.coordinator.port = server.sockets[0].getsockname()[1]
        try:
            await self.coordinator._connect_once()
            self.register('light', light_module.OpenBekenLight)
            self.register('sensor', sensor_module.OpenBekenSensor)
            original = self.added[0]
            changed.set()
            async def wait_for_snapshot():
                while self.coordinator._seq != 2:
                    await asyncio.sleep(0.01)
            await asyncio.wait_for(wait_for_snapshot(), timeout=2)
            self.assertEqual(len(self.added), 2)
            self.assertIs(self.added[0], original)
            self.assertEqual(original.supported_color_modes, {ColorMode.COLOR_TEMP})
            self.assertEqual(original.color_temp_kelvin, 4000)
            self.assertEqual(self.added[1].native_value, 230)
            self.assertTrue(original.available)
        finally:
            await self.coordinator.async_shutdown()
            server.close()
            await server.wait_closed()
            await asyncio.wait_for(server_done.wait(), timeout=2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
