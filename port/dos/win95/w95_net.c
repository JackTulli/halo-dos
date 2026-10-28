/*
W95_NET.C

The sockets of the Windows 95 build: the socket half of ChupathingyCE's
port/linux/src/posix.h (posix_socket_*, which its Xbox Winsock, xnet.c, is
written over) on Windows 95's own Winsock 1.1 (WSOCK32.DLL), which every
later Windows still has. Its port/windows/src/win32_net.c is the model;
what Windows 95 lacks changes this much:

- sockets come from socket() (WSASocket is Winsock 2's);
- Windows 2000 and later report an ICMP port unreachable (a datagram to a
  machine nothing listens on) as WSAECONNRESET from a UDP socket's next
  recvfrom, which Winsock 1.1 cannot turn off (WSAIoctl is Winsock 2's):
  such a recvfrom is retried, never passed to the game, which would take
  it for its connection lost;
- names are looked up with gethostbyname, and this machine's address is
  the one a UDP socket gets when it is connected towards the internet
  (nothing is sent), else the first its host name has;
- random bytes come from CryptoAPI, loaded at run time (Windows 95 OSR2
  with Internet Explorer 3.02 and later), else from a hash of what varies
  between machines and moments.

The game polls its sockets from its one thread, so the last error is a
plain static.
*/

/* select() over as many sockets as a big game has */
#define FD_SETSIZE 256

#include <winsock.h>
#include <windows.h>
#include <wincrypt.h>

#include <stdlib.h>
#include <string.h>

#include "../../../chupathingyce/port/linux/src/posix.h"

static int last_error;
static int started;

static int fail(void)
{
	last_error = WSAGetLastError();
	return -1;
}

static int succeed(int result)
{
	if (result == SOCKET_ERROR)
		return fail();
	last_error = 0;
	return result;
}

static int from_socket(SOCKET socket)
{
	return socket == INVALID_SOCKET ? -1 : (int)socket;
}

static void start_winsock(void)
{
	if (!started)
	{
		WSADATA data;

		started = 1;
		WSAStartup(MAKEWORD(1, 1), &data);
	}
}

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	SOCKET result;

	start_winsock();
	result = socket(family, type, protocol);
	if (result == INVALID_SOCKET)
		return fail();
	last_error = 0;
	return from_socket(result);
}

int posix_socket_close(int socket)
{
	return succeed(closesocket((SOCKET)socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	return succeed(bind((SOCKET)socket, address, address_length));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	return succeed(connect((SOCKET)socket, address, address_length));
}

int posix_socket_listen(int socket, int backlog)
{
	return succeed(listen((SOCKET)socket, backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	SOCKET result = accept((SOCKET)socket, address, address_length);

	if (result == INVALID_SOCKET)
		return fail();
	last_error = 0;
	return from_socket(result);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	return succeed(send((SOCKET)socket, buffer, length, flags));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	return succeed(sendto((SOCKET)socket, buffer, length, flags, address, address_length));
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	return succeed(recv((SOCKET)socket, buffer, length, flags));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	int tries;

	/* (an ICMP port unreachable reported by a later Windows: the next
	datagram, if there is one, or WSAEWOULDBLOCK) */
	for (tries = 0; tries < 64; tries++)
	{
		int length_in = address_length ? *address_length : 0;
		int result = recvfrom((SOCKET)socket, buffer, length, flags, address, address_length);

		if (result != SOCKET_ERROR)
		{
			last_error = 0;
			return result;
		}
		if (WSAGetLastError() != WSAECONNRESET)
			return fail();
		if (address_length)
			*address_length = length_in;
	}
	last_error = WSAEWOULDBLOCK;
	return -1;
}

int posix_socket_shutdown(int socket, int how)
{
	return succeed(shutdown((SOCKET)socket, how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	u_long value = nonblocking ? 1 : 0;

	return succeed(ioctlsocket((SOCKET)socket, FIONBIO, &value));
}

int posix_socket_set_nodelay(int socket)
{
	BOOL value = TRUE;

	return succeed(setsockopt((SOCKET)socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&value, sizeof(value)));
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	u_long available = 0;
	int result = ioctlsocket((SOCKET)socket, FIONREAD, &available);

	if (result != SOCKET_ERROR)
		*count = (posix_ulong)available;
	return succeed(result);
}

/* SOL_SOCKET options Windows knows; the Xbox's own (SO_ENCRYPT and the
like) have nothing to do here */
static int known_option(int level, int name)
{
	if (level != SOL_SOCKET)
		return 1;
	switch (name)
	{
	case SO_REUSEADDR:
	case SO_KEEPALIVE:
	case SO_BROADCAST:
	case SO_LINGER:
	case SO_SNDBUF:
	case SO_RCVBUF:
	case SO_ERROR:
	case SO_TYPE:
		return 1;
	default:
		return 0;
	}
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	if (!known_option(level, name))
	{
		last_error = 0;
		return 0;
	}
	return succeed(setsockopt((SOCKET)socket, level, name, value, length));
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	if (!known_option(level, name))
	{
		last_error = WSAENOPROTOOPT;
		return -1;
	}
	return succeed(getsockopt((SOCKET)socket, level, name, value, length));
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	return succeed(getsockname((SOCKET)socket, address, address_length));
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	return succeed(getpeername((SOCKET)socket, address, address_length));
}

static int fill_set(fd_set *set, const int *descriptors, int count)
{
	int index, filled = 0;

	FD_ZERO(set);
	for (index = 0; index < count; index++)
	{
		if (descriptors[index] >= 0 && set->fd_count < FD_SETSIZE)
		{
			FD_SET((SOCKET)descriptors[index], set);
			filled++;
		}
	}
	return filled;
}

static void keep_ready(fd_set *set, int *descriptors, int *count)
{
	int index, kept = 0;

	for (index = 0; index < *count; index++)
	{
		if (descriptors[index] >= 0 && FD_ISSET((SOCKET)descriptors[index], set))
			descriptors[kept++] = descriptors[index];
	}
	*count = kept;
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	fd_set read_set, write_set, error_set;
	struct timeval timeout;
	int sockets = 0;
	int result;

	sockets += fill_set(&read_set, read, read ? *read_count : 0);
	sockets += fill_set(&write_set, write, write ? *write_count : 0);
	sockets += fill_set(&error_set, error, error ? *error_count : 0);
	if (!sockets)
	{
		/* Windows rejects a select() without sockets; POSIX waits */
		if (!infinite)
			Sleep((DWORD)(timeout_seconds * 1000 + timeout_microseconds / 1000));
		if (read)
			*read_count = 0;
		if (write)
			*write_count = 0;
		if (error)
			*error_count = 0;
		last_error = 0;
		return 0;
	}
	timeout.tv_sec = timeout_seconds;
	timeout.tv_usec = timeout_microseconds;
	result = select(0, read ? &read_set : NULL, write ? &write_set : NULL,
		error ? &error_set : NULL, infinite ? NULL : &timeout);
	if (result == SOCKET_ERROR)
		return fail();
	if (read)
		keep_ready(&read_set, read, read_count);
	if (write)
		keep_ready(&write_set, write, write_count);
	if (error)
		keep_ready(&error_set, error, error_count);
	last_error = 0;
	return result;
}

/* the address of the interface the default route leaves by (connecting a
UDP socket sends nothing), else the first the host name resolves to (a
network without a gateway) */
posix_ulong posix_local_ipv4_address(void)
{
	struct sockaddr_in remote, local;
	int length = sizeof(local);
	posix_ulong result = 0;
	SOCKET probe;
	char name[256];

	start_winsock();
	probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (probe != INVALID_SOCKET)
	{
		memset(&remote, 0, sizeof(remote));
		remote.sin_family = AF_INET;
		/* (192.0.2.1, RFC 5737's documentation network: never sent to) */
		remote.sin_addr.s_addr = htonl(0xC0000201UL);
		remote.sin_port = htons(9);
		memset(&local, 0, sizeof(local));
		if (connect(probe, (struct sockaddr *)&remote, sizeof(remote)) == 0 &&
			getsockname(probe, (struct sockaddr *)&local, &length) == 0 &&
			local.sin_addr.s_addr != htonl(INADDR_ANY) && (ntohl(local.sin_addr.s_addr) >> 24) != 127)
		{
			result = local.sin_addr.s_addr;
		}
		closesocket(probe);
	}
	if (!result && gethostname(name, sizeof(name)) == 0)
	{
		struct hostent *host = gethostbyname(name);
		int index;

		for (index = 0; host && host->h_addrtype == AF_INET && host->h_addr_list[index]; index++)
		{
			unsigned long address;

			memcpy(&address, host->h_addr_list[index], sizeof(address));
			if ((ntohl(address) >> 24) != 127 && address != htonl(INADDR_ANY))
			{
				result = address;
				break;
			}
		}
	}
	return result;
}

int posix_network_link_up(void)
{
	return posix_local_ipv4_address() != 0;
}

/* 64-bit mixing (SplitMix64's finaliser) of what varies between machines
and moments, chained from one call to the next: for a Windows 95 without
CryptoAPI */
static unsigned long long mix(unsigned long long value)
{
	value ^= value >> 30;
	value *= 0xBF58476D1CE4E5B9ULL;
	value ^= value >> 27;
	value *= 0x94D049BB133111EBULL;
	value ^= value >> 31;
	return value;
}

void posix_random_bytes(void *buffer, posix_ulong size)
{
	typedef BOOL (WINAPI *acquire_function)(HCRYPTPROV *provider, LPCSTR container, LPCSTR name, DWORD type,
		DWORD flags);
	typedef BOOL (WINAPI *generate_function)(HCRYPTPROV provider, DWORD size, BYTE *buffer);
	static int looked;
	static HCRYPTPROV provider;
	static generate_function generate;
	static unsigned long long chain;
	unsigned char *bytes = buffer;

	if (!looked)
	{
		HMODULE advapi = LoadLibraryA("advapi32.dll");
		acquire_function acquire = advapi ? (acquire_function)GetProcAddress(advapi, "CryptAcquireContextA") : NULL;
		generate_function found = advapi ? (generate_function)GetProcAddress(advapi, "CryptGenRandom") : NULL;

		looked = 1;
		if (acquire && found && acquire(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
			generate = found;
	}
	if (generate && generate(provider, (DWORD)size, bytes))
		return;
	while (size)
	{
		LARGE_INTEGER performance;
		FILETIME time;
		POINT cursor;
		MEMORYSTATUS memory;
		unsigned long long value;
		posix_ulong count = size < 8 ? size : 8;

		QueryPerformanceCounter(&performance);
		GetSystemTimeAsFileTime(&time);
		GetCursorPos(&cursor);
		memory.dwLength = sizeof(memory);
		GlobalMemoryStatus(&memory);
		value = chain ^ __builtin_ia32_rdtsc();
		value = mix(value ^ (unsigned long long)performance.QuadPart);
		value = mix(value ^ ((unsigned long long)time.dwHighDateTime << 32 | time.dwLowDateTime));
		value = mix(value ^ ((unsigned long long)GetCurrentProcessId() << 32 | GetTickCount()));
		value = mix(value ^ ((unsigned long long)(unsigned)cursor.x << 32 | (unsigned)cursor.y));
		value = mix(value ^ memory.dwAvailPhys ^ (unsigned long long)(unsigned long)&value << 32);
		chain = mix(value + 0x9E3779B97F4A7C15ULL);
		memcpy(bytes, &value, count);
		bytes += count;
		size -= count;
	}
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	unsigned long address;
	struct hostent *entry;

	start_winsock();
	address = inet_addr(host);
	if (address != INADDR_NONE || !strcmp(host, "255.255.255.255"))
		return address;
	entry = gethostbyname(host);
	if (!entry || entry->h_addrtype != AF_INET || !entry->h_addr_list[0])
		return 0;
	memcpy(&address, entry->h_addr_list[0], sizeof(address));
	return address;
}
