# OpenBeken Native API

The OpenBeken Native API (OBKA) is a small LAN-only, persistent TCP protocol used by the bundled Home Assistant custom integration. It is independent from MQTT, the raw command TCP server (port 100), REST, and the ESPHome Bluetooth Proxy API.

## Discovery and connection

The server listens on TCP port **6054**. When mDNS is available it publishes `_openbeken._tcp.local` with `id`, `name`, `version`, and `api=1` TXT records. `id` is the Wi-Fi MAC address, not an IP address. The existing `_http` mDNS service remains enabled.

The driver is opt-in: run `startDriver OpenBekenAPI` to start it. Run `startDriver MDNS` for automatic discovery. Add both commands to `autoexec.bat` to start them after each reboot.

There is one client slot. Each message is one UTF-8 JSON object terminated by `\n`; a line must not exceed 1023 bytes. A client first receives:

```json
{"type":"hello","protocol":1,"device_id":"AABBCCDDEEFF","name":"OpenBeken","firmware":"1.x","platform":"BK7231N"}
```

It must reply before any other request:

```json
{"type":"hello","protocol":1,"client":"home-assistant"}
```

The device then sends `entities` and a full `state` snapshot. Unsupported versions return `unsupported_protocol` and the connection closes.

## Entities and state

Version 1 exposes only metadata the firmware can represent reliably:

- `light_0` for the standard LED controller's active outputs. It always has `on_off`, `brightness` (0–255), and `mode`. `rgb` follows configured logical RGB outputs, including active LED driver remaps and initialized strip color order. `color_temp` is advertised for warm/cold outputs, or the standard flag 24 RGB cool-white emulation with a white output. Thus two-PWM CCT and five-output RGBCW devices are supported without requiring SM16703P. `white_level` is advertised only when a physical PWM white output can be identified. `effects` and the exact effect-name list are advertised only when PixelAnim and SM16703P are both running. Raw PWM mode and forced controller flags follow the standard controller configuration.
- `switch_<channel>` for configured relay channels.
- `sensor_<channel>` for configured numeric sensor channels, including automatically configured DHT/SHT/CHT and DS18B20 channels. Descriptors include `device_class` and `unit` when known; state is `{"value":number}` or `{"value":null}` before a valid reading.
- `binary_sensor_<channel>` for motion and open/closed channels, with `{"on":boolean}` state.
- `energy_<index>` for voltage, current, power, frequency and total energy when power metering is active. Descriptors include `device_class`, `unit` and `state_class`; cached reading changes are sent at most once per second.
- `restart_0`, a Home Assistant button that requests a device restart.

Channels marked private with `SetChannelPrivate` are excluded from entity listings and state updates.

`mode` is `rgb`, `white`, or `effect` where supported. The `rgb` value is the remembered RGB selection, even while warm white is active; `white_level` (0–255) reports the physical white PWM channel output. Thus an RGB+white panel can distinguish selected RGB color from actual warm-white output. `effect` is the selected PixelAnim name in effect mode and `null` otherwise. Animation frames are not emitted as state changes.

The generic `ha_api_dynamic` firmware uses OpenBeken's standard LED output layer. It keeps protocol version 1 and the existing state field names, including `color_temp`, `min_mireds`, and `max_mireds` where supported. Physical output behavior is determined by OpenBeken's configured pins, channels and drivers. The API does not implement separate light mixing or embed a device profile.

Entity descriptions are sent on every connection and can also arrive unsolicited during a connection. The server rebuilds and compares the description once per second; changed descriptions are immediately followed by a full state snapshot. Readings and animation frames do not affect this comparison. Descriptions include optional `device` metadata with `name` and `firmware`, which can also change. A client must reconcile entity IDs, refresh existing entity capabilities and make disappeared entities unavailable, rather than treating the first description as permanent. Existing protocol 1 clients may ignore the optional metadata.

`get_state` requests a fresh authoritative snapshot at any time. Snapshots and deltas include a boot-local monotonic `seq`. A client that sees a gap must issue `get_state`.

```json
{"type":"get_state"}
{"type":"state","full":true,"seq":42,"entities":{"light_0":{"on":true,"brightness":180,"mode":"white","rgb":[255,80,0],"white_level":180,"effect":null},"switch_4":{"on":false}}}
{"type":"state_changed","seq":43,"entity":"light_0","state":{"on":true,"brightness":128,"mode":"rgb","rgb":[255,0,0],"white_level":0,"effect":null}}
```

Changes to normal channels flow through OpenBeken's central `CHANNEL_Set` path. Logical LED changes are emitted after the existing LED layer has applied RGB/CW/PWM output; therefore IR, PixelAnim, the web UI, scripts, TuyaMCU, timers, and native commands share the same source of truth. Light changes are coalesced for about 40 ms.

## Commands and errors

Only typed state commands and the explicit restart request are accepted—there is no arbitrary console-command endpoint.

```json
{"type":"set_state","id":123,"entity":"light_0","state":{"on":true,"brightness":128,"rgb":[255,100,0]}}
{"type":"result","id":123,"success":true}
{"type":"restart","id":124}
{"type":"result","id":124,"success":true}
```

For a light with the matching advertised features, `{"mode":"white","white_level":200}` selects its white output and sets the brightness; `{"mode":"rgb","rgb":[255,0,0]}` selects RGB; `{"mode":"effect","effect":"Rainbow Cycle"}` selects an exact advertised effect name. When `color_temp` is advertised, `{"color_temp":327}` sets OpenBeken's LED temperature value and applies the configured outputs through the standard LED layer. `white_level` and `brightness` may not be supplied together because both control the firmware's single light dimmer. `on` can accompany any supported mode. The `mode` field distinguishes RGB color, white output, and animated effects. The firmware's normal light/LED control layer applies commands; this protocol never writes pins directly.

Possible stable errors are `malformed_json`, `packet_too_large`, `invalid_request`, `unsupported_protocol`, `unsupported_type`, `unknown_entity`, and `unsupported_feature`.

## Resilience and security

The server accepts a replacement connection after disconnects and resumes listening after Wi-Fi returns. Reconnecting clients always receive entities plus a full snapshot. It uses a low-rate 60-second `ping`/`pong` heartbeat, samples cached power readings once per second, bounds input size, and disconnects broken sockets rather than blocking channel callbacks.

This version has no TLS or authentication and must be used only on a trusted LAN. Protocol versioning and the initial hello leave room for an authenticated future version.

## Home Assistant integration

The repository's `custom_components/openbeken` integration browses `_openbeken._tcp.local`, uses TXT `id` as the unique identity, connects to port 6054, negotiates protocol 1, consumes `entities`, issues `get_state` after reconnect, and uses `seq` to detect missed deltas. It is a local push (`iot_class: local_push`) integration. Installation and setup are documented in [homeAssistantNative.md](homeAssistantNative.md).
