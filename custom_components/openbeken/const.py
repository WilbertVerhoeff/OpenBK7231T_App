"""Constants for OpenBeken."""

from homeassistant.const import Platform

DOMAIN = "openbeken"
PLATFORMS = [Platform.LIGHT, Platform.SWITCH, Platform.SENSOR, Platform.BINARY_SENSOR, Platform.BUTTON]
DEFAULT_PORT = 6054
PROTOCOL_VERSION = 1
SERVICE_TYPE = "_openbeken._tcp.local."
