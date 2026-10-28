/*
RETAIL_SOCKET.H

The few socket calls the retail Halo PC / Custom Edition server list makes,
all non-blocking: the game's own socket layer (ChupathingyCE's posix.h:
port/dos/dos/dos_net.cpp over mTCP on DOS, port/dos/win95/w95_net.c over
WSOCK32 on Windows 95), or BSD sockets for the host test (Linux).

A stream's connection is not waited on: its first send is tried again each
poll (both layers answer "would block" or "not connected" until it is
made).
*/

#ifndef __RETAIL_SOCKET_H
#define __RETAIL_SOCKET_H

#include <stdint.h>
#include <string.h>

#ifdef HALO_DOS
/* (by path: the game's own units include this too, without the port's
source folder on their include path) */
#include "../../../chupathingyce/port/linux/src/platform.h"
#include "../../../chupathingyce/port/linux/src/posix.h"
#elif defined(_WIN32)
/* the host tests on Windows: Winsock under the BSD names (WSAStartup is
the test's) */
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#define RETAIL_WINSOCK 1
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* a host name's IPv4 address (network order), or 0: on DOS the lookup
waits a moment and may answer 0 until it is done (ask again) */
static __inline uint32_t retail_resolve(const char *host)
{
#ifdef HALO_DOS
	return (uint32_t)posix_resolve_ipv4(host);
#else
	struct hostent *h = gethostbyname(host);
	uint32_t address = 0;

	if (h && h->h_length == 4)
		memcpy(&address, h->h_addr, 4);
	return address;
#endif
}

typedef int retail_socket_t;
#define RETAIL_NO_SOCKET (-1)

/* a port in network order (both systems are little-endian) */
static __inline uint16_t retail_port_to_network(uint16_t port)
{
	return (uint16_t)((port & 0xFF) << 8 | port >> 8);
}

static __inline void retail_make_address(struct sockaddr_in *to, uint32_t address, uint16_t port)
{
	memset(to, 0, sizeof(*to));
	to->sin_family = AF_INET;
	to->sin_port = retail_port_to_network(port);
	to->sin_addr.s_addr = address;
}

/* a socket, non-blocking; RETAIL_NO_SOCKET when there is none */
static __inline retail_socket_t retail_socket_open(int stream)
{
#ifdef HALO_DOS
	int s = posix_socket(AF_INET, stream ? SOCK_STREAM : SOCK_DGRAM, 0);

	if (s < 0)
		return RETAIL_NO_SOCKET;
	if (posix_socket_set_nonblocking(s, 1) < 0)
	{
		posix_socket_close(s);
		return RETAIL_NO_SOCKET;
	}
	return s;
#elif defined(RETAIL_WINSOCK)
	SOCKET s = socket(AF_INET, stream ? SOCK_STREAM : SOCK_DGRAM, 0);
	u_long on = 1;

	if (s == INVALID_SOCKET)
		return RETAIL_NO_SOCKET;
	ioctlsocket(s, FIONBIO, &on);
	return (retail_socket_t)s;
#else
	int s = socket(AF_INET, stream ? SOCK_STREAM : SOCK_DGRAM, 0);

	if (s >= 0)
		fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	return s;
#endif
}

static __inline void retail_socket_close(retail_socket_t s)
{
	if (s == RETAIL_NO_SOCKET)
		return;
#ifdef HALO_DOS
	posix_socket_close(s);
#elif defined(RETAIL_WINSOCK)
	closesocket((SOCKET)s);
#else
	close(s);
#endif
}

/* the last call is to be tried again later: it would have blocked, or the
connection is not made yet */
static __inline int retail_socket_pending(void)
{
#ifdef HALO_DOS
	int e = posix_socket_last_error();

	return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY || e == WSAENOTCONN;
#elif defined(RETAIL_WINSOCK)
	int e = WSAGetLastError();

	return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY || e == WSAENOTCONN;
#else
	return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY || errno == ENOTCONN;
#endif
}

/* starts a connection: 0 started (or made), -1 failed */
static __inline int retail_socket_connect(retail_socket_t s, uint32_t address, uint16_t port)
{
	struct sockaddr_in to;
	int result;

	retail_make_address(&to, address, port);
#ifdef HALO_DOS
	result = posix_socket_connect(s, &to, sizeof(to));
#else
	result = connect(s, (struct sockaddr *)&to, sizeof(to));
#endif
	return result == 0 || retail_socket_pending() ? 0 : -1;
}

static __inline int retail_socket_send(retail_socket_t s, const void *data, int size)
{
#ifdef HALO_DOS
	return posix_socket_send(s, data, size, 0);
#else
	return (int)send(s, data, size, 0);
#endif
}

static __inline int retail_socket_receive(retail_socket_t s, void *data, int size)
{
#ifdef HALO_DOS
	return posix_socket_recv(s, data, size, 0);
#else
	return (int)recv(s, data, size, 0);
#endif
}

static __inline int retail_socket_send_to(retail_socket_t s, const void *data, int size, uint32_t address, uint16_t port)
{
	struct sockaddr_in to;

	retail_make_address(&to, address, port);
#ifdef HALO_DOS
	return posix_socket_sendto(s, data, size, 0, &to, sizeof(to));
#else
	return (int)sendto(s, data, size, 0, (struct sockaddr *)&to, sizeof(to));
#endif
}

/* a datagram and where it came from (its port in host order) */
static __inline int retail_socket_receive_from(retail_socket_t s, void *data, int size, uint32_t *address, uint16_t *port)
{
	struct sockaddr_in from;
	int got;
#ifdef HALO_DOS
	int from_size = sizeof(from);

	got = posix_socket_recvfrom(s, data, size, 0, &from, &from_size);
#elif defined(RETAIL_WINSOCK)
	int from_size = sizeof(from);

	got = (int)recvfrom((SOCKET)s, data, size, 0, (struct sockaddr *)&from, &from_size);
#else
	socklen_t from_size = sizeof(from);

	got = (int)recvfrom(s, data, size, 0, (struct sockaddr *)&from, &from_size);
#endif
	if (got >= 0)
	{
		*address = from.sin_addr.s_addr;
		*port = retail_port_to_network(from.sin_port);
	}
	return got;
}

#endif
