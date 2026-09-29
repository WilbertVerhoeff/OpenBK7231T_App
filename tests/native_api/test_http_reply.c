/* Exercise the production reply writer, including valid lwIP socket zero. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "new_http.h"

static char captured[4096];
static size_t captured_len;
static int expected_fd, send_calls;
static int test_send(int fd, const void *data, size_t len, int flags) {
	assert(fd == expected_fd && fd >= 0 && flags == 0);
	assert(captured_len + len <= sizeof(captured));
	memcpy(captured + captured_len, data, len);
	captured_len += len;
	send_calls++;
	return (int)len;
}
#define send test_send
static void rtos_delay_milliseconds(int ms) { (void)ms; }
#include "http_reply_body.inc"

static void check_socket(int fd) {
	char buffer[64], payload[1024];
	http_request_t request = {0};
	request.fd = expected_fd = fd;
	request.reply = buffer;
	request.replymaxlen = sizeof(buffer) - 1;
	captured_len = send_calls = 0;
	postany(&request, "small JSON response", 19);
	postany(&request, NULL, 0);
	assert(send_calls == 1 && captured_len == 19);
	assert(!memcmp(captured, "small JSON response", 19));
	assert(request.replylen == 0);
	postany(&request, NULL, 0);
	assert(send_calls == 1);
	for (unsigned int i = 0; i < sizeof(payload); i++) payload[i] = (char)(i % 127);
	captured_len = send_calls = 0;
	postany(&request, payload, sizeof(payload));
	postany(&request, "tail", 4);
	postany(&request, NULL, 0);
	assert(captured_len == sizeof(payload) + 4);
	assert(!memcmp(captured, payload, sizeof(payload)));
	assert(!memcmp(captured + sizeof(payload), "tail", 4));
	assert(request.replylen == 0);
}

int main(void) {
	check_socket(0);
	check_socket(3);
	char buffer[64];
	http_request_t request = {0};
	request.fd = HTTP_INVALID_SOCKET;
	request.reply = buffer;
	request.replymaxlen = sizeof(buffer) - 1;
	int previous_calls = send_calls;
	postany(&request, "fake", 4);
	assert(postany(&request, NULL, 0) == 4);
	assert(request.replylen == 4 && send_calls == previous_calls);
	puts("PASS HTTP replies: socket zero, positive socket, complete large response, empty flush and explicit test socket");
	return 0;
}
