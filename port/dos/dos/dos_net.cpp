/*
DOS_NET.CPP

The sockets of the DOS build: the socket half of ChupathingyCE's
port/linux/src/posix.h (posix_socket_*, which its Xbox Winsock, xnet.c, is
written over) on mTCP (port/dos/mtcp), Michael Brutman's TCP/IP stack for
DOS, built into the program and talking to the network card's packet
driver. So the DOS build plays system link with ChupathingyCE's PC builds,
the Windows 95 build and other DOS machines on the LAN.

- The stack starts the first time the game asks for the network: the
  packet driver found on a software interrupt from 0x60 to 0x80 (or the
  setting net_packet_int), the address from the LAN's DHCP server (built
  in: no DHCP.EXE to run first), or net_address, net_netmask,
  net_gateway, net_dns in HALODOS.CFG; an mTCP configuration file named by
  MTCPCFG gives them too. With no packet driver the game plays without a
  network, as before.
- UDP sockets keep the datagrams for their port in a queue; TCP sockets are
  mTCP's (TcpSocket), listening ones given the connections made to their
  port.
- mTCP has no loopback interface, and a host's own client joins its game
  through 127.0.0.1: traffic to 127.x.x.x or to this machine's own address
  is passed between the sockets here (datagrams to the bound port's queue,
  a stream to a pair of in-memory rings).
- Everything is polled: each call the game makes drives the stack (the
  packets the receiver queued, TCP's retransmissions, ARP), as does a
  select, which waits by driving it.
- The packet driver is let go when the program exits (dos_at_exit), or it
  would call a receiver that is no longer there.
*/

/* (the platform's structures as the C units lay them out, not as mTCP's
packing, halo_mtcp.h) */
#pragma pack(push, 8)
extern "C" {
#include "../../../chupathingyce/port/linux/src/posix.h"
#include "dos_dpmi.h"
#include "halo_system.h"

const char *dos_setting(const char *name, const char *default_value);
uint8_t mtcp_find_packet_driver(void);
void dos_set_utc_offset(long long ticks);
void __stdcall GetLocalTime(void *system);
int __stdcall SystemTimeToFileTime(const void *system, void *file_time);
}
#pragma pack(pop)

#include "types.h"
#include "utils.h"
#include "packet.h"
#include "timer.h"
#include "eth.h"
#include "arp.h"
#include "ip.h"
#include "udp.h"
#include "tcp.h"
#include "tcpsockm.h"
#include "dns.h"

// ---------- Winsock's numbers (the game's)

enum
{
	E_INTR = 10004,
	E_BADF = 10009,
	E_FAULT = 10014,
	E_INVAL = 10022,
	E_MFILE = 10024,
	E_WOULDBLOCK = 10035,
	E_INPROGRESS = 10036,
	E_ALREADY = 10037,
	E_NOTSOCK = 10038,
	E_MSGSIZE = 10040,
	E_NOPROTOOPT = 10042,
	E_OPNOTSUPP = 10045,
	E_ADDRINUSE = 10048,
	E_NETDOWN = 10050,
	E_NETUNREACH = 10051,
	E_CONNABORTED = 10053,
	E_CONNRESET = 10054,
	E_NOBUFS = 10055,
	E_ISCONN = 10056,
	E_NOTCONN = 10057,
	E_TIMEDOUT = 10060,
	E_CONNREFUSED = 10061,
	E_HOSTUNREACH = 10065,

	SOCKET_STREAM = 1,
	SOCKET_DATAGRAM = 2,

	LEVEL_SOCKET = 0xFFFF,
	LEVEL_TCP = 6,
	OPTION_REUSEADDR = 0x0004,
	OPTION_KEEPALIVE = 0x0008,
	OPTION_BROADCAST = 0x0020,
	OPTION_LINGER = 0x0080,
	OPTION_SNDBUF = 0x1001,
	OPTION_RCVBUF = 0x1002,
	OPTION_ERROR = 0x1007,
	OPTION_TYPE = 0x1008,
	OPTION_NODELAY = 0x0001,

	SHUT_RECEIVE = 0,
	SHUT_SEND = 1,
	SHUT_BOTH = 2,
};

// sockaddr_in, as Winsock and BSD have it
struct net_address
{
	uint16_t family;
	uint16_t port;     // network order
	uint32_t address;  // network order
	uint8_t zero[8];
};

// ---------- the sockets

#define MAXIMUM_SOCKETS 64
#define DATAGRAM_QUEUE 32
#define DATAGRAM_SIZE 1480
#define STREAM_RECEIVE_BUFFER 16384u
#define LOOP_RING_SIZE 65536u
#define ACCEPT_QUEUE 16

struct datagram
{
	uint32_t from_address;
	uint16_t from_port;
	uint16_t length;
	uint8_t data[DATAGRAM_SIZE];
};

struct loop_ring
{
	uint8_t *data;
	uint32_t read, write; // (counts: read - write is what is queued)
};

struct net_socket
{
	int used;
	int type;
	int nonblocking;
	int broadcast;
	uint16_t port;          // local, host order (0: none yet)
	int error;              // SO_ERROR, once

	// a datagram socket's queue, and the address connect gave it
	struct datagram *queue;
	int queue_first, queue_count;
	uint32_t connected_address;
	uint16_t connected_port;

	// a stream socket: mTCP's, or a loopback pair's
	TcpSocket *tcp;
	int listening;
	int connecting;
	int shut_send, shut_receive;
	int peer;               // the loopback pair's other socket (+1; 0: none)
	struct loop_ring ring;  // (a loopback stream's incoming bytes)
	int peer_closed;
	unsigned long bytes_in, bytes_out; // (a loopback stream's, for net_debug)
	uint32_t peer_address;  // (network order)
	uint16_t peer_port;     // (host order)
	// a listening socket's connections, not yet accepted (+1: socket index)
	int accept_queue[ACCEPT_QUEUE];
	int accept_count;
};

static struct net_socket sockets[MAXIMUM_SOCKETS];
static int last_error;
static int stack_state; // 0 not tried, 1 running, -1 no network
static uint16_t next_ephemeral = 49152;

// mTCP sockets being closed, freed once their close completes
static TcpSocket *closing[TCP_MAX_SOCKETS];
static int closing_count;

static int fail(int error)
{
	last_error = error;
	return -1;
}

static int succeed(int result)
{
	last_error = 0;
	return result;
}

static struct net_socket *socket_of(int descriptor)
{
	if (descriptor < 1 || descriptor > MAXIMUM_SOCKETS || !sockets[descriptor - 1].used)
		return NULL;
	return &sockets[descriptor - 1];
}

static int descriptor_of(struct net_socket *s)
{
	return (int)(s - sockets) + 1;
}

static void log_line(const char *format, ...)
{
	char text[160];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	system_log(text);
}

// the setting net_debug: the loopback streams' connections logged
static int debugging(void)
{
	static int setting = -1;

	if (setting < 0)
		setting = atoi(dos_setting("net_debug", "0")) != 0;
	return setting;
}

static uint32_t address_of(const IpAddr_t bytes)
{
	uint32_t address;

	memcpy(&address, bytes, 4);
	return address;
}

static int is_loopback(uint32_t address)
{
	return (address & 0xFF) == 127 || (stack_state > 0 && address == address_of(MyIpAddr) && address != 0);
}

// ---------- the stack: start, drive, stop

static void stack_stop(void)
{
	if (stack_state <= 0)
		return;
	Buffer_stopReceiving();
	Packet_release_type();
	Dns::stop();
	TcpSocketMgr::stop();
	TcpBuffer::stop();
	Ip::reassemblyStop();
	Buffer_stop();
	stack_state = -1;
}

static void accept_new_connections(void);
static void reap_closing(void);

static void drive(void)
{
	if (stack_state <= 0)
		return;
	PACKET_PROCESS_MULT(20);
	Tcp::drivePackets();
	Arp::driveArp();
	if (Dns::isQueryPending())
		Dns::drivePendingQuery();
	accept_new_connections();
	reap_closing();
}

static int parse_address(const char *text, IpAddr_t address)
{
	unsigned a, b, c, d;

	if (!text || sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
		return 0;
	address[0] = (uint8_t)a;
	address[1] = (uint8_t)b;
	address[2] = (uint8_t)c;
	address[3] = (uint8_t)d;
	return 1;
}

// an mTCP configuration file's values (MTCPCFG): PACKETINT, IPADDR, NETMASK,
// GATEWAY, NAMESERVER, MTU
static void read_mtcp_config(uint8_t *packet_int, int *have_address)
{
	const char *path = getenv("MTCPCFG");
	char line[160];
	FILE *file;

	if (!path || !(file = fopen(path, "r")))
		return;
	while (fgets(line, sizeof(line), file))
	{
		char name[40], value[100];
		IpAddr_t address;

		if (sscanf(line, "%39s %99s", name, value) != 2)
			continue;
		if (!stricmp(name, "PACKETINT"))
			*packet_int = (uint8_t)strtoul(value, NULL, 16);
		else if (!stricmp(name, "IPADDR") && parse_address(value, address))
		{
			Ip::setMyIpAddr(address);
			*have_address = 1;
		}
		else if (!stricmp(name, "NETMASK") && parse_address(value, address))
			Ip::setMyNetmask(address);
		else if (!stricmp(name, "GATEWAY") && parse_address(value, address))
			Ip::copy(Gateway, address);
		else if (!stricmp(name, "NAMESERVER") && parse_address(value, address))
			Ip::copy(Dns::NameServer, address);
		else if (!stricmp(name, "MTU"))
			MyMTU = (uint16_t)atoi(value);
	}
	fclose(file);
}

static int dhcp(void);
static void time_from_server(void);

static void stack_start(void)
{
	uint8_t packet_int = 0;
	int have_address = 0;
	const char *setting;
	IpAddr_t address;

	stack_state = -1;
	/* (an Ethernet's 1500 bytes, not mTCP's cautious 576: internet play's
	datagrams are up to about 1450 bytes, and a router or NAT may well drop
	IP fragments; MTU in the mTCP file or net_mtu says otherwise) */
	MyMTU = ETH_MTU_MAX;
	read_mtcp_config(&packet_int, &have_address);
	setting = dos_setting("net_mtu", "");
	if (atoi(setting) >= ETH_MTU_SAFE && atoi(setting) <= ETH_MTU_MAX)
		MyMTU = (uint16_t)atoi(setting);
	setting = dos_setting("net_packet_int", "");
	if (*setting)
		packet_int = (uint8_t)strtoul(setting, NULL, 16);
	if (!packet_int)
		packet_int = mtcp_find_packet_driver();
	if (!packet_int)
	{
		log_line("network: no packet driver (load the network card's packet driver to play over the network)");
		return;
	}
	if (Buffer_init())
	{
		log_line("network: no conventional memory for the packet buffers");
		return;
	}
	if (Packet_init(packet_int))
	{
		log_line("network: the packet driver at 0x%02X refused", packet_int);
		Buffer_stop();
		return;
	}
	Packet_get_addr(MyEthAddr);
	if (Packet_registerEtherType(0x0800, Ip::process) || Packet_registerEtherType(0x0806, Arp::processArp))
	{
		Packet_release_type();
		Buffer_stop();
		return;
	}
	Arp::init();
	Ip::initForReassembly();
	Icmp::init();
	TcpSocketMgr::init(TCP_MAX_SOCKETS);
	TcpBuffer::init(TCP_MAX_XMIT_BUFS);
	Dns::init();
	dos_at_exit(stack_stop);
	stack_state = 1;
	Buffer_startReceiving();
	log_line("network: packet driver at 0x%02X, MAC %02X:%02X:%02X:%02X:%02X:%02X", packet_int, MyEthAddr[0],
		MyEthAddr[1], MyEthAddr[2], MyEthAddr[3], MyEthAddr[4], MyEthAddr[5]);

	// the settings' address, else the mTCP file's, else DHCP's
	setting = dos_setting("net_address", "");
	if (parse_address(setting, address))
	{
		Ip::setMyIpAddr(address);
		have_address = 1;
		if (parse_address(dos_setting("net_netmask", "255.255.255.0"), address))
			Ip::setMyNetmask(address);
		if (parse_address(dos_setting("net_gateway", ""), address))
			Ip::copy(Gateway, address);
		if (parse_address(dos_setting("net_dns", ""), address))
			Ip::copy(Dns::NameServer, address);
	}
	if (!have_address && !dhcp())
	{
		log_line("network: no address (no DHCP server answered; set net_address in HALODOS.CFG)");
		stack_stop();
		return;
	}
	log_line("network: address %u.%u.%u.%u, netmask %u.%u.%u.%u, gateway %u.%u.%u.%u, MTU %u", MyIpAddr[0],
		MyIpAddr[1], MyIpAddr[2], MyIpAddr[3], Netmask[0], Netmask[1], Netmask[2], Netmask[3], Gateway[0], Gateway[1],
		Gateway[2], Gateway[3], MyMTU);
	time_from_server();
}

static int network_up(void)
{
	if (!stack_state)
		stack_start();
	drive();
	return stack_state > 0;
}

// ---------- DHCP (RFC 2131): this machine's address from the LAN's server

#pragma pack(push, 1)
struct dhcp_message
{
	uint8_t op, htype, hlen, hops;
	uint32_t xid;
	uint16_t secs, flags;
	uint8_t ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
	uint8_t chaddr[16];
	uint8_t sname[64];
	uint8_t file[128];
	uint8_t cookie[4];
	uint8_t options[312];
};
#pragma pack(pop)

static struct
{
	uint32_t xid;
	int stage;           // 1 waiting for an offer, 2 for the acknowledgement, 3 done
	IpAddr_t offered, server, netmask, gateway, dns;
	int have_netmask, have_gateway, have_dns;
} dhcp_state;

static const uint8_t *dhcp_option(const struct dhcp_message *message, int length, uint8_t code, int *size)
{
	const uint8_t *option = message->options;
	const uint8_t *end = (const uint8_t *)message + length;

	while (option < end && *option != 255)
	{
		if (*option == 0)
		{
			option++;
			continue;
		}
		if (option + 2 > end || option + 2 + option[1] > end)
			break;
		if (*option == code)
		{
			*size = option[1];
			return option + 2;
		}
		option += 2 + option[1];
	}
	return NULL;
}

static void dhcp_received(const unsigned char *packet, const UdpHeader *udp)
{
	const struct dhcp_message *message = (const struct dhcp_message *)(udp + 1);
	int length = ntohs(udp->len) - (int)sizeof(UdpHeader);
	const uint8_t *option;
	int size;

	if (length >= 240 && message->op == 2 && message->xid == dhcp_state.xid &&
		!memcmp(message->chaddr, MyEthAddr, 6) && message->cookie[0] == 99 && message->cookie[1] == 130 &&
		message->cookie[2] == 83 && message->cookie[3] == 99 &&
		(option = dhcp_option(message, length, 53, &size)) != NULL && size == 1)
	{
		const uint8_t *server = dhcp_option(message, length, 54, &size);

		if (*option == 2 && dhcp_state.stage == 1 && server && size == 4)
		{
			Ip::copy(dhcp_state.offered, message->yiaddr);
			Ip::copy(dhcp_state.server, server);
			dhcp_state.stage = 2;
		}
		else if (*option == 5 && dhcp_state.stage == 2)
		{
			const uint8_t *value;

			Ip::copy(dhcp_state.offered, message->yiaddr);
			if ((value = dhcp_option(message, length, 1, &size)) != NULL && size == 4)
			{
				Ip::copy(dhcp_state.netmask, value);
				dhcp_state.have_netmask = 1;
			}
			if ((value = dhcp_option(message, length, 3, &size)) != NULL && size >= 4)
			{
				Ip::copy(dhcp_state.gateway, value);
				dhcp_state.have_gateway = 1;
			}
			if ((value = dhcp_option(message, length, 6, &size)) != NULL && size >= 4)
			{
				Ip::copy(dhcp_state.dns, value);
				dhcp_state.have_dns = 1;
			}
			dhcp_state.stage = 3;
		}
		else if (*option == 6 && dhcp_state.stage == 2)
		{
			// (refused: from the start)
			dhcp_state.stage = 0;
		}
	}
	Buffer_free(packet);
}

static void dhcp_send(int type)
{
	static struct dhcp_message message;
	uint8_t *option = message.options;
	const IpAddr_t everyone = { 255, 255, 255, 255 };

	memset(&message, 0, sizeof(message));
	message.op = 1;
	message.htype = 1;
	message.hlen = 6;
	message.xid = dhcp_state.xid;
	message.flags = htons(0x8000);
	memcpy(message.chaddr, MyEthAddr, 6);
	message.cookie[0] = 99;
	message.cookie[1] = 130;
	message.cookie[2] = 83;
	message.cookie[3] = 99;
	*option++ = 53;
	*option++ = 1;
	*option++ = (uint8_t)type;
	*option++ = 61;
	*option++ = 7;
	*option++ = 1;
	memcpy(option, MyEthAddr, 6);
	option += 6;
	if (type == 3)
	{
		*option++ = 50;
		*option++ = 4;
		memcpy(option, dhcp_state.offered, 4);
		option += 4;
		*option++ = 54;
		*option++ = 4;
		memcpy(option, dhcp_state.server, 4);
		option += 4;
	}
	*option++ = 12;
	*option++ = 4;
	memcpy(option, "HALO", 4);
	option += 4;
	*option++ = 55;
	*option++ = 3;
	*option++ = 1;
	*option++ = 3;
	*option++ = 6;
	*option++ = 255;
	Udp::sendUdp(everyone, 68, 67, (uint16_t)(option - (uint8_t *)&message), (uint8_t *)&message, 0);
}

// DISCOVER, OFFER, REQUEST, ACK: up to 4 tries, 3 seconds each
static int dhcp(void)
{
	const IpAddr_t nobody = { 0, 0, 0, 0 };

	log_line("network: asking the LAN's DHCP server for an address...");
	Ip::setMyIpAddr(nobody);
	Ip::setMyNetmask(nobody);
	Udp::registerCallback(68, dhcp_received);
	for (int attempt = 0; attempt < 4 && dhcp_state.stage != 3; attempt++)
	{
		uint64_t until = dos_microseconds() + 3000000ull;
		int sent_request = 0;

		dhcp_state.xid = (uint32_t)dos_rdtsc() ^ ((uint32_t)MyEthAddr[5] << 24);
		dhcp_state.stage = 1;
		dhcp_send(1);
		while (dos_microseconds() < until && dhcp_state.stage != 3 && dhcp_state.stage != 0)
		{
			drive();
			if (dhcp_state.stage == 2 && !sent_request)
			{
				dhcp_send(3);
				sent_request = 1;
				until = dos_microseconds() + 3000000ull;
			}
		}
	}
	Udp::unregisterCallback(68);
	if (dhcp_state.stage != 3)
		return 0;
	Ip::setMyIpAddr(dhcp_state.offered);
	if (dhcp_state.have_netmask)
		Ip::setMyNetmask(dhcp_state.netmask);
	else
		Ip::setMyNetmask(255, 255, 255, 0);
	if (dhcp_state.have_gateway)
		Ip::copy(Gateway, dhcp_state.gateway);
	if (dhcp_state.have_dns)
		Ip::copy(Dns::NameServer, dhcp_state.dns);
	return 1;
}

// ---------- the time (SNTP, RFC 4330): the coordinated time from a server

/* DOS holds the local time only, and internet play needs the coordinated
time (UTC) to the minute: a game's listing carries its host's, and those
more than ten minutes off are refused (p2p_lobby.c). So once the network is
up an internet time server is asked (net_time_server, pool.ntp.org; none
if "0"), and DOS's local time less its answer is the offset GetSystemTime
takes off (dos_kernel32.c): the zone and however far DOS's clock is off. A
second each at most is spent on the lookup and the answer; without one the
setting time_zone gives the zone (hours east of UTC: -5 for New York in
winter, 1 for Berlin), or DOS's TZ variable. */

#define NTP_PORT 123
#define NTP_LOCAL_PORT 49123
// 1601 (FILETIME's start) to 1900 (NTP's), in seconds
#define NTP_FROM_FILETIME 9435484800ULL

static struct
{
	uint8_t sent[8];
	int answered;
	uint32_t seconds, fraction;
	uint8_t stratum;
} sntp_state;

static void sntp_received(const unsigned char *packet, const UdpHeader *udp)
{
	const uint8_t *message = (const uint8_t *)(udp + 1);
	int length = ntohs(udp->len) - (int)sizeof(UdpHeader);

	// (a server's answer, to this request: its originate time is ours)
	if (length >= 48 && (message[0] & 7) == 4 && message[1] >= 1 && message[1] <= 15 &&
		!memcmp(message + 24, sntp_state.sent, 8) && !sntp_state.answered)
	{
		sntp_state.seconds = ((uint32_t)message[40] << 24) | ((uint32_t)message[41] << 16) |
			((uint32_t)message[42] << 8) | message[43];
		sntp_state.fraction = ((uint32_t)message[44] << 24) | ((uint32_t)message[45] << 16) |
			((uint32_t)message[46] << 8) | message[47];
		sntp_state.stratum = message[1];
		sntp_state.answered = 1;
	}
	Buffer_free(packet);
}

static unsigned long long local_ticks(void)
{
	// (SYSTEMTIME and FILETIME as KERNEL32's)
	uint16_t system[8];
	uint32_t file_time[2];

	GetLocalTime(system);
	SystemTimeToFileTime(system, file_time);
	return ((unsigned long long)file_time[1] << 32) | file_time[0];
}

// the setting time_zone's offset, if it has one (no time server answered)
static void time_zone_from_setting(void)
{
	const char *zone = dos_setting("time_zone", "");

	if (zone[0])
		dos_set_utc_offset((long long)(atof(zone) * 3600.0) * 10000000LL);
}

static void time_from_server(void)
{
	const char *server = dos_setting("net_time_server", "pool.ntp.org");
	char name[DNS_MAX_NAME_LEN];
	IpAddr_t address;
	uint64_t until;
	int8_t result;

	if (!server[0] || !strcmp(server, "0") || strlen(server) >= sizeof(name))
	{
		time_zone_from_setting();
		return;
	}
	for (int i = 0; ; i++)
	{
		name[i] = (char)(server[i] >= 'A' && server[i] <= 'Z' ? server[i] - 'A' + 'a' : server[i]);
		if (!server[i])
			break;
	}
	if (!parse_address(name, address))
	{
		result = Dns::resolve(name, address, 1);
		until = dos_microseconds() + 1000000ull;
		while (result == 1 && Dns::isQueryPending() && dos_microseconds() < until)
			drive();
		if (result == 1 && Dns::resolve(name, address, 0) == 0)
			result = 0;
		if (result != 0)
		{
			log_line("network: no address for the time server %s (time_zone gives the zone)", name);
			time_zone_from_setting();
			return;
		}
	}
	Udp::registerCallback(NTP_LOCAL_PORT, sntp_received);
	until = dos_microseconds() + 1000000ull;
	for (uint64_t next = 0; !sntp_state.answered && dos_microseconds() < until; drive())
	{
		// (again every quarter second: the first may wait on ARP)
		if (dos_microseconds() >= next)
		{
			uint8_t request[48];
			uint64_t stamp = dos_rdtsc();

			memset(request, 0, sizeof(request));
			request[0] = 0x1B; // version 3, client
			for (int i = 0; i < 8; i++)
				sntp_state.sent[i] = (uint8_t)(stamp >> (8 * i)) ^ MyEthAddr[i % 6];
			memcpy(request + 40, sntp_state.sent, 8);
			Udp::sendUdp(address, NTP_LOCAL_PORT, NTP_PORT, sizeof(request), request, 0);
			next = dos_microseconds() + 250000ull;
		}
	}
	Udp::unregisterCallback(NTP_LOCAL_PORT);
	if (!sntp_state.answered)
	{
		log_line("network: the time server %s did not answer (time_zone gives the zone)", name);
		time_zone_from_setting();
		return;
	}
	{
		// (NTP's seconds wrap in 2036: those below 2^31 are the next era's)
		unsigned long long seconds = sntp_state.seconds + (sntp_state.seconds < 0x80000000u ? 0x100000000ULL : 0) +
			NTP_FROM_FILETIME;
		unsigned long long utc = seconds * 10000000ULL + (((unsigned long long)sntp_state.fraction * 10000000ULL) >> 32);
		long long offset = (long long)(local_ticks() - utc);
		long long minutes = offset / 600000000LL;
		long long zone = (minutes >= 0 ? minutes + 7 : minutes - 7) / 15 * 15;
		long long off = offset / 10000000LL - zone * 60;

		dos_set_utc_offset(offset);
		log_line("network: the time from %s: this computer's zone is UTC%c%d:%02d, its clock %lld s %s", name,
			zone < 0 ? '-' : '+', (int)((zone < 0 ? -zone : zone) / 60), (int)((zone < 0 ? -zone : zone) % 60),
			off < 0 ? -off : off, off < 0 ? "behind" : "ahead");
	}
}

// ---------- datagrams

static void queue_datagram(struct net_socket *s, uint32_t from_address, uint16_t from_port, const uint8_t *data,
	int length)
{
	struct datagram *slot;

	if (!s->queue || s->queue_count == DATAGRAM_QUEUE || length > DATAGRAM_SIZE)
		return;
	slot = &s->queue[(s->queue_first + s->queue_count) % DATAGRAM_QUEUE];
	slot->from_address = from_address;
	slot->from_port = from_port;
	slot->length = (uint16_t)length;
	memcpy(slot->data, data, (size_t)length);
	s->queue_count++;
}

static struct net_socket *datagram_socket_on(uint16_t port)
{
	for (int i = 0; i < MAXIMUM_SOCKETS; i++)
	{
		if (sockets[i].used && sockets[i].type == SOCKET_DATAGRAM && sockets[i].port == port)
			return &sockets[i];
	}
	return NULL;
}

static void datagram_received(const unsigned char *packet, const UdpHeader *udp)
{
	const IpHeader *ip = (const IpHeader *)(packet + sizeof(EthHeader));
	struct net_socket *s = datagram_socket_on(ntohs(udp->dst));

	if (s)
	{
		queue_datagram(s, address_of(ip->ip_src), ntohs(udp->src), (const uint8_t *)(udp + 1),
			ntohs(udp->len) - (int)sizeof(UdpHeader));
	}
	Buffer_free(packet);
}

static uint16_t ephemeral_port(void)
{
	for (int tries = 0; tries < 16384; tries++)
	{
		uint16_t port = next_ephemeral++;
		int used = 0;

		if (next_ephemeral < 49152)
			next_ephemeral = 49152;
		for (int i = 0; i < MAXIMUM_SOCKETS && !used; i++)
			used = sockets[i].used && sockets[i].port == port;
		if (!used)
			return port;
	}
	return 0;
}

static int bind_datagram(struct net_socket *s, uint16_t port)
{
	if (!port)
		port = ephemeral_port();
	if (datagram_socket_on(port))
		return fail(E_ADDRINUSE);
	if (!s->queue && !(s->queue = (struct datagram *)malloc(sizeof(struct datagram) * DATAGRAM_QUEUE)))
		return fail(E_NOBUFS);
	if (stack_state > 0 && Udp::registerCallback(port, datagram_received))
		return fail(E_NOBUFS);
	s->port = port;
	return succeed(0);
}

static int send_datagram(struct net_socket *s, const void *buffer, int length, uint32_t address, uint16_t port)
{
	IpAddr_t host;

	if (!s->port && bind_datagram(s, 0))
		return -1;
	if (length > DATAGRAM_SIZE)
		return fail(E_MSGSIZE);
	// (to this machine: to the socket on that port; a broadcast both there
	// and onto the LAN)
	if (is_loopback(address) || address == 0xFFFFFFFFu)
	{
		struct net_socket *target = datagram_socket_on(port);

		if (target && target != s)
			queue_datagram(target, is_loopback(address) ? 0x0100007Fu : address_of(MyIpAddr), s->port,
				(const uint8_t *)buffer, length);
		if (address != 0xFFFFFFFFu)
			return succeed(length);
	}
	if (!network_up())
		return fail(E_NETDOWN);
	memcpy(host, &address, 4);
	// (a datagram waiting for ARP is lost, as one may be; the game sends again)
	if (Udp::sendUdp(host, s->port, port, (uint16_t)length, (uint8_t *)buffer, 0) < 0)
		return fail(E_NOBUFS);
	return succeed(length);
}

// ---------- streams

static int ring_used(const struct loop_ring *ring)
{
	return (int)(ring->write - ring->read);
}

static int ring_put(struct loop_ring *ring, const uint8_t *data, int length)
{
	int room = (int)LOOP_RING_SIZE - ring_used(ring);
	int count = length < room ? length : room;

	for (int i = 0; i < count; i++)
		ring->data[(ring->write + (uint32_t)i) % LOOP_RING_SIZE] = data[i];
	ring->write += (uint32_t)count;
	return count;
}

static int ring_get(struct loop_ring *ring, uint8_t *data, int length, int peek)
{
	int used = ring_used(ring);
	int count = length < used ? length : used;

	for (int i = 0; i < count; i++)
		data[i] = ring->data[(ring->read + (uint32_t)i) % LOOP_RING_SIZE];
	if (!peek)
		ring->read += (uint32_t)count;
	return count;
}

static struct net_socket *listener_on(uint16_t port)
{
	for (int i = 0; i < MAXIMUM_SOCKETS; i++)
	{
		if (sockets[i].used && sockets[i].type == SOCKET_STREAM && sockets[i].listening && sockets[i].port == port)
			return &sockets[i];
	}
	return NULL;
}

static struct net_socket *new_socket(int type)
{
	for (int i = 0; i < MAXIMUM_SOCKETS; i++)
	{
		if (!sockets[i].used)
		{
			memset(&sockets[i], 0, sizeof(sockets[i]));
			sockets[i].used = 1;
			sockets[i].type = type;
			return &sockets[i];
		}
	}
	return NULL;
}

// mTCP's new connections, to their port's listening socket
static void accept_new_connections(void)
{
	TcpSocket *tcp;

	while ((tcp = TcpSocketMgr::accept()) != NULL)
	{
		struct net_socket *listener = listener_on(tcp->srcPort);
		struct net_socket *s = listener && listener->accept_count < ACCEPT_QUEUE ? new_socket(SOCKET_STREAM) : NULL;

		if (!s)
		{
			tcp->closeNonblocking();
			if (closing_count < TCP_MAX_SOCKETS)
				closing[closing_count++] = tcp;
			continue;
		}
		s->tcp = tcp;
		s->port = tcp->srcPort;
		s->peer_address = address_of(tcp->dstHost);
		s->peer_port = tcp->dstPort;
		listener->accept_queue[listener->accept_count++] = descriptor_of(s);
	}
}

static void reap_closing(void)
{
	for (int i = 0; i < closing_count;)
	{
		if (closing[i]->isCloseDone())
		{
			TcpSocketMgr::freeSocket(closing[i]);
			closing[i] = closing[--closing_count];
		}
		else
		{
			i++;
		}
	}
}

static int connect_loopback(struct net_socket *s, uint16_t port)
{
	struct net_socket *listener = listener_on(port);
	struct net_socket *other;

	if (!listener || listener->accept_count == ACCEPT_QUEUE)
	{
		if (debugging())
			log_line("network: loopback connection to port %u refused (%s)", port, listener ? "queue full" :
				"nothing listens");
		return fail(E_CONNREFUSED);
	}
	if (!(other = new_socket(SOCKET_STREAM)))
		return fail(E_MFILE);
	s->ring.data = (uint8_t *)malloc(LOOP_RING_SIZE);
	other->ring.data = (uint8_t *)malloc(LOOP_RING_SIZE);
	if (!s->ring.data || !other->ring.data)
	{
		free(s->ring.data);
		free(other->ring.data);
		s->ring.data = NULL;
		other->used = 0;
		return fail(E_NOBUFS);
	}
	if (!s->port)
		s->port = ephemeral_port();
	other->port = port;
	other->peer = descriptor_of(s);
	other->peer_address = 0x0100007Fu;
	other->peer_port = s->port;
	s->peer = descriptor_of(other);
	s->peer_address = 0x0100007Fu;
	s->peer_port = port;
	listener->accept_queue[listener->accept_count++] = descriptor_of(other);
	if (debugging())
		log_line("network: loopback connection from port %u to port %u", s->port, port);
	return succeed(0);
}

static int stream_connect(struct net_socket *s, uint32_t address, uint16_t port)
{
	IpAddr_t host;

	if (s->tcp || s->peer)
		return fail(s->connecting ? E_ALREADY : E_ISCONN);
	if (is_loopback(address))
		return connect_loopback(s, port);
	if (!network_up())
		return fail(E_NETDOWN);
	if (!(s->tcp = TcpSocketMgr::getSocket()))
		return fail(E_NOBUFS);
	if (s->tcp->setRecvBuffer(STREAM_RECEIVE_BUFFER))
	{
		TcpSocketMgr::freeSocket(s->tcp);
		s->tcp = NULL;
		return fail(E_NOBUFS);
	}
	if (!s->port)
		s->port = ephemeral_port();
	memcpy(host, &address, 4);
	if (s->tcp->connectNonBlocking(s->port, host, port))
	{
		TcpSocketMgr::freeSocket(s->tcp);
		s->tcp = NULL;
		return fail(E_NETUNREACH);
	}
	s->peer_address = address;
	s->peer_port = port;
	s->connecting = 1;
	if (s->nonblocking)
		return fail(E_WOULDBLOCK);
	{
		uint64_t until = dos_microseconds() + 10000000ull;

		while (dos_microseconds() < until && !s->tcp->isConnectComplete() && !s->tcp->isClosed())
			drive();
	}
	s->connecting = 0;
	if (!s->tcp->isConnectComplete())
		return fail(s->tcp->isClosed() ? E_CONNREFUSED : E_TIMEDOUT);
	return succeed(0);
}

// ---------- readiness (select)

static int readable(struct net_socket *s)
{
	if (s->type == SOCKET_DATAGRAM)
		return s->queue_count > 0;
	if (s->listening)
		return s->accept_count > 0;
	if (s->peer || s->ring.data)
		return ring_used(&s->ring) > 0 || s->peer_closed;
	if (s->tcp)
		return s->tcp->recvDataWaiting() || s->tcp->isRemoteClosed() || s->tcp->isClosed();
	return 0;
}

static int writable(struct net_socket *s)
{
	if (s->type == SOCKET_DATAGRAM)
		return 1;
	if (s->peer)
	{
		struct net_socket *other = socket_of(s->peer);

		return !other || ring_used(&other->ring) < (int)LOOP_RING_SIZE;
	}
	if (s->tcp)
	{
		if (s->connecting && s->tcp->isConnectComplete())
			s->connecting = 0;
		return !s->connecting && s->tcp->isEstablished() && !s->tcp->outgoingQueueIsFull();
	}
	return 0;
}

// the stack's counters, for the log when something fails
static void log_statistics(const char *why)
{
	log_line("network: %s: packets sent %lu received %lu dropped %lu; tcp sent %lu received %lu resent %lu "
		"bad checksums %lu seq/ack errors %lu; ip bad checksums %lu; udp received %lu",
		why, (unsigned long)Packets_sent, (unsigned long)Packets_received, (unsigned long)Packets_dropped,
		(unsigned long)Tcp::Packets_Sent, (unsigned long)Tcp::Packets_Received,
		(unsigned long)Tcp::Packets_Retransmitted, (unsigned long)Tcp::ChecksumErrors,
		(unsigned long)Tcp::Packets_SeqOrAckError, (unsigned long)Ip::badChecksum,
		(unsigned long)Udp::Packets_Received);
}

static int failed(struct net_socket *s)
{
	if (s->tcp && s->connecting && s->tcp->isClosed())
	{
		log_statistics("a connection was refused");
		s->connecting = 0;
		s->error = E_CONNREFUSED;
		return 1;
	}
	return 0;
}

// ---------- posix.h

extern "C" {

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	struct net_socket *s;

	(void)protocol;
	if (family != 2 || (type != SOCKET_STREAM && type != SOCKET_DATAGRAM))
		return fail(E_INVAL);
	network_up();
	if (!(s = new_socket(type)))
		return fail(E_MFILE);
	return succeed(descriptor_of(s));
}

int posix_socket_close(int socket)
{
	struct net_socket *s = socket_of(socket);

	if (!s)
		return fail(E_NOTSOCK);
	if (s->type == SOCKET_DATAGRAM && s->port && stack_state > 0)
		Udp::unregisterCallback(s->port);
	if (s->tcp)
	{
		if (s->connecting)
			log_statistics("a connection still being made was closed");
		s->tcp->closeNonblocking();
		if (closing_count < TCP_MAX_SOCKETS)
			closing[closing_count++] = s->tcp;
	}
	if (s->listening && stack_state > 0)
	{
		// (the listening mTCP socket, and the connections never accepted)
		for (int i = 0; i < s->accept_count; i++)
			posix_socket_close(s->accept_queue[i]);
	}
	if (s->peer || s->ring.data)
	{
		struct net_socket *other = socket_of(s->peer);

		if (debugging())
		{
			log_line("network: loopback stream on port %u closed: %lu bytes in, %lu out, %d unread", s->port,
				s->bytes_in, s->bytes_out, s->ring.data ? ring_used(&s->ring) : 0);
		}
		if (other)
		{
			other->peer_closed = 1;
			other->peer = 0;
		}
	}
	free(s->queue);
	free(s->ring.data);
	memset(s, 0, sizeof(*s));
	drive();
	return succeed(0);
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	struct net_socket *s = socket_of(socket);
	const struct net_address *where = (const struct net_address *)address;
	uint16_t port;

	if (!s)
		return fail(E_NOTSOCK);
	if (!where || address_length < (int)sizeof(struct net_address))
		return fail(E_FAULT);
	if (s->port)
		return fail(E_INVAL);
	port = ntohs(where->port);
	if (s->type == SOCKET_DATAGRAM)
		return bind_datagram(s, port);
	if (port && listener_on(port))
		return fail(E_ADDRINUSE);
	s->port = port ? port : ephemeral_port();
	return succeed(0);
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	struct net_socket *s = socket_of(socket);
	const struct net_address *where = (const struct net_address *)address;

	if (!s)
		return fail(E_NOTSOCK);
	if (!where || address_length < (int)sizeof(struct net_address))
		return fail(E_FAULT);
	if (s->type == SOCKET_DATAGRAM)
	{
		if (!s->port && bind_datagram(s, 0))
			return -1;
		s->connected_address = where->address;
		s->connected_port = ntohs(where->port);
		return succeed(0);
	}
	return stream_connect(s, where->address, ntohs(where->port));
}

int posix_socket_listen(int socket, int backlog)
{
	struct net_socket *s = socket_of(socket);

	(void)backlog;
	if (!s || s->type != SOCKET_STREAM)
		return fail(s ? E_OPNOTSUPP : E_NOTSOCK);
	if (s->listening)
		return succeed(0);
	if (!s->port)
		s->port = ephemeral_port();
	// (the network's: a socket of mTCP's listening on the port, if there
	// is a network; the loopback's always)
	if (network_up())
	{
		if (!(s->tcp = TcpSocketMgr::getSocket()))
			return fail(E_NOBUFS);
		if (s->tcp->listen(s->port, STREAM_RECEIVE_BUFFER))
		{
			TcpSocketMgr::freeSocket(s->tcp);
			s->tcp = NULL;
			return fail(E_ADDRINUSE);
		}
	}
	s->listening = 1;
	return succeed(0);
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	struct net_socket *s = socket_of(socket);
	struct net_socket *accepted;
	int descriptor;

	if (!s || !s->listening)
		return fail(s ? E_INVAL : E_NOTSOCK);
	drive();
	if (!s->accept_count)
		return fail(E_WOULDBLOCK);
	descriptor = s->accept_queue[0];
	memmove(s->accept_queue, s->accept_queue + 1, sizeof(s->accept_queue[0]) * (size_t)--s->accept_count);
	accepted = socket_of(descriptor);
	if (!accepted)
		return fail(E_CONNABORTED);
	if (address && address_length && *address_length >= (int)sizeof(struct net_address))
	{
		struct net_address *where = (struct net_address *)address;

		memset(where, 0, sizeof(*where));
		where->family = 2;
		where->port = htons(accepted->peer_port);
		where->address = accepted->peer_address;
		*address_length = sizeof(*where);
	}
	return succeed(descriptor);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	struct net_socket *s = socket_of(socket);

	(void)flags;
	if (!s)
		return fail(E_NOTSOCK);
	if (s->type == SOCKET_DATAGRAM)
	{
		if (!s->connected_port)
			return fail(E_NOTCONN);
		return send_datagram(s, buffer, length, s->connected_address, s->connected_port);
	}
	if (s->shut_send)
		return fail(E_CONNABORTED);
	if (s->peer || s->ring.data)
	{
		struct net_socket *other = socket_of(s->peer);
		int written;

		if (!other)
			return fail(E_CONNRESET);
		written = ring_put(&other->ring, (const uint8_t *)buffer, length);
		s->bytes_out += (unsigned long)written;
		return written ? succeed(written) : fail(E_WOULDBLOCK);
	}
	if (!s->tcp)
		return fail(E_NOTCONN);
	drive();
	if (s->connecting && !s->tcp->isConnectComplete())
		return fail(E_WOULDBLOCK);
	s->connecting = 0;
	if (!s->tcp->isEstablished())
		return fail(s->tcp->isClosed() || s->tcp->isRemoteClosed() ? E_CONNRESET : E_NOTCONN);
	{
		int sent = 0;

		// (mTCP takes up to 64 KB a call)
		while (sent < length)
		{
			int part = length - sent > 16384 ? 16384 : length - sent;
			int16_t result = s->tcp->send((uint8_t *)buffer + sent, (uint16_t)part);

			if (result <= 0)
				break;
			sent += result;
			if (result < part)
				break;
		}
		drive();
		return sent ? succeed(sent) : fail(E_WOULDBLOCK);
	}
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags, const void *address,
	int address_length)
{
	struct net_socket *s = socket_of(socket);
	const struct net_address *where = (const struct net_address *)address;

	(void)flags;
	if (!s)
		return fail(E_NOTSOCK);
	if (s->type != SOCKET_DATAGRAM || !where || address_length < (int)sizeof(struct net_address))
		return posix_socket_send(socket, buffer, length, flags);
	return send_datagram(s, buffer, length, where->address, ntohs(where->port));
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	struct net_socket *s = socket_of(socket);
	int peek = (flags & 2) != 0; // MSG_PEEK

	if (!s)
		return fail(E_NOTSOCK);
	if (s->type == SOCKET_DATAGRAM)
		return posix_socket_recvfrom(socket, buffer, length, flags, NULL, NULL);
	drive();
	if (s->peer || s->ring.data)
	{
		int count = ring_get(&s->ring, (uint8_t *)buffer, length, peek);

		if (!peek)
			s->bytes_in += (unsigned long)count;
		if (count)
			return succeed(count);
		return s->peer_closed ? succeed(0) : fail(E_WOULDBLOCK);
	}
	if (!s->tcp)
		return fail(E_NOTCONN);
	if (s->tcp->state < TCP_STATE_ESTABLISHED)
		return s->tcp->isClosed() ? fail(E_CONNRESET) : fail(E_WOULDBLOCK);
	if (peek)
	{
		// (mTCP takes, never peeks: what is waiting, as far as it goes)
		return s->tcp->recvDataWaiting() ? succeed(1) : fail(E_WOULDBLOCK);
	}
	{
		int16_t count = s->tcp->recv((uint8_t *)buffer, (uint16_t)(length > 32767 ? 32767 : length));

		if (count > 0)
			return succeed(count);
		if (count < 0 || s->tcp->isRemoteClosed() || s->tcp->isClosed())
			return succeed(0);
		return fail(E_WOULDBLOCK);
	}
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags, void *address, int *address_length)
{
	struct net_socket *s = socket_of(socket);
	struct datagram *datagram;
	int count;

	if (!s)
		return fail(E_NOTSOCK);
	if (s->type != SOCKET_DATAGRAM)
	{
		count = posix_socket_recv(socket, buffer, length, flags);
		if (count >= 0 && address && address_length && *address_length >= (int)sizeof(struct net_address))
			posix_socket_getpeername(socket, address, address_length);
		return count;
	}
	drive();
	if (!s->queue_count)
		return fail(E_WOULDBLOCK);
	datagram = &s->queue[s->queue_first];
	count = datagram->length < length ? datagram->length : length;
	memcpy(buffer, datagram->data, (size_t)count);
	if (address && address_length && *address_length >= (int)sizeof(struct net_address))
	{
		struct net_address *where = (struct net_address *)address;

		memset(where, 0, sizeof(*where));
		where->family = 2;
		where->port = htons(datagram->from_port);
		where->address = datagram->from_address;
		*address_length = sizeof(*where);
	}
	if (!(flags & 2))
	{
		s->queue_first = (s->queue_first + 1) % DATAGRAM_QUEUE;
		s->queue_count--;
	}
	if (datagram->length > length)
		return fail(E_MSGSIZE);
	return succeed(count);
}

int posix_socket_shutdown(int socket, int how)
{
	struct net_socket *s = socket_of(socket);

	if (!s)
		return fail(E_NOTSOCK);
	if (how == SHUT_SEND || how == SHUT_BOTH)
		s->shut_send = 1;
	if (how == SHUT_RECEIVE || how == SHUT_BOTH)
		s->shut_receive = 1;
	if (s->tcp && s->tcp->isEstablished())
		s->tcp->shutdown(how == SHUT_SEND ? TCP_SHUT_WR : how == SHUT_RECEIVE ? TCP_SHUT_RD : TCP_SHUT_RDWR);
	return succeed(0);
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	struct net_socket *s = socket_of(socket);

	if (!s)
		return fail(E_NOTSOCK);
	s->nonblocking = nonblocking != 0;
	return succeed(0);
}

int posix_socket_set_nodelay(int socket)
{
	return socket_of(socket) ? succeed(0) : fail(E_NOTSOCK);
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	struct net_socket *s = socket_of(socket);

	if (!s)
		return fail(E_NOTSOCK);
	drive();
	*count = 0;
	if (s->type == SOCKET_DATAGRAM)
		*count = s->queue_count ? s->queue[s->queue_first].length : 0;
	else if (s->peer || s->ring.data)
		*count = (posix_ulong)ring_used(&s->ring);
	else if (s->tcp && s->tcp->state >= TCP_STATE_ESTABLISHED)
		*count = s->tcp->recvDataWaiting() ? s->tcp->rcvBufEntries : 0;
	return succeed(0);
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	struct net_socket *s = socket_of(socket);

	if (!s)
		return fail(E_NOTSOCK);
	if (level == LEVEL_SOCKET && name == OPTION_BROADCAST && value && length >= 1)
		s->broadcast = *(const char *)value != 0;
	// (the rest mean nothing here: buffers are mTCP's own sizes)
	return succeed(0);
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	struct net_socket *s = socket_of(socket);
	int answer = 0;

	if (!s)
		return fail(E_NOTSOCK);
	if (level != LEVEL_SOCKET && !(level == LEVEL_TCP && name == OPTION_NODELAY))
		return fail(E_NOPROTOOPT);
	switch (name)
	{
	case OPTION_ERROR:
		answer = s->error;
		s->error = 0;
		break;
	case OPTION_TYPE:
		answer = s->type;
		break;
	case OPTION_BROADCAST:
		answer = s->broadcast;
		break;
	case OPTION_SNDBUF:
	case OPTION_RCVBUF:
		answer = (int)STREAM_RECEIVE_BUFFER;
		break;
	case OPTION_NODELAY:
		answer = 1;
		break;
	default:
		answer = 0;
		break;
	}
	if (!value || !length || *length < (int)sizeof(int))
		return fail(E_FAULT);
	memcpy(value, &answer, sizeof(answer));
	*length = sizeof(answer);
	return succeed(0);
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	struct net_socket *s = socket_of(socket);
	struct net_address *where = (struct net_address *)address;

	if (!s)
		return fail(E_NOTSOCK);
	if (!where || !address_length || *address_length < (int)sizeof(struct net_address))
		return fail(E_FAULT);
	memset(where, 0, sizeof(*where));
	where->family = 2;
	where->port = htons(s->port);
	where->address = (s->peer || (s->peer_address & 0xFF) == 127) ? 0x0100007Fu :
		stack_state > 0 && (s->tcp || s->peer_address) ? address_of(MyIpAddr) : 0;
	*address_length = sizeof(*where);
	return succeed(0);
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	struct net_socket *s = socket_of(socket);
	struct net_address *where = (struct net_address *)address;

	if (!s)
		return fail(E_NOTSOCK);
	if (!where || !address_length || *address_length < (int)sizeof(struct net_address))
		return fail(E_FAULT);
	if (s->type == SOCKET_DATAGRAM ? !s->connected_port : !s->peer_port)
		return fail(E_NOTCONN);
	memset(where, 0, sizeof(*where));
	where->family = 2;
	where->port = htons(s->type == SOCKET_DATAGRAM ? s->connected_port : s->peer_port);
	where->address = s->type == SOCKET_DATAGRAM ? s->connected_address : s->peer_address;
	*address_length = sizeof(*where);
	return succeed(0);
}

static int count_ready(const int *descriptors, const int *count, int (*ready)(struct net_socket *s))
{
	int ready_count = 0;

	if (!descriptors || !count)
		return 0;
	for (int i = 0; i < *count; i++)
	{
		struct net_socket *s = socket_of(descriptors[i]);

		if (s && ready(s))
			ready_count++;
	}
	return ready_count;
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count, int *error, int *error_count,
	posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	uint64_t until = dos_microseconds() + (uint64_t)timeout_seconds * 1000000ull + (uint64_t)timeout_microseconds;
	int reads, writes, errors;

	for (;;)
	{
		drive();
		reads = count_ready(read, read_count, readable);
		writes = count_ready(write, write_count, writable);
		errors = count_ready(error, error_count, failed);
		if (reads || writes || errors || (!infinite && dos_microseconds() >= until))
			break;
	}
	// (each list keeps its ready descriptors, in order)
	if (read && read_count)
	{
		int kept = 0;

		for (int i = 0; i < *read_count; i++)
		{
			struct net_socket *s = socket_of(read[i]);

			if (s && readable(s))
				read[kept++] = read[i];
		}
		*read_count = kept;
	}
	if (write && write_count)
	{
		int kept = 0;

		for (int i = 0; i < *write_count; i++)
		{
			struct net_socket *s = socket_of(write[i]);

			if (s && writable(s))
				write[kept++] = write[i];
		}
		*write_count = kept;
	}
	if (error && error_count)
	{
		int kept = 0;

		for (int i = 0; i < *error_count; i++)
		{
			struct net_socket *s = socket_of(error[i]);

			if (s && (s->error || failed(s)))
				error[kept++] = error[i];
		}
		*error_count = kept;
	}
	return succeed((read_count ? *read_count : 0) + (write_count ? *write_count : 0) +
		(error_count ? *error_count : 0));
}

posix_ulong posix_local_ipv4_address(void)
{
	return network_up() ? (posix_ulong)address_of(MyIpAddr) : 0;
}

int posix_network_link_up(void)
{
	return network_up();
}

// (the keys the game makes: the time stamp counter, mixed)
void posix_random_bytes(void *buffer, posix_ulong size)
{
	static uint64_t chain = 0x9E3779B97F4A7C15ull;
	uint8_t *bytes = (uint8_t *)buffer;

	while (size)
	{
		uint64_t value = chain ^ dos_rdtsc();
		posix_ulong count = size < 8 ? size : 8;

		value ^= value >> 30;
		value *= 0xBF58476D1CE4E5B9ull;
		value ^= value >> 27;
		value *= 0x94D049BB133111EBull;
		value ^= value >> 31;
		chain = value + 0x9E3779B97F4A7C15ull;
		memcpy(bytes, &value, count);
		bytes += count;
		size -= count;
	}
}

/* A name's address, without waiting: the game's one thread must not stop
for DNS (its callers, internet play's, ask again later). The first call
for a name sends its query (one at a time: mTCP keeps one pending) and
answers 0; a later call answers from mTCP's cache once the reply came. A
name that found no address is not asked about again for 30 seconds. */
#define RESOLVE_SLOTS 12

static struct
{
	char name[DNS_MAX_NAME_LEN];
	uint32_t address;
	int state; /* 0 unused, 1 waiting its turn, 2 asked, 3 found, 4 not found */
	uint64_t when;
} resolves[RESOLVE_SLOTS];

posix_ulong posix_resolve_ipv4(const char *host)
{
	IpAddr_t address;
	char name[DNS_MAX_NAME_LEN];
	int slot = -1;
	int8_t result;

	if (parse_address(host, address))
		return address_of(address);
	if (!host || !network_up() || strlen(host) >= sizeof(name))
		return 0;
	/* (mTCP's names are lower case) */
	for (int i = 0; ; i++)
	{
		name[i] = (char)(host[i] >= 'A' && host[i] <= 'Z' ? host[i] - 'A' + 'a' : host[i]);
		if (!host[i])
			break;
	}
	for (int i = 0; i < RESOLVE_SLOTS && slot < 0; i++)
	{
		if (resolves[i].state && !strcmp(resolves[i].name, name))
			slot = i;
	}
	if (slot < 0)
	{
		for (int i = 0; i < RESOLVE_SLOTS && slot < 0; i++)
		{
			if (!resolves[i].state)
				slot = i;
		}
		if (slot < 0)
			slot = (int)(dos_rdtsc() % RESOLVE_SLOTS);
		strcpy(resolves[slot].name, name);
		resolves[slot].state = 1;
	}
	if (resolves[slot].state == 3)
		return resolves[slot].address;
	if (resolves[slot].state == 4)
	{
		if (dos_microseconds() - resolves[slot].when < 30000000ull)
			return 0;
		resolves[slot].state = 1;
	}
	/* (answered meanwhile: in mTCP's cache) */
	if (Dns::resolve(name, address, 0) == 0)
	{
		resolves[slot].address = address_of(address);
		resolves[slot].state = 3;
		return resolves[slot].address;
	}
	if (Dns::isQueryPending())
		return 0;
	if (resolves[slot].state == 2)
	{
		/* (its query ended without an address) */
		resolves[slot].state = 4;
		resolves[slot].when = dos_microseconds();
		log_line("network: no address for %s", name);
		return 0;
	}
	result = Dns::resolve(name, address, 1);
	/* (a LAN's or the provider's DNS answers in milliseconds: waited for
	that long, a quarter second at most, so the caller's first try finds
	it; a slower one is answered on a later call) */
	if (result == 1)
	{
		uint64_t until = dos_microseconds() + 250000ull;

		while (Dns::isQueryPending() && dos_microseconds() < until)
			drive();
		if (Dns::resolve(name, address, 0) == 0)
			result = 0;
	}
	if (result == 0)
	{
		resolves[slot].address = address_of(address);
		resolves[slot].state = 3;
		return resolves[slot].address;
	}
	if (result == 1)
		resolves[slot].state = 2;
	else if (result < 0)
	{
		resolves[slot].state = 4;
		resolves[slot].when = dos_microseconds();
	}
	return 0;
}

} // extern "C"
