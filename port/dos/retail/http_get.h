/*
HTTP_GET.H

One HTTP GET at a time, over the retail list's sockets (retail_socket.h:
mTCP on DOS, WSOCK32 on Windows 95), never waiting: http_get_poll() from
the game's frame moves it on and hands the body over as it comes. HTTP/1.0
(no chunks), redirects followed; http:// only (neither system can speak
TLS), so a site that moved to https:// is an error said as such.
*/

#ifndef __HTTP_GET_H
#define __HTTP_GET_H

#include <stdint.h>

#include "retail_socket.h"

enum
{
	_http_get_off,
	_http_get_resolving,
	_http_get_connecting,
	_http_get_head, /* the request sent, the answer's head being read */
	_http_get_body,
	_http_get_done,
	_http_get_failed /* error[] says why; status the HTTP status if one came */
};

/* the body's bytes as they come: 0 to go on, else the transfer is given up
(error[] the callback's) */
typedef int http_get_writer(void *context, const unsigned char *data, int size);

struct http_get
{
	int state;
	char host[96];
	uint16_t port;
	char path[512];
	char user_agent[48];
	uint32_t address;
	retail_socket_t socket;
	char request[768];
	int request_size, request_sent;
	char head[4096];
	int head_size;
	int status;
	/* the body's length (-1 when the server does not say) and how much came */
	long content_length, received;
	int redirects;
	unsigned long state_ms, last_data_ms;
	char error[96];
};

/* starts fetching url ("http://host[:port]/path"): 0, or -1 (error[]) */
int http_get_begin(struct http_get *get, const char *url, const char *user_agent, unsigned long now_ms);
/* moves it on: its state */
int http_get_poll(struct http_get *get, unsigned long now_ms, http_get_writer *write, void *context);
void http_get_cancel(struct http_get *get);

#endif
