#include "../new_common.h"
#include "../new_cfg.h"
#include "../new_pins.h"
#include "../hal/hal_wifi.h"
#include "../cmnds/cmd_public.h"
#include "../cJSON/cJSON.h"
#include "../logging/logging.h"
#include "drv_openbeken_api.h"
#include "drv_mdns.h"
#include "drv_public.h"
#include "drv_local.h"
#include <lwip/sockets.h>
#include <math.h>
#include <float.h>

#if ENABLE_DRIVER_OPENBEKEN_API

#define OBKA_PORT 6054
#define OBKA_INVALID_SOCK -1
#define OBKA_RX_MAX 1024
#define OBKA_TX_MAX 8192
#define OBKA_BATCH_MS 40

extern int Main_HasWiFiConnected(void);

static volatile unsigned long g_obkaChangedChannels[2];
static volatile int g_obkaLightDirty;
static float g_obkaEnergyValues[OBK__NUM_SENSORS];
static int g_obkaEnergyKnown[OBK__NUM_SENSORS];
static volatile unsigned long g_obkaChangedEnergy;
static volatile unsigned int g_obkaSequence;
static volatile unsigned int g_obkaLastChangeMs;
static volatile int g_obkaRunning;
static int g_obkaListenSock = OBKA_INVALID_SOCK;
static xTaskHandle g_obkaThread = NULL;
/* Only the server thread writes this buffer. Keeping it off the task stack is
 * important: the receive buffer and JSON parser are active during a hello. */
static char g_obkaTx[OBKA_TX_MAX];

static void OBKA_MarkChannel(int ch) {
	if (ch >= 0 && ch < CHANNEL_MAX) {
		g_obkaChangedChannels[ch / 32] |= 1UL << (ch % 32);
		g_obkaLastChangeMs = rtos_get_time();
	}
}

typedef struct {
	const char *deviceClass;
	const char *unit;
	float divisor;
	int binary;
	int inverse;
} OBKA_SensorSpec;

/* Channel types are the same source used by MQTT HA discovery. */
static int OBKA_GetSensorSpec(int ch, OBKA_SensorSpec *spec) {
	int type;
	if (!CHANNEL_IsInUse(ch) || CHANNEL_HasNeverPublishFlag(ch)) return 0;
	type = CHANNEL_GetType(ch);
	spec->deviceClass = ""; spec->unit = ""; spec->divisor = 1; spec->binary = 0; spec->inverse = 0;
	if (type == ChType_Default) {
		int pin;
		for (pin = 0; pin < PLATFORM_GPIO_MAX; pin++) {
			int role = PIN_GetPinRoleForPinIndex(pin);
			if (g_cfg.pins.channels[pin] == ch) {
				if (IS_PIN_DHT_ROLE(role) || IS_PIN_TEMP_HUM_SENSOR_ROLE(role)) {
					spec->deviceClass = "temperature"; spec->unit = "°C"; spec->divisor = 10; return 1;
				}
				if (role == IOR_DS1820_IO) {
					spec->deviceClass = "temperature"; spec->unit = "°C"; spec->divisor = 100; return 1;
				}
			}
			if (g_cfg.pins.channels2[pin] == ch &&
				(IS_PIN_DHT_ROLE(role) || IS_PIN_TEMP_HUM_SENSOR_ROLE(role))) {
				spec->deviceClass = "humidity"; spec->unit = "%"; return 1;
			}
		}
		return 0;
	}
	switch (type) {
	case ChType_Temperature: spec->deviceClass = "temperature"; spec->unit = "°C"; break;
	case ChType_Temperature_div2: spec->deviceClass = "temperature"; spec->unit = "°C"; spec->divisor = 2; break;
	case ChType_Temperature_div10: spec->deviceClass = "temperature"; spec->unit = "°C"; spec->divisor = 10; break;
	case ChType_Temperature_div100: spec->deviceClass = "temperature"; spec->unit = "°C"; spec->divisor = 100; break;
	case ChType_Humidity: spec->deviceClass = "humidity"; spec->unit = "%"; break;
	case ChType_Humidity_div10: spec->deviceClass = "humidity"; spec->unit = "%"; spec->divisor = 10; break;
	case ChType_Voltage_div10: spec->deviceClass = "voltage"; spec->unit = "V"; spec->divisor = 10; break;
	case ChType_Voltage_div100: spec->deviceClass = "voltage"; spec->unit = "V"; spec->divisor = 100; break;
	case ChType_Current_div100: spec->deviceClass = "current"; spec->unit = "A"; spec->divisor = 100; break;
	case ChType_Current_div1000: case ChType_LeakageCurrent_div1000: spec->deviceClass = "current"; spec->unit = "A"; spec->divisor = 1000; break;
	case ChType_Power: spec->deviceClass = "power"; spec->unit = "W"; break;
	case ChType_Power_div10: spec->deviceClass = "power"; spec->unit = "W"; spec->divisor = 10; break;
	case ChType_Power_div100: spec->deviceClass = "power"; spec->unit = "W"; spec->divisor = 100; break;
	case ChType_BatteryLevelPercent: spec->deviceClass = "battery"; spec->unit = "%"; break;
	case ChType_Illuminance: spec->deviceClass = "illuminance"; spec->unit = "lx"; break;
	case ChType_ReadOnly: case ChType_Custom: break;
	case ChType_ReadOnly_div10: spec->divisor = 10; break;
	case ChType_ReadOnly_div100: spec->divisor = 100; break;
	case ChType_ReadOnly_div1000: spec->divisor = 1000; break;
	case ChType_Motion: spec->binary = 1; spec->inverse = 1; spec->deviceClass = "motion"; break;
	case ChType_Motion_n: spec->binary = 1; spec->deviceClass = "motion"; break;
	case ChType_OpenClosed: spec->binary = 1; spec->deviceClass = "door"; break;
	case ChType_OpenClosed_Inv: spec->binary = 1; spec->inverse = 1; spec->deviceClass = "door"; break;
	default: return 0;
	}
	return 1;
}

static void OBKA_AppendSensorState(char *out, int outLen, int ch, const OBKA_SensorSpec *spec) {
	float value = CHANNEL_GetFloat(ch) / spec->divisor;
	if (spec->binary) snprintf(out, outLen, "{\"on\":%s}", ((CHANNEL_Get(ch) != 0) != spec->inverse) ? "true" : "false");
	else if (value == value && value <= FLT_MAX && value >= -FLT_MAX) snprintf(out, outLen, "{\"value\":%.4f}", value);
	else snprintf(out, outLen, "{\"value\":null}");
}

static int OBKA_IncludeEnergySensor(int index) {
	return index == OBK_VOLTAGE || index == OBK_CURRENT || index == OBK_POWER ||
		index == OBK_FREQUENCY || index == OBK_CONSUMPTION_TOTAL;
}

static void OBKA_ScanEnergy(void) {
#if ENABLE_BL_SHARED
	int i;
	if (!DRV_IsMeasuringPower()) return;
	for (i = OBK__FIRST; i <= OBK__LAST; i++) {
		float value;
		if (!OBKA_IncludeEnergySensor(i) || !BL_HasEnergySensorReading((energySensor_t)i)) continue;
		value = DRV_GetReading((energySensor_t)i);
		if (value != value || value > FLT_MAX || value < -FLT_MAX) continue;
		if (!g_obkaEnergyKnown[i] || fabsf(value - g_obkaEnergyValues[i]) > 0.0001f) {
			g_obkaEnergyValues[i] = value;
			g_obkaEnergyKnown[i] = 1;
			g_obkaChangedEnergy |= 1UL << i;
			g_obkaLastChangeMs = rtos_get_time();
		}
	}
#endif
}

static int OBKA_Send(int sock, const char *text) {
	int len = strlen(text);
	int offset = 0, retries = 0;
	if (len >= OBKA_TX_MAX) return 0;
	while (offset < len) {
		int sent = send(sock, text + offset, len - offset, 0);
		if (sent > 0) { offset += sent; continue; }
		if (sent < 0 && errno == EINTR) continue;
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && retries++ < 40) {
			rtos_delay_milliseconds(5);
			continue;
		}
		return 0; /* Slow or broken clients must not stall the device. */
	}
	return 1;
}

static void OBKA_JSONString(char *out, int outLen, const char *in) {
	int o = 0;
	while (*in && o < outLen - 2) {
		char c = *in++;
		if (c == '"' || c == '\\') { if (o < outLen - 3) out[o++] = '\\'; else break; }
		if ((unsigned char)c < 32) c = ' ';
		out[o++] = c;
	}
	out[o] = 0;
}

static int OBKA_HasLight(void) {
#if ENABLE_LED_BASIC
	int ch;
	for (ch = 0; ch < 5; ch++) {
		if (CHANNEL_HasChannelPinWithRoleOrRole(ch, IOR_PWM, IOR_PWM_n)) return 1;
	}
#if ENABLE_DRIVER_SM16703P
	if (DRV_IsRunning("SM16703P") && pixel_count > 0) return 1;
#endif
	return 0;
#else
	return 0;
#endif
}

static int OBKA_HasRGB(void) {
#if ENABLE_DRIVER_SM16703P
	if (DRV_IsRunning("SM16703P") && pixel_count > 0) return 1;
#endif
	return CHANNEL_HasChannelPinWithRoleOrRole(0, IOR_PWM, IOR_PWM_n) &&
		CHANNEL_HasChannelPinWithRoleOrRole(1, IOR_PWM, IOR_PWM_n) &&
		CHANNEL_HasChannelPinWithRoleOrRole(2, IOR_PWM, IOR_PWM_n);
}

static int OBKA_WhiteChannel(void) {
	int ch;
	if (CHANNEL_HasChannelPinWithRoleOrRole(4, IOR_PWM, IOR_PWM_n)) return 4;
	/* Strip + one PWM is a common RGB + white arrangement. */
#if ENABLE_DRIVER_SM16703P
	if (DRV_IsRunning("SM16703P") && pixel_count > 0) {
		for (ch = 0; ch < 5; ch++)
			if (CHANNEL_HasChannelPinWithRoleOrRole(ch, IOR_PWM, IOR_PWM_n)) return ch;
	}
#endif
	if (!OBKA_HasRGB()) {
		for (ch = 0; ch < 5; ch++)
			if (CHANNEL_HasChannelPinWithRoleOrRole(ch, IOR_PWM, IOR_PWM_n)) return ch;
	}
	return -1;
}

static int OBKA_HasEffects(void) {
#if ENABLE_DRIVER_PIXELANIM && ENABLE_DRIVER_SM16703P
	return DRV_IsRunning("PixelAnim") && DRV_IsRunning("SM16703P") && pixel_count > 0;
#else
	return 0;
#endif
}

static int OBKA_HasColorTemp(void) {
#if ENABLE_LED_BASIC && ENABLE_DRIVER_SM16703P
	int pwmCount = 0;
	PIN_get_Relay_PWM_Count(0, &pwmCount, 0);
	return OBKA_HasRGB() && OBKA_WhiteChannel() >= 0 &&
		pwmCount == 1 &&
		DRV_IsRunning("SM16703P") && pixel_count > 0 &&
		CFG_HasFlag(OBK_FLAG_LED_EMULATE_COOL_WITH_RGB);
#else
	return 0;
#endif
}

static const char *OBKA_LightMode(void) {
#if ENABLE_LED_BASIC
	switch (LED_GetMode()) {
	case Light_Temperature: return OBKA_WhiteChannel() >= 0 ? "white" : "temperature";
	case Light_RGB: return "rgb";
	case Light_Anim: return "effect";
	default: return "all";
	}
#else
	return "all";
#endif
}

static void OBKA_AppendLightState(char *out, int outLen) {
#if ENABLE_LED_BASIC
	int r = (int)LED_GetRed255(), g = (int)LED_GetGreen255(), b = (int)LED_GetBlue255();
	int whiteChannel = OBKA_WhiteChannel();
	int n = snprintf(out, outLen, "{\"on\":%s,\"brightness\":%d,\"mode\":\"%s\"",
		LED_GetEnableAll() ? "true" : "false", (int)(LED_GetDimmer() * 255 / 100), OBKA_LightMode());
	if (OBKA_HasRGB() && n < outLen)
		n += snprintf(out + n, outLen - n, ",\"rgb\":[%d,%d,%d]", r, g, b);
	if (whiteChannel >= 0 && n < outLen)
		n += snprintf(out + n, outLen - n, ",\"white_level\":%d", CHANNEL_Get(whiteChannel) * 255 / 100);
	if (OBKA_HasColorTemp() && n < outLen)
		n += snprintf(out + n, outLen - n, ",\"color_temp\":%d", (int)LED_GetTemperature());
#if ENABLE_DRIVER_PIXELANIM
	if (OBKA_HasEffects() && n < outLen) {
		if (LED_GetMode() == Light_Anim && activeAnim >= 0 && activeAnim < g_numAnims) {
			char effect[64];
			OBKA_JSONString(effect, sizeof(effect), g_anims[activeAnim].name);
			n += snprintf(out + n, outLen - n, ",\"effect\":\"%s\"", effect);
		} else n += snprintf(out + n, outLen - n, ",\"effect\":null");
	}
#endif
	if (n < outLen) snprintf(out + n, outLen - n, "}");
#else
	strcpy_safe(out, "{}", outLen);
#endif
}

static int OBKA_SendEntities(int sock) {
	char *out = g_obkaTx, name[96];
	int ch, first = 1;
	int n = snprintf(out, OBKA_TX_MAX, "{\"type\":\"entities\",\"entities\":[");
	n += snprintf(out + n, OBKA_TX_MAX - n, "{\"id\":\"restart_0\",\"platform\":\"button\",\"name\":\"Restart\"}");
	first = 0;
	if (OBKA_HasLight()) {
		n += snprintf(out + n, OBKA_TX_MAX - n, "%s{\"id\":\"light_0\",\"platform\":\"light\",\"name\":\"Light\",\"features\":[\"on_off\",\"brightness\",\"mode\"", first ? "" : ",");
		if (OBKA_HasRGB()) n += snprintf(out + n, OBKA_TX_MAX - n, ",\"rgb\"");
		if (OBKA_WhiteChannel() >= 0) n += snprintf(out + n, OBKA_TX_MAX - n, ",\"white_level\"");
		if (OBKA_HasColorTemp()) n += snprintf(out + n, OBKA_TX_MAX - n, ",\"color_temp\"");
		if (OBKA_HasEffects()) n += snprintf(out + n, OBKA_TX_MAX - n, ",\"effects\"");
		n += snprintf(out + n, OBKA_TX_MAX - n, "]");
		if (OBKA_HasColorTemp()) n += snprintf(out + n, OBKA_TX_MAX - n,
			",\"min_mireds\":%d,\"max_mireds\":%d", (int)led_temperature_min, (int)led_temperature_max);
#if ENABLE_DRIVER_PIXELANIM
		if (OBKA_HasEffects()) {
			int i;
			n += snprintf(out + n, OBKA_TX_MAX - n, ",\"effects\":[");
			for (i = 0; i < g_numAnims && n < OBKA_TX_MAX - 80; i++) {
				char effect[64];
				OBKA_JSONString(effect, sizeof(effect), g_anims[i].name);
				n += snprintf(out + n, OBKA_TX_MAX - n, "%s\"%s\"", i ? "," : "", effect);
			}
			n += snprintf(out + n, OBKA_TX_MAX - n, "]");
		}
#endif
		n += snprintf(out + n, OBKA_TX_MAX - n, "}");
		first = 0;
	}
	for (ch = 0; ch < CHANNEL_MAX; ch++) {
		int written;
		if (!CHANNEL_IsInUse(ch) || !CHANNEL_IsPowerRelayChannel(ch) || CHANNEL_HasNeverPublishFlag(ch)) continue;
		OBKA_JSONString(name, sizeof(name), CHANNEL_GetLabel(ch));
		if (n >= OBKA_TX_MAX - 8) break;
		written = snprintf(out + n, OBKA_TX_MAX - n, "%s{\"id\":\"switch_%d\",\"platform\":\"switch\",\"name\":\"%s\",\"features\":[\"on_off\"]}", first ? "" : ",", ch, name[0] ? name : "Switch");
		if (written < 0 || written >= OBKA_TX_MAX - n - 4) { out[n] = 0; break; }
		n += written;
		first = 0;
	}
	for (ch = 0; ch < CHANNEL_MAX; ch++) {
		OBKA_SensorSpec spec;
		int written;
		if (!OBKA_GetSensorSpec(ch, &spec)) continue;
		OBKA_JSONString(name, sizeof(name), CHANNEL_GetLabel(ch));
		written = snprintf(out + n, OBKA_TX_MAX - n,
			"%s{\"id\":\"%s_%d\",\"platform\":\"%s\",\"name\":\"%s\",\"device_class\":\"%s\",\"unit\":\"%s\"}",
			first ? "" : ",", spec.binary ? "binary_sensor" : "sensor", ch,
			spec.binary ? "binary_sensor" : "sensor", name, spec.deviceClass, spec.unit);
		if (written < 0 || written >= OBKA_TX_MAX - n - 4) return 0;
		n += written; first = 0;
	}
#if ENABLE_BL_SHARED
	if (DRV_IsMeasuringPower()) for (ch = OBK__FIRST; ch <= OBK__LAST; ch++) {
		energySensorNames_t *sensor;
		int written;
		if (!OBKA_IncludeEnergySensor(ch)) continue;
		sensor = DRV_GetEnergySensorNames((energySensor_t)ch);
		if (!sensor || !sensor->name_friendly || !strcmp(sensor->hass_dev_class, "timestamp")) continue;
		OBKA_JSONString(name, sizeof(name), sensor->name_friendly);
		written = snprintf(out + n, OBKA_TX_MAX - n,
			"%s{\"id\":\"energy_%d\",\"platform\":\"sensor\",\"name\":\"%s\",\"device_class\":\"%s\",\"unit\":\"%s\",\"state_class\":\"%s\"}",
			first ? "" : ",", ch, name, sensor->hass_dev_class, sensor->units,
			!strcmp(sensor->hass_dev_class, "energy") ? "total_increasing" : "measurement");
		if (written < 0 || written >= OBKA_TX_MAX - n - 4) return 0;
		n += written; first = 0;
	}
#endif
	snprintf(out + n, OBKA_TX_MAX - n, "]}\n");
	return OBKA_Send(sock, out);
}

static int OBKA_SendSnapshot(int sock) {
	char *out = g_obkaTx, state[256];
	int ch, first = 1, n;
	n = snprintf(out, OBKA_TX_MAX, "{\"type\":\"state\",\"full\":true,\"seq\":%u,\"entities\":{", g_obkaSequence);
	if (OBKA_HasLight()) {
		OBKA_AppendLightState(state, sizeof(state));
		n += snprintf(out + n, OBKA_TX_MAX - n, "\"light_0\":%s", state); first = 0;
	}
	for (ch = 0; ch < CHANNEL_MAX; ch++) {
		if (!CHANNEL_IsInUse(ch) || !CHANNEL_IsPowerRelayChannel(ch) || CHANNEL_HasNeverPublishFlag(ch)) continue;
		if (n > OBKA_TX_MAX - 48) break;
		n += snprintf(out + n, OBKA_TX_MAX - n, "%s\"switch_%d\":{\"on\":%s}", first ? "" : ",", ch, CHANNEL_Get(ch) ? "true" : "false"); first = 0;
	}
	for (ch = 0; ch < CHANNEL_MAX; ch++) {
		OBKA_SensorSpec spec;
		if (!OBKA_GetSensorSpec(ch, &spec)) continue;
		OBKA_AppendSensorState(state, sizeof(state), ch, &spec);
		if (n + (int)strlen(state) + 40 >= OBKA_TX_MAX) return 0;
		n += snprintf(out + n, OBKA_TX_MAX - n, "%s\"%s_%d\":%s", first ? "" : ",", spec.binary ? "binary_sensor" : "sensor", ch, state);
		first = 0;
	}
#if ENABLE_BL_SHARED
	if (DRV_IsMeasuringPower()) for (ch = OBK__FIRST; ch <= OBK__LAST; ch++) {
		float value;
		if (!OBKA_IncludeEnergySensor(ch)) continue;
		if (!BL_HasEnergySensorReading((energySensor_t)ch)) {
			if (n + 60 >= OBKA_TX_MAX) return 0;
			n += snprintf(out + n, OBKA_TX_MAX - n, "%s\"energy_%d\":{\"value\":null}", first ? "" : ",", ch);
			first = 0;
			continue;
		}
		value = DRV_GetReading((energySensor_t)ch);
		if (n + 70 >= OBKA_TX_MAX) return 0;
		n += snprintf(out + n, OBKA_TX_MAX - n, "%s\"energy_%d\":{\"value\":%.4f}", first ? "" : ",", ch, value);
		first = 0;
		g_obkaEnergyValues[ch] = value; g_obkaEnergyKnown[ch] = 1;
	}
#endif
	snprintf(out + n, OBKA_TX_MAX - n, "}}\n");
	return OBKA_Send(sock, out);
}

static int OBKA_SendPending(int sock) {
	unsigned long changed0, changed1;
	char out[512], state[256];
	int ch;
	if (!g_obkaLightDirty && !g_obkaChangedChannels[0] && !g_obkaChangedChannels[1] && !g_obkaChangedEnergy) return 1;
	if ((unsigned int)(rtos_get_time() - g_obkaLastChangeMs) < OBKA_BATCH_MS) return 1;
	changed0 = g_obkaChangedChannels[0]; changed1 = g_obkaChangedChannels[1];
	g_obkaChangedChannels[0] = 0; g_obkaChangedChannels[1] = 0;
	if (g_obkaLightDirty) {
		g_obkaLightDirty = 0; OBKA_AppendLightState(state, sizeof(state));
		snprintf(out, sizeof(out), "{\"type\":\"state_changed\",\"seq\":%u,\"entity\":\"light_0\",\"state\":%s}\n", ++g_obkaSequence, state);
		if (!OBKA_Send(sock, out)) return 0;
	}
	for (ch = 0; ch < CHANNEL_MAX; ch++) {
		if (!((ch < 32 ? changed0 : changed1) & (1UL << (ch % 32))) || !CHANNEL_IsPowerRelayChannel(ch) || CHANNEL_HasNeverPublishFlag(ch)) continue;
		snprintf(out, sizeof(out), "{\"type\":\"state_changed\",\"seq\":%u,\"entity\":\"switch_%d\",\"state\":{\"on\":%s}}\n", ++g_obkaSequence, ch, CHANNEL_Get(ch) ? "true" : "false");
		if (!OBKA_Send(sock, out)) return 0;
	}
	for (ch = 0; ch < CHANNEL_MAX; ch++) {
		OBKA_SensorSpec spec;
		if (!((ch < 32 ? changed0 : changed1) & (1UL << (ch % 32))) || !OBKA_GetSensorSpec(ch, &spec)) continue;
		OBKA_AppendSensorState(state, sizeof(state), ch, &spec);
		snprintf(out, sizeof(out), "{\"type\":\"state_changed\",\"seq\":%u,\"entity\":\"%s_%d\",\"state\":%s}\n",
			++g_obkaSequence, spec.binary ? "binary_sensor" : "sensor", ch, state);
		if (!OBKA_Send(sock, out)) return 0;
	}
#if ENABLE_BL_SHARED
	if (g_obkaChangedEnergy) for (ch = OBK__FIRST; ch <= OBK__LAST; ch++) {
		if (!(g_obkaChangedEnergy & (1UL << ch))) continue;
		snprintf(out, sizeof(out), "{\"type\":\"state_changed\",\"seq\":%u,\"entity\":\"energy_%d\",\"state\":{\"value\":%.4f}}\n",
			++g_obkaSequence, ch, g_obkaEnergyValues[ch]);
		if (!OBKA_Send(sock, out)) return 0;
	}
	g_obkaChangedEnergy = 0;
#endif
	return 1;
}

static void OBKA_Result(int sock, cJSON *id, int ok, const char *error) {
	char out[160];
	if (id && cJSON_IsNumber(id)) {
		if (error) snprintf(out, sizeof(out), "{\"type\":\"result\",\"id\":%d,\"success\":false,\"error\":\"%s\"}\n", id->valueint, error);
		else snprintf(out, sizeof(out), "{\"type\":\"result\",\"id\":%d,\"success\":%s}\n", id->valueint, ok ? "true" : "false");
	} else {
		if (error) snprintf(out, sizeof(out), "{\"type\":\"result\",\"success\":false,\"error\":\"%s\"}\n", error);
		else snprintf(out, sizeof(out), "{\"type\":\"result\",\"success\":%s}\n", ok ? "true" : "false");
	}
	OBKA_Send(sock, out);
}

static void OBKA_ProcessSetState(int sock, cJSON *root) {
	cJSON *entity = cJSON_GetObjectItemCaseSensitive(root, "entity"), *state = cJSON_GetObjectItemCaseSensitive(root, "state"), *id = cJSON_GetObjectItemCaseSensitive(root, "id");
	if (!cJSON_IsString(entity) || !cJSON_IsObject(state)) { OBKA_Result(sock, id, 0, "invalid_request"); return; }
	if (!strcmp(entity->valuestring, "light_0") && OBKA_HasLight()) {
#if ENABLE_LED_BASIC
		cJSON *on = cJSON_GetObjectItemCaseSensitive(state, "on");
		cJSON *brightness = cJSON_GetObjectItemCaseSensitive(state, "brightness");
		cJSON *rgb = cJSON_GetObjectItemCaseSensitive(state, "rgb");
		cJSON *mode = cJSON_GetObjectItemCaseSensitive(state, "mode");
		cJSON *white = cJSON_GetObjectItemCaseSensitive(state, "white_level");
		cJSON *colorTemp = cJSON_GetObjectItemCaseSensitive(state, "color_temp");
		cJSON *effect = cJSON_GetObjectItemCaseSensitive(state, "effect");
		cJSON *field;
		cJSON *r = NULL, *g = NULL, *b = NULL;
		const char *requestedMode = NULL;
		int effectIndex = -1;
		int currentEffect = -1;
#if ENABLE_DRIVER_PIXELANIM
		currentEffect = activeAnim;
#endif
		for (field = state->child; field; field = field->next) {
			if (strcmp(field->string, "on") && strcmp(field->string, "brightness") &&
				strcmp(field->string, "rgb") && strcmp(field->string, "mode") &&
				strcmp(field->string, "white_level") && strcmp(field->string, "color_temp") &&
				strcmp(field->string, "effect")) {
				OBKA_Result(sock,id,0,"unsupported_feature"); return;
			}
		}
		if (!on && !brightness && !rgb && !mode && !white && !colorTemp && !effect) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
		if (on && !cJSON_IsBool(on)) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
		if (brightness && (!cJSON_IsNumber(brightness) || brightness->valueint < 0 || brightness->valueint > 255 || brightness->valuedouble != brightness->valueint)) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
		if (white && (OBKA_WhiteChannel() < 0 || !cJSON_IsNumber(white) || white->valueint < 0 || white->valueint > 255 || white->valuedouble != white->valueint || brightness)) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
		if (colorTemp && (!OBKA_HasColorTemp() || !cJSON_IsNumber(colorTemp) ||
			colorTemp->valueint < (int)led_temperature_min || colorTemp->valueint > (int)led_temperature_max ||
			colorTemp->valuedouble != colorTemp->valueint)) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
		if (rgb) {
			if (!OBKA_HasRGB() || !cJSON_IsArray(rgb) || cJSON_GetArraySize(rgb) != 3) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
			r = cJSON_GetArrayItem(rgb,0); g = cJSON_GetArrayItem(rgb,1); b = cJSON_GetArrayItem(rgb,2);
			if (!cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b) || r->valueint < 0 || r->valueint > 255 || g->valueint < 0 || g->valueint > 255 || b->valueint < 0 || b->valueint > 255 || r->valuedouble != r->valueint || g->valuedouble != g->valueint || b->valuedouble != b->valueint) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
		}
		if (effect) {
#if ENABLE_DRIVER_PIXELANIM
			int i;
			if (!OBKA_HasEffects() || !cJSON_IsString(effect)) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
			for (i = 0; i < g_numAnims; i++) if (!strcmp(effect->valuestring, g_anims[i].name)) { effectIndex = i; break; }
			if (effectIndex < 0) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
#else
			OBKA_Result(sock,id,0,"unsupported_feature"); return;
#endif
		}
		if (mode) {
			if (!cJSON_IsString(mode)) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
			requestedMode = mode->valuestring;
			if ((!strcmp(requestedMode,"rgb") && !OBKA_HasRGB()) ||
				(!strcmp(requestedMode,"white") && OBKA_WhiteChannel() < 0) ||
				(!strcmp(requestedMode,"effect") && !OBKA_HasEffects()) ||
				(strcmp(requestedMode,"rgb") && strcmp(requestedMode,"white") && strcmp(requestedMode,"effect"))) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
		} else if (effect) requestedMode = "effect";
		else if (white || colorTemp) requestedMode = "white";
		else if (rgb) requestedMode = "rgb";
		if ((rgb && strcmp(requestedMode,"rgb")) || (white && strcmp(requestedMode,"white")) ||
			(colorTemp && strcmp(requestedMode,"white")) || (effect && strcmp(requestedMode,"effect")) ||
			(requestedMode && !strcmp(requestedMode,"effect") && effectIndex < 0 && currentEffect < 0)) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
		if (requestedMode && !strcmp(requestedMode,"white")) LED_SetTemperature(colorTemp ? colorTemp->valueint : (int)led_temperature_max, true);
		else if (requestedMode && !strcmp(requestedMode,"rgb")) LED_SetFinalRGB(rgb ? r->valueint : LED_GetRed255(), rgb ? g->valueint : LED_GetGreen255(), rgb ? b->valueint : LED_GetBlue255());
#if ENABLE_DRIVER_PIXELANIM
		else if (requestedMode && !strcmp(requestedMode,"effect")) PixelAnim_SetAnim(effectIndex >= 0 ? effectIndex : currentEffect);
#endif
		if (brightness) LED_SetDimmer(brightness->valueint * 100 / 255);
		if (white) LED_SetDimmer(white->valueint * 100 / 255);
		if (on) LED_SetEnableAll(cJSON_IsTrue(on));
		OBKA_Result(sock, id, 1, NULL); return;
#endif
	}
	if (!strncmp(entity->valuestring, "switch_", 7)) {
		char *end;
		long ch = strtol(entity->valuestring + 7, &end, 10);
		cJSON *on = cJSON_GetObjectItemCaseSensitive(state, "on");
		if (end == entity->valuestring + 7 || *end || ch < 0 || ch >= CHANNEL_MAX) { OBKA_Result(sock,id,0,"unknown_entity"); return; }
		if (!CHANNEL_IsPowerRelayChannel((int)ch) || CHANNEL_HasNeverPublishFlag((int)ch)) { OBKA_Result(sock,id,0,"unknown_entity"); return; }
		if (!on || !cJSON_IsBool(on) || !state->child || state->child->next || strcmp(state->child->string,"on")) { OBKA_Result(sock,id,0,"unsupported_feature"); return; }
		CHANNEL_Set((int)ch, cJSON_IsTrue(on), 0); OBKA_Result(sock,id,1,NULL); return;
	}
	OBKA_Result(sock, id, 0, "unknown_entity");
}

static int OBKA_ProcessLine(int sock, char *line, int *helloDone) {
	cJSON *root, *type, *protocol;
	root = cJSON_ParseWithOpts(line, 0, 1);
	if (!root || !cJSON_IsObject(root)) { if(root) cJSON_Delete(root); OBKA_Send(sock,"{\"type\":\"error\",\"error\":\"malformed_json\"}\n"); return 1; }
	type = cJSON_GetObjectItemCaseSensitive(root, "type");
	if (!cJSON_IsString(type)) { cJSON_Delete(root); OBKA_Send(sock,"{\"type\":\"error\",\"error\":\"invalid_request\"}\n"); return 1; }
	if (!*helloDone) {
		protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol");
		if (strcmp(type->valuestring,"hello") || !cJSON_IsNumber(protocol) || protocol->valueint != 1) { cJSON_Delete(root); OBKA_Send(sock,"{\"type\":\"error\",\"error\":\"unsupported_protocol\",\"protocol\":1}\n"); return 0; }
		*helloDone = 1;
		if (!OBKA_SendEntities(sock) || !OBKA_SendSnapshot(sock)) { cJSON_Delete(root); return 0; }
	} else if (!strcmp(type->valuestring,"get_state")) {
		if (!OBKA_SendSnapshot(sock)) { cJSON_Delete(root); return 0; }
	}
	else if (!strcmp(type->valuestring,"ping")) OBKA_Send(sock,"{\"type\":\"pong\"}\n");
	else if (!strcmp(type->valuestring,"set_state")) OBKA_ProcessSetState(sock, root);
	else if (!strcmp(type->valuestring,"restart")) {
		cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
		if (cJSON_IsNumber(id)) {
			commandResult_t result = CMD_ExecuteCommand("restart", 0);
			OBKA_Result(sock, id, result == CMD_RES_OK, result == CMD_RES_OK ? NULL : "restart_failed");
		} else OBKA_Result(sock, id, 0, "invalid_request");
	}
	else OBKA_Send(sock,"{\"type\":\"error\",\"error\":\"unsupported_type\"}\n");
	cJSON_Delete(root); return 1;
}

static void OBKA_ServerThread(void *param) {
	char rx[OBKA_RX_MAX], mac[24], name[96];
	int len, hello, seconds;
	unsigned int lastEnergyScanMs = 0;
	struct sockaddr_in addr;
	int reuse = 1;
	(void)param;
	addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(OBKA_PORT);
	g_obkaListenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_obkaListenSock < 0) goto done;
	setsockopt(g_obkaListenSock,SOL_SOCKET,SO_REUSEADDR,(const char*)&reuse,sizeof(reuse));
	if (bind(g_obkaListenSock,(struct sockaddr*)&addr,sizeof(addr)) || listen(g_obkaListenSock,1)) goto done;
	{ int flags=fcntl(g_obkaListenSock,F_GETFL,0); fcntl(g_obkaListenSock,F_SETFL,flags|O_NONBLOCK); }
	while (g_obkaRunning) {
		int client; struct sockaddr_storage source; socklen_t alen=sizeof(source);
		if (!Main_HasWiFiConnected()) { rtos_delay_milliseconds(250); continue; }
		client=accept(g_obkaListenSock,(struct sockaddr*)&source,&alen); if(client<0) { rtos_delay_milliseconds(50); continue; }
		{ int flags=fcntl(client,F_GETFL,0); fcntl(client,F_SETFL,flags|O_NONBLOCK); }
		HAL_GetMACStr(mac); OBKA_JSONString(name,sizeof(name),CFG_GetDeviceName());
		snprintf(g_obkaTx,sizeof(g_obkaTx),"{\"type\":\"hello\",\"protocol\":1,\"device_id\":\"%s\",\"name\":\"%s\",\"firmware\":\"%s\",\"platform\":\"%s\"}\n",mac,name,USER_SW_VER,PLATFORM_MCU_NAME);
		if (!OBKA_Send(client,g_obkaTx)) { close(client); continue; }
		len=0; hello=0; seconds=0;
		while(g_obkaRunning && Main_HasWiFiConnected()) {
			int got=recv(client,rx+len,sizeof(rx)-len-1,0);
			if(got>0) { seconds=0; int start=0,i; len+=got; rx[len]=0; for(i=0;i<len;i++) if(rx[i]=='\n') { rx[i]=0; if(!OBKA_ProcessLine(client,rx+start,&hello)) goto close_client; start=i+1; } if(start) { memmove(rx,rx+start,len-start); len-=start; } if(len>=sizeof(rx)-1) { OBKA_Send(client,"{\"type\":\"error\",\"error\":\"packet_too_large\"}\n"); goto close_client; } }
			else if(got==0 || (got<0 && errno!=EAGAIN && errno!=EWOULDBLOCK)) goto close_client;
			if (hello && (unsigned int)(rtos_get_time() - lastEnergyScanMs) >= 1000) {
				lastEnergyScanMs = rtos_get_time();
				OBKA_ScanEnergy();
			}
			if(hello && !OBKA_SendPending(client)) goto close_client;
			rtos_delay_milliseconds(10); if (++seconds >= 12000) goto close_client; if (hello && seconds == 6000 && !OBKA_Send(client,"{\"type\":\"ping\"}\n")) goto close_client;
		}
	close_client: close(client);
	}
done:
	if(g_obkaListenSock!=OBKA_INVALID_SOCK) close(g_obkaListenSock); g_obkaListenSock=OBKA_INVALID_SOCK;
	rtos_suspend_thread(NULL);
}

void DRV_OpenBeken_API_Init(void) { OSStatus e; DRV_MDNS_RegisterOpenBekenAPI(OBKA_PORT); g_obkaRunning=1; e=rtos_create_thread(&g_obkaThread,BEKEN_APPLICATION_PRIORITY-1,"OBKA_Srv",(beken_thread_function_t)OBKA_ServerThread,5120,0); if(e!=kNoErr) { g_obkaRunning=0; ADDLOG_ERROR(LOG_FEATURE_DRV,"OpenBekenAPI: server thread failed %d",e); } }
void DRV_OpenBeken_API_Deinit(void) { g_obkaRunning=0; if(g_obkaListenSock!=OBKA_INVALID_SOCK) close(g_obkaListenSock); if(g_obkaThread) { rtos_delete_thread(&g_obkaThread); g_obkaThread=NULL; } }
void DRV_OpenBeken_API_OnEverySecond(void) { }
void DRV_OpenBeken_API_OnChannelChanged(int channel,int value) {
	(void)value;
	OBKA_MarkChannel(channel);
	if (channel >= 0 && channel < 5 &&
		CHANNEL_HasChannelPinWithRoleOrRole(channel, IOR_PWM, IOR_PWM_n)) {
		g_obkaLightDirty = 1;
	}
}
void DRV_OpenBeken_API_OnLightChanged(void) { g_obkaLightDirty=1; g_obkaLastChangeMs=rtos_get_time(); }
int DRV_OpenBeken_API_GetPort(void) { return OBKA_PORT; }
#else
void DRV_OpenBeken_API_Init(void) {} void DRV_OpenBeken_API_Deinit(void) {} void DRV_OpenBeken_API_OnEverySecond(void) {} void DRV_OpenBeken_API_OnChannelChanged(int c,int v) {(void)c;(void)v;} void DRV_OpenBeken_API_OnLightChanged(void) {} int DRV_OpenBeken_API_GetPort(void) { return 0; }
#endif
