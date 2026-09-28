# Native Home Assistant integration

The Home Assistant integration is maintained in its own repository:
[WilbertVerhoeff/openbeken-homeassistant](https://github.com/WilbertVerhoeff/openbeken-homeassistant).
This firmware repository contains the native API server and its protocol documentation.
The integration source, tests, installation instructions and releases belong to the integration repository.

## Device setup

Use firmware built for your device's chipset that includes the `OpenBekenAPI` driver.
In the device console run `startDriver OpenBekenAPI`. For automatic discovery,
also run `startDriver MDNS` on firmware that supports mDNS. Add these commands
to `autoexec.bat` to start them after every reboot.

The API listens on TCP port 6054 and mDNS advertises `_openbeken._tcp.local`.
Manual IP setup in Home Assistant works if mDNS is unavailable. MQTT and existing
MQTT Home Assistant discovery remain separate from the native API.

The connection currently has no authentication or TLS; use it on a trusted LAN.
See [openbekenNativeAPI.md](openbekenNativeAPI.md) for the protocol, supported
entities and connection behavior. Follow the installation instructions in the
[integration repository](https://github.com/WilbertVerhoeff/openbeken-homeassistant)
to install and configure the Home Assistant component.
