#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include "cJSON.h"
#include "channel_types.h"

#define ENABLE_DRIVER_OPENBEKEN_API 1
#define ENABLE_LED_BASIC 0
#define ENABLE_BL_SHARED 0
#define ENABLE_DRIVER_PIXELANIM 0
#define ENABLE_DRIVER_SM16703P 0
#define CHANNEL_MAX 64
#define PLATFORM_GPIO_MAX 32
#define OBK__NUM_SENSORS 6
#define OBK_VOLTAGE 0
#define OBK_CURRENT 1
#define OBK_POWER 2
#define OBK_FREQUENCY 3
#define OBK_CONSUMPTION_TOTAL 4
#define IOR_DS1820_IO 1
#define IOR_PWM 2
#define IOR_PWM_n 3
#define IS_PIN_DHT_ROLE(x) 0
#define IS_PIN_TEMP_HUM_SENSOR_ROLE(x) 0
#define USER_SW_VER "test-firmware"
#define PLATFORM_MCU_NAME "HOST_ADAPTER"
#define BEKEN_APPLICATION_PRIORITY 7
#define kNoErr 0
#define CMD_RES_OK 0
#define LOG_FEATURE_DRV 0
#define ADDLOG_ERROR(...) ((void)0)
typedef int OSStatus;
typedef int commandResult_t;
typedef void (*beken_thread_function_t)(void *);
struct test_thread { pthread_t handle; beken_thread_function_t fn; void *arg; };
typedef struct test_thread *xTaskHandle;
static pthread_mutex_t critical = PTHREAD_MUTEX_INITIALIZER;
static unsigned int fake_time;
static bool fake_clock;
static volatile int wifi = 1;
static int values[CHANNEL_MAX], types[CHANNEL_MAX], private_channels[CHANNEL_MAX];
static int mdns_port, fail_thread, restart_requests, inject_change;
static float led_temperature_min = 154, led_temperature_max = 500;
static struct { struct { int channels[32], channels2[32]; } pins; } g_cfg;
void DRV_OpenBeken_API_OnChannelChanged(int channel, int value);
void DRV_OpenBeken_API_OnLightChanged(void);
static void critical_restore(void) {
	pthread_mutex_unlock(&critical);
	if (inject_change) { inject_change = 0; DRV_OpenBeken_API_OnChannelChanged(2, 1); }
}
#define GLOBAL_INT_DECLARATION() ((void)0)
#define GLOBAL_INT_DISABLE() pthread_mutex_lock(&critical)
#define GLOBAL_INT_RESTORE() critical_restore()
static unsigned int rtos_get_time(void) {
	struct timespec now;
	if (fake_clock) return fake_time;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned int)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}
static void rtos_delay_milliseconds(int ms) { usleep(ms * 1000); }
static void *thread_entry(void *arg) {
	struct test_thread *thread = arg;
	thread->fn(thread->arg);
	return NULL;
}
static int rtos_create_thread(xTaskHandle *out, int priority, const char *name,
	beken_thread_function_t fn, int stack, void *arg) {
	(void)priority; (void)name; (void)stack;
	if (fail_thread) return -1;
	*out = calloc(1, sizeof(**out)); assert(*out);
	(*out)->fn = fn; (*out)->arg = arg;
	assert(!pthread_create(&(*out)->handle, NULL, thread_entry, *out));
	return 0;
}
static void rtos_delete_thread(xTaskHandle *thread) {
	assert(!pthread_join((*thread)->handle, NULL)); free(*thread); *thread = NULL;
}
static void rtos_suspend_thread(void *unused) { (void)unused; }
int Main_HasWiFiConnected(void) { return wifi; }
static void HAL_GetMACStr(char *out) { strcpy(out, "AABBCCDDEEFF"); }
static const char *CFG_GetDeviceName(void) { return "API test"; }
static void DRV_MDNS_RegisterOpenBekenAPI(int port) { mdns_port = port; }
static int CHANNEL_IsInUse(int channel) { return channel >= 0 && channel < 3; }
static int CHANNEL_HasNeverPublishFlag(int channel) { return private_channels[channel]; }
static int CHANNEL_GetType(int channel) { return types[channel]; }
static int PIN_GetPinRoleForPinIndex(int pin) { (void)pin; return 0; }
static int CHANNEL_Get(int channel) { return values[channel]; }
static float CHANNEL_GetFloat(int channel) { return values[channel]; }
static const char *CHANNEL_GetLabel(int channel) { (void)channel; return "relay"; }
static int CHANNEL_IsPowerRelayChannel(int channel) { return channel == 1 || channel == 2; }
static int CHANNEL_HasChannelPinWithRoleOrRole(int channel, int a, int b) { (void)channel; (void)a; (void)b; return 0; }
static void CHANNEL_Set(int channel, int value, int flags) {
	(void)flags; values[channel] = value; DRV_OpenBeken_API_OnChannelChanged(channel, value);
}
static int CMD_ExecuteCommand(const char *command, int flags) {
	(void)flags; assert(!strcmp(command, "restart")); restart_requests++; return 0;
}
static void strcpy_safe(char *out, const char *in, int size) { snprintf(out, size, "%s", in); }
#include "native_api_body.inc"

static cJSON *read_json(int sock) {
	char line[10000]; unsigned int n = 0;
	while (n < sizeof(line) - 1) {
		int got = recv(sock, line + n, 1, 0);
		assert(got == 1);
		if (line[n++] == '\n') break;
	}
	line[n] = 0;
	cJSON *json = cJSON_Parse(line); assert(json); return json;
}
static void expect_type(int sock, const char *type) {
	cJSON *json = read_json(sock);
	assert(!strcmp(cJSON_GetObjectItem(json, "type")->valuestring, type));
	cJSON_Delete(json);
}
static void expect_error(int sock, const char *error) {
	cJSON *json = read_json(sock);
	assert(!strcmp(cJSON_GetObjectItem(json, "error")->valuestring, error));
	cJSON_Delete(json);
}
static int process(int sock, const char *request, int *hello) {
	char line[1100]; snprintf(line, sizeof(line), "%s", request);
	return OBKA_ProcessLine(sock, line, hello);
}
static void protocol_tests(void) {
	int sockets[2], hello = 0;
	g_obkaRunning = 1;
	assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
	assert(!process(sockets[0], "{\"type\":\"hello\",\"protocol\":1.5}", &hello));
	expect_error(sockets[1], "unsupported_protocol"); assert(!hello);
	assert(!process(sockets[0], "{\"type\":\"hello\",\"protocol\":\"1\"}", &hello));
	expect_error(sockets[1], "unsupported_protocol");
	assert(process(sockets[0], "{\"type\":\"hello\",\"protocol\":1}", &hello));
	expect_type(sockets[1], "entities"); expect_type(sockets[1], "state");
	assert(process(sockets[0], "{", &hello)); expect_error(sockets[1], "malformed_json");
	assert(process(sockets[0], "{\"type\":\"unknown\"}", &hello)); expect_error(sockets[1], "unsupported_type");
	assert(process(sockets[0], "{\"type\":\"set_state\",\"id\":1.5,\"entity\":\"switch_1\",\"state\":{\"on\":true}}", &hello));
	expect_error(sockets[1], "invalid_request"); assert(!values[1]);
	assert(process(sockets[0], "{\"type\":\"set_state\",\"id\":1,\"entity\":\"switch_1\",\"state\":{\"on\":\"true\"}}", &hello));
	expect_error(sockets[1], "unsupported_feature"); assert(!values[1]);
	private_channels[1] = 1;
	assert(process(sockets[0], "{\"type\":\"set_state\",\"id\":1,\"entity\":\"switch_1\",\"state\":{\"on\":true}}", &hello));
	expect_error(sockets[1], "unknown_entity"); assert(!values[1]); private_channels[1] = 0;
	assert(process(sockets[0], "{\"type\":\"set_state\",\"id\":1,\"entity\":\"switch_1\",\"state\":{\"on\":true}}", &hello));
	expect_type(sockets[1], "result"); assert(values[1]);
	assert(process(sockets[0], "{\"type\":\"ping\"}", &hello)); expect_type(sockets[1], "pong");
	assert(process(sockets[0], "{\"type\":\"pong\"}", &hello));
	assert(!OBKA_ValidDepth("[[[[[[[[[0]]]]]]]]]"));
	assert(OBKA_ValidDepth("{\"quoted\":\"[[[[[[[[[[\"}"));
	assert(process(sockets[0], "{\"type\":\"restart\",\"id\":2.5}", &hello)); expect_error(sockets[1], "invalid_request");
	assert(!restart_requests);
	assert(process(sockets[0], "{\"type\":\"restart\",\"id\":2}", &hello)); expect_type(sockets[1], "result"); assert(restart_requests == 1);
	close(sockets[0]); close(sockets[1]);
	puts("PASS protocol: strict hello, commands, errors, private channels, ping/pong, depth");
}
static void pending_tests(void) {
	int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
	fake_clock = true; fake_time = 100; g_obkaRunning = 1;
	g_obkaPending = 0; g_obkaChangedChannels[0] = g_obkaChangedChannels[1] = 0;
	DRV_OpenBeken_API_OnChannelChanged(1, 1);
	fake_time = 130; DRV_OpenBeken_API_OnChannelChanged(1, 1);
	assert(g_obkaLastChangeMs == 100);
	fake_time = 141; inject_change = 1;
	assert(OBKA_SendPending(sockets[0])); expect_type(sockets[1], "state_changed");
	assert(g_obkaPending && (g_obkaChangedChannels[0] & (1u << 2)));
	fake_time = 182; assert(OBKA_SendPending(sockets[0])); expect_type(sockets[1], "state_changed"); assert(!g_obkaPending);
	fake_time = UINT32_MAX - 10; DRV_OpenBeken_API_OnChannelChanged(1, 1);
	fake_time = 35; assert(OBKA_SendPending(sockets[0])); expect_type(sockets[1], "state_changed");
	fake_clock = false; g_obkaRunning = 0; close(sockets[0]); close(sockets[1]);
	puts("PASS batching: continuous changes, callback after snapshot, clock wrap");
}
static int connect_server(void) {
	struct sockaddr_in address = {0};
	int sock = socket(AF_INET, SOCK_STREAM, 0); assert(sock >= 0);
	struct timeval timeout = {2, 0}; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	address.sin_family = AF_INET; address.sin_port = htons(6054); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(!connect(sock, (struct sockaddr *)&address, sizeof(address)));
	return sock;
}
static void lifecycle_tests(void) {
	int i, sock;
	fail_thread = 1; DRV_OpenBeken_API_Init(); DRV_OpenBeken_API_OnEverySecond(); assert(!mdns_port && g_obkaExited); fail_thread = 0;
	for (i = 0; i < 10; i++) {
		DRV_OpenBeken_API_Init();
		for (int j = 0; j < 200 && !g_obkaListening && !g_obkaExited; j++) usleep(1000);
		assert(g_obkaListening); DRV_OpenBeken_API_OnEverySecond(); assert(mdns_port == 6054);
		sock = connect_server(); expect_type(sock, "hello");
		assert(send(sock, "{\"type\":\"hello\",\"protocol\":1}\n", 30, 0) == 30);
		expect_type(sock, "entities"); expect_type(sock, "state");
		DRV_OpenBeken_API_Deinit(); assert(g_obkaExited && !g_obkaThread && !g_obkaListening && !mdns_port);
		char byte; assert(recv(sock, &byte, 1, 0) == 0); close(sock);
	}
	puts("PASS lifecycle: thread failure, 10 starts/stops, active client EOF, discovery withdrawal");
}
static void framing_tests(void) {
	int sock;
	char byte, oversized[1024];
	DRV_OpenBeken_API_Init();
	for (int i = 0; i < 200 && !g_obkaListening; i++) usleep(1000);
	assert(g_obkaListening);
	sock = connect_server(); expect_type(sock, "hello");
	const char *hello = "{\"type\":\"hello\",\"protocol\":1}\n";
	/* A TCP read need not contain a complete JSON message. */
	for (size_t i = 0; i < strlen(hello); i++) {
		assert(send(sock, hello + i, 1, 0) == 1); usleep(1000);
	}
	expect_type(sock, "entities"); expect_type(sock, "state");
	const char *requests = "{\"type\":\"ping\"}\n{\"type\":\"get_state\"}\n";
	assert(send(sock, requests, strlen(requests), 0) == (int)strlen(requests));
	expect_type(sock, "pong"); expect_type(sock, "state"); close(sock);
	sock = connect_server(); expect_type(sock, "hello");
	memset(oversized, ' ', sizeof(oversized));
	assert(send(sock, oversized, sizeof(oversized), 0) == sizeof(oversized));
	expect_error(sock, "packet_too_large"); assert(recv(sock, &byte, 1, 0) == 0); close(sock);
	sock = connect_server(); expect_type(sock, "hello");
	assert(send(sock, "{}\0\n", 4, 0) == 4);
	expect_error(sock, "malformed_json"); assert(recv(sock, &byte, 1, 0) == 0); close(sock);
	sock = connect_server(); expect_type(sock, "hello");
	wifi = 0; assert(recv(sock, &byte, 1, 0) == 0); close(sock); wifi = 1;
	sock = connect_server(); expect_type(sock, "hello"); close(sock);
	DRV_OpenBeken_API_Deinit();
	assert(!OBKA_Send(-1, "ignored"));
	/* A occupied port must not be advertised as a working API. */
	int blocker = socket(AF_INET, SOCK_STREAM, 0);
	int reuse = 1; setsockopt(blocker, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	struct sockaddr_in address = {0}; address.sin_family = AF_INET;
	address.sin_port = htons(6054); address.sin_addr.s_addr = htonl(INADDR_ANY);
	assert(!bind(blocker, (struct sockaddr *)&address, sizeof(address))); assert(!listen(blocker, 1));
	DRV_OpenBeken_API_Init();
	for (int i = 0; i < 200 && !g_obkaExited; i++) usleep(1000);
	assert(g_obkaExited && !g_obkaListening); DRV_OpenBeken_API_OnEverySecond(); assert(!mdns_port);
	DRV_OpenBeken_API_Deinit(); close(blocker);
	puts("PASS TCP: fragmented/coalesced messages, oversize, NUL, Wi-Fi recovery, bind failure");
}
int main(void) {
	for (int i = 0; i < CHANNEL_MAX; i++) types[i] = ChType_Default;
	protocol_tests(); pending_tests(); lifecycle_tests(); framing_tests();
	puts("Native API C checks passed with AddressSanitizer and UndefinedBehaviorSanitizer.");
	return 0;
}
