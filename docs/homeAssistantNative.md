# Native Home Assistant integration

OpenBeken includes a native Home Assistant integration in `custom_components/openbeken`. It uses a local TCP push protocol and mDNS discovery, so it does not require MQTT. MQTT Home Assistant discovery remains available for existing installations.

## Device setup

The device firmware must include the `OpenBekenAPI` and `MDNS` drivers. The generic local BK7231N image is `output/ha_api_dynamic/OpenBK7231N_App_ha_api_dynamic.rbl` (variant 0), built with `build_scripts\build_bk7231n.bat ha_api_dynamic build 0`. It includes the native API, mDNS and the standard variant 0 drivers, including IR, PixelAnim and SM16703P. Configure pins, channels, button actions and remote handlers through the device settings or `autoexec.bat`. The image does not embed a device profile or the earlier device-specific LED output modifications. Firmware images are chip-specific; this image is for BK7231N. The Home Assistant custom component is installed separately and is not part of the RBL file.

In the device console run `startDriver OpenBekenAPI` and `startDriver MDNS`. Add both commands to `autoexec.bat` so they run after every reboot. The API listens on TCP port 6054 and mDNS advertises `_openbeken._tcp.local`. Manual IP setup in Home Assistant works if mDNS is unavailable.

Copy `custom_components/openbeken` into the Home Assistant configuration's `custom_components` directory, then restart Home Assistant. Add the **OpenBeken** integration. Home Assistant offers discovered devices for confirmation; a device can also be added by entering its IP address and port. The connection is local and currently has no authentication or TLS, so use it on a trusted network.

## Home Assistant behavior

The logical LED output is presented as a Home Assistant light. It supports on/off and brightness, RGB color where the firmware reports RGB output, color temperature where the firmware reports a supported white-temperature range, and the exact PixelAnim effect names exposed by the device. HA's standard light effect selector is used, like WLED's effect selector. Relays are separate switch entities.

Configured temperature, humidity, voltage, current, power, battery, illuminance and generic read-only channels appear as sensors. Motion and open/closed channels appear as binary sensors. Automatically configured DHT/SHT/CHT and DS18B20 channels are also recognized. Active power-meter drivers expose voltage, current, power, frequency and total energy. Channels marked private with `SetChannelPrivate` are excluded.

Firmware checks the advertised entity description once per second and pushes a new description plus a full state snapshot when it changes. Integration version 1.1.0 dynamically adds new entities, updates existing light modes, temperature ranges, effects and sensor metadata, and marks disappeared entities unavailable. The existing unique IDs and entity registrations remain intact when an entity returns. No integration reload is needed for channel or driver changes. Device name and firmware metadata also follow the new description.

The integration uses the Wi-Fi MAC as the stable device identity, shows the firmware version and links the device entry to its web interface. It provides a restart button, restores full state on every connection, streams channel changes and samples cached power readings once per second. It requests a snapshot if it detects a missed sequence number. Effect animation frames are not sent as state updates. Light and channel commands go through OpenBeken's existing control layers.

Integration version 1.0.1 refreshes the registered firmware version and web interface link after reconnecting, including after an OTA update. To install this fix, replace the `custom_components/openbeken` directory in Home Assistant and restart Home Assistant; a new firmware upload is not required.

For dynamic discovery, install both the `ha_api_dynamic` firmware and integration version 1.1.0. Upgrade the integration first, then upload the firmware through the device's OTA page. The existing device configuration remains separate from the image. Older native API firmware can still be used with integration 1.1.0, but cannot push configuration changes during a connection.

## Development installation

For local development, symlink or copy this repository's `custom_components/openbeken` directory into Home Assistant's `config/custom_components/openbeken`. Validate it with Home Assistant's config checker and integration tests in a Home Assistant Core checkout.

Run `python -m unittest discover -s tests/openbeken -v` for isolated lifecycle regression checks. These use HA test doubles and a real loopback TCP connection, and do not replace validation in a running Home Assistant installation or physical hardware checks.
