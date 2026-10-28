/*
HTTP_GET.C

One HTTP GET, polled (http_get.h): the host looked up, the connection
made, "GET <path> HTTP/1.0" sent with Host and User-Agent, the answer's
head read to its blank line (status, Content-Length, Location), then the
body handed to the caller until the connection closes or Content-Length is
reached. A 301, 302, 303, 307 or 308 to another http:// address (or a path
on the same host) starts again there, five times at most.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_get.h"

/* how long a step may take: the host's lookup and the connection, and the
silence of a server mid-answer */
#define HTTP_CONNECT_MS 15000
#define HTTP_SILENCE_MS 30000
#define HTTP_MAXIMUM_REDIRECTS 5

static void fail(struct http_get *get, const char *reason)
{
	retail_socket_close(get->socket);
	get->socket = RETAIL_NO_SOCKET;
	snprintf(get->error, sizeof(get->error), "%s", reason);
	get->state = _http_get_failed;
}

static void set_state(struct http_get *get, int state, unsigned long now_ms)
{
	get->state = state;
	get->state_ms = now_ms;
	get->last_data_ms = now_ms;
}

/* url into host, port and path: 0, or -1 (an https:// one: -2) */
static int parse_url(struct http_get *get, const char *url)
{
	const char *host, *slash, *colon;
	int length;

	if (!strncmp(url, "https://", 8))
		return -2;
	if (strncmp(url, "http://", 7))
		return -1;
	host = url + 7;
	slash = strchr(host, '/');
	length = slash ? (int)(slash - host) : (int)strlen(host);
	if (length <= 0 || length >= (int)sizeof(get->host))
		return -1;
	snprintf(get->host, sizeof(get->host), "%.*s", length, host);
	get->port = 80;
	colon = strchr(get->host, ':');
	if (colon)
	{
		get->port = (uint16_t)atoi(colon + 1);
		get->host[colon - get->host] = 0;
	}
	snprintf(get->path, sizeof(get->path), "%s", slash ? slash : "/");
	return 0;
}

/* (re)starts at the url: 0, or -1 */
static int start(struct http_get *get, const char *url, unsigned long now_ms)
{
	int parsed = parse_url(get, url);

	retail_socket_close(get->socket);
	get->socket = RETAIL_NO_SOCKET;
	get->head_size = 0;
	get->status = 0;
	get->content_length = -1;
	get->received = 0;
	if (parsed == -2)
	{
		fail(get, "the site moved to https, which this Halo cannot fetch");
		return -1;
	}
	if (parsed < 0)
	{
		fail(get, "not an http:// address");
		return -1;
	}
	get->request_size = snprintf(get->request, sizeof(get->request),
		"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n\r\n", get->path, get->host,
		get->user_agent);
	if (get->request_size <= 0 || get->request_size >= (int)sizeof(get->request))
	{
		fail(get, "the address is too long");
		return -1;
	}
	get->request_sent = 0;
	get->address = 0;
	set_state(get, _http_get_resolving, now_ms);
	return 0;
}

int http_get_begin(struct http_get *get, const char *url, const char *user_agent, unsigned long now_ms)
{
	memset(get, 0, sizeof(*get));
	get->socket = RETAIL_NO_SOCKET;
	snprintf(get->user_agent, sizeof(get->user_agent), "%s", user_agent ? user_agent : "Halo");
	return start(get, url, now_ms);
}

void http_get_cancel(struct http_get *get)
{
	retail_socket_close(get->socket);
	get->socket = RETAIL_NO_SOCKET;
	get->state = _http_get_off;
}

/* whether text starts with prefix, whatever their letters' case */
static int starts_with(const char *text, const char *prefix)
{
	for (; *prefix; text++, prefix++)
	{
		int a = *text >= 'A' && *text <= 'Z' ? *text - 'A' + 'a' : *text;
		int b = *prefix >= 'A' && *prefix <= 'Z' ? *prefix - 'A' + 'a' : *prefix;

		if (a != b)
			return 0;
	}
	return 1;
}

/* a header's value in the head read so far (name with its colon), into
value: 1 if it is there */
static int header_value(const char *head, const char *name, char *value, int size)
{
	const char *line = head;

	while ((line = strstr(line, "\r\n")) != NULL)
	{
		line += 2;
		if (starts_with(line, name))
		{
			const char *at = line + strlen(name), *end;

			while (*at == ' ' || *at == '\t')
				at++;
			end = strstr(at, "\r\n");
			if (!end)
				return 0;
			snprintf(value, (size_t)size, "%.*s", (int)(end - at), at);
			return 1;
		}
	}
	return 0;
}

/* the head complete (its blank line read): the status and the headers
taken, a redirect followed; the bytes past it are the body's first */
static int head_complete(struct http_get *get, int body_at, unsigned long now_ms, http_get_writer *write,
	void *context)
{
	char value[512];

	get->head[body_at - 4] = 0;
	if (sscanf(get->head, "HTTP/%*d.%*d %d", &get->status) != 1)
	{
		fail(get, "the server's answer is not HTTP");
		return -1;
	}
	if (get->status == 301 || get->status == 302 || get->status == 303 || get->status == 307 || get->status == 308)
	{
		char url[640];

		if (++get->redirects > HTTP_MAXIMUM_REDIRECTS || !header_value(get->head, "Location:", value, sizeof(value)))
		{
			fail(get, "the server sent us round in circles");
			return -1;
		}
		/* (a path on the same host, or another address) */
		if (value[0] == '/')
			snprintf(url, sizeof(url), "http://%s:%u%s", get->host, (unsigned)get->port, value);
		else
			snprintf(url, sizeof(url), "%s", value);
		return start(get, url, now_ms) < 0 ? -1 : 1;
	}
	if (get->status != 200)
	{
		char reason[64];

		snprintf(reason, sizeof(reason), get->status == 404 ? "not there (404)" : "the server answered %d",
			get->status);
		fail(get, reason);
		return -1;
	}
	if (header_value(get->head, "Content-Length:", value, sizeof(value)))
		get->content_length = atol(value);
	set_state(get, _http_get_body, now_ms);
	/* (what came after the head) */
	if (get->head_size > body_at)
	{
		int extra = get->head_size - body_at;

		get->received += extra;
		if (write(context, (const unsigned char *)get->head + body_at, extra))
		{
			fail(get, "the download was stopped");
			return -1;
		}
	}
	return 0;
}

int http_get_poll(struct http_get *get, unsigned long now_ms, http_get_writer *write, void *context)
{
	unsigned char buffer[8192];
	int got, loops;

	switch (get->state)
	{
	case _http_get_resolving:
		get->address = retail_resolve(get->host);
		if (get->address)
		{
			get->socket = retail_socket_open(1);
			if (get->socket == RETAIL_NO_SOCKET || retail_socket_connect(get->socket, get->address, get->port) < 0)
			{
				fail(get, "no connection could be opened");
				break;
			}
			set_state(get, _http_get_connecting, now_ms);
		}
		else if (now_ms - get->state_ms > HTTP_CONNECT_MS)
			fail(get, "the server's name was not found");
		break;
	case _http_get_connecting:
	{
		/* (the request's first send waits for the connection: retail_socket.h) */
		int sent = retail_socket_send(get->socket, get->request + get->request_sent,
			get->request_size - get->request_sent);

		if (sent > 0)
		{
			get->request_sent += sent;
			if (get->request_sent >= get->request_size)
				set_state(get, _http_get_head, now_ms);
		}
		else if (!retail_socket_pending())
			fail(get, "the connection was refused");
		else if (now_ms - get->state_ms > HTTP_CONNECT_MS)
			fail(get, "the server did not answer");
		break;
	}
	case _http_get_head:
	case _http_get_body:
		/* (as much as has come, a few reads a frame) */
		for (loops = 0; loops < 64 && (get->state == _http_get_head || get->state == _http_get_body); loops++)
		{
			if (get->state == _http_get_head)
			{
				char *end;

				got = retail_socket_receive(get->socket, get->head + get->head_size,
					(int)sizeof(get->head) - 1 - get->head_size);
				if (got <= 0)
					goto quiet;
				get->last_data_ms = now_ms;
				get->head_size += got;
				get->head[get->head_size] = 0;
				end = strstr(get->head, "\r\n\r\n");
				if (end)
				{
					if (head_complete(get, (int)(end - get->head) + 4, now_ms, write, context))
						break;
				}
				else if (get->head_size >= (int)sizeof(get->head) - 1)
				{
					fail(get, "the server's answer is too long");
					break;
				}
				continue;
			}
			got = retail_socket_receive(get->socket, buffer, (int)sizeof(buffer));
			if (got <= 0)
				goto quiet;
			get->last_data_ms = now_ms;
			get->received += got;
			if (write(context, buffer, got))
			{
				fail(get, "the download was stopped");
				break;
			}
			if (get->content_length >= 0 && get->received >= get->content_length)
			{
				retail_socket_close(get->socket);
				get->socket = RETAIL_NO_SOCKET;
				get->state = _http_get_done;
			}
			continue;
quiet:
			/* (nothing now; the connection's end; a silence too long) */
			if (got == 0 || !retail_socket_pending())
			{
				if (get->state == _http_get_body && (get->content_length < 0 || get->received >= get->content_length))
				{
					retail_socket_close(get->socket);
					get->socket = RETAIL_NO_SOCKET;
					get->state = _http_get_done;
				}
				else
					fail(get, get->state == _http_get_head ? "the server closed the connection" :
						"the connection was lost before the end");
			}
			else if (now_ms - get->last_data_ms > HTTP_SILENCE_MS)
				fail(get, "the server stopped sending");
			break;
		}
		break;
	}
	return get->state;
}
