#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef signed char s8_t;
struct netif { int unused; };
#define DNSSD_PROTO_TCP 0
#define LOG_ERROR 0
#define LOG_FEATURE_HTTP 0
#define addLogAdv(...) ((void)0)
static int used[MDNS_MAX_SERVICES], deleted, fail_api;
static s8_t g_mdnsServiceSlot = -1, g_mdnsOpenBekenServiceSlot = -1;
static int g_mdnsOpenBekenPort;
static void DRV_MDNS_OpenBekenTXT(void) {}
static int mdns_resp_add_service(struct netif *netif, const char *host, const char *name,
	int protocol, int port, int ttl, void *callback, void *arg) {
	(void)netif; (void)host; (void)protocol; (void)port; (void)ttl; (void)callback; (void)arg;
	if (fail_api && !strcmp(name, "_openbeken")) return -1;
	for (int i = 0; i < MDNS_MAX_SERVICES; i++) if (!used[i]) { used[i] = !strcmp(name, "_http") ? 1 : 2; return i; }
	return -1;
}
static void mdns_resp_del_service(struct netif *netif, int slot) {
	(void)netif; assert(slot >= 0 && slot < MDNS_MAX_SERVICES); used[slot] = 0; deleted++;
}
#include "mdns_services.inc"
int main(void) {
	struct netif netif;
	DRV_MDNS_UpdateServices(&netif, "test"); assert(g_mdnsServiceSlot == 0 && used[0] == 1);
	g_mdnsOpenBekenPort = 6054; fail_api = 1;
	DRV_MDNS_UpdateServices(&netif, "test"); assert(used[0] == 1 && !deleted);
	fail_api = 0; DRV_MDNS_UpdateServices(&netif, "test"); assert(used[0] == 1);
#if MDNS_MAX_SERVICES >= 2
	assert(g_mdnsOpenBekenServiceSlot == 1 && used[1] == 2);
	DRV_MDNS_UpdateServices(&netif, "test"); assert(!deleted);
	g_mdnsOpenBekenPort = 0; DRV_MDNS_UpdateServices(&netif, "test");
	assert(used[0] == 1 && used[1] == 0 && deleted == 1 && g_mdnsOpenBekenServiceSlot == -1);
	g_mdnsOpenBekenPort = 6054; DRV_MDNS_UpdateServices(&netif, "test"); assert(used[1] == 2);
#else
	assert(g_mdnsOpenBekenServiceSlot == -1 && !deleted);
#endif
	printf("PASS mDNS: %d slots, HTTP retained, API failure/stop/restart\n", MDNS_MAX_SERVICES);
	return 0;
}
