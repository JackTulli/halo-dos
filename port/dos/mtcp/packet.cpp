/*
   PACKET.CPP

   halo-dos: mTCP's packet driver interface (PACKET.CPP: Packet_* and
   Buffer_*) for the DOS build's 32-bit protected mode under DOS/32A.

   The packet driver is real-mode code that calls its receiver from the
   network card's interrupt. That receiver (pktstub.s) lives in conventional
   memory with the receive buffers, and works in real mode alone: it takes
   a buffer from a free stack and, once the packet is copied, puts it on a
   ring. The stack (Packet_process_internal) takes packets off the ring,
   hands each to the handler of its EtherType (IP, ARP), which gives the
   buffer back with Buffer_free. Sending copies the packet into a block of
   conventional memory of its own and calls the driver through DPMI's
   real-mode interrupt service.

   mTCP: Copyright (C) 2005-2026 Michael B. Brutman (mbbrutman@gmail.com),
   GPLv3 (see LICENSE). The packet driver calls are mTCP's, from the packet
   driver specification 1.09.
*/

#include "types.h"
#include "packet.h"
#include "eth.h"
#include "ip.h"

extern "C" {
#include "dos_dpmi.h"
}

// the receiver's block, at offset 0 of its conventional memory (pktstub.s
// has these offsets)
#pragma pack(push, 1)
struct receiver_block {
  uint16_t reserved;
  uint16_t free_count;           // 2
  uint16_t ring_next;            // 4: the receiver's next slot
  uint16_t ring_first;           // 6: (unused: Buffer_first is ours)
  uint16_t buffer_length;        // 8: the longest packet taken
  uint16_t pad;
  uint32_t dropped;              // 12
  uint32_t received;             // 16
  uint16_t free_stack[48];       // 20: offsets of free buffers
  uint16_t ring_offset[50];      // 116
  uint16_t ring_length[50];      // 216
  uint8_t  pad2[320 - 316];
};
#pragma pack(pop)

static_assert( sizeof(struct receiver_block) == 320 );
static_assert( PACKET_BUFFERS <= 48 );

#define RECEIVER_CODE   (320)
#define RING_SIZE       (49)
#define BUFFERS_START   (512)
#define BUFFER_STRIDE   (1520)

extern "C" const uint8_t mtcp_receiver_code[], mtcp_receiver_code_end[];

static struct receiver_block *block;   // (in conventional memory)
static uint16_t block_segment, block_selector;
static uint8_t *send_buffer;           // (conventional memory too)
static uint16_t send_segment, send_selector;

uint16_t Buffer_first;
static volatile uint16_t no_packets;
volatile uint16_t *Buffer_ring_next = &no_packets;

uint8_t Buffer_lowFreeCount;

uint32_t Packets_dropped = 0;
uint32_t Packets_received = 0;
uint32_t Packets_sent = 0;
uint32_t Packets_send_errs = 0;
uint32_t Packets_send_retries = 0;

const char * PKT_DRVR_EYE_CATCHER = "PKT DRVR";

static uint16_t Packet_handle;
static uint8_t  Packet_int = 0x0;

void (*Packet_EtherTypeHandler[PACKET_HANDLERS])(uint8_t *packet, uint16_t len);
EtherType Packet_EtherTypeVal[PACKET_HANDLERS];
uint8_t  Packet_EtherTypeHandlers = 0;
void (*Packet_typeUnhandled)(uint8_t *packet, uint16_t len);


// ---------- interrupts off and on (types.h's disable_ints)

static unsigned long saved_flags;

extern "C" void mtcp_disable_ints( void ) { saved_flags = dos_interrupts_disable( ); }
extern "C" void mtcp_enable_ints( void ) { dos_interrupts_restore( saved_flags ); }


// ---------- buffers

int8_t Buffer_init( void ) {

  // the receiver, its ring and its buffers: one segment
  uint32_t size = BUFFERS_START + (uint32_t)PACKET_BUFFERS * BUFFER_STRIDE;

  if ( size > 0x10000ul ) return -1;
  if ( !dos_dos_allocate( (uint16_t)((size + 15) / 16), &block_segment, &block_selector ) ) return -1;
  if ( !dos_dos_allocate( (BUFFER_STRIDE + 15) / 16, &send_segment, &send_selector ) ) {
    dos_dos_free( block_selector );
    return -1;
  }
  block = (struct receiver_block *)dos_real_pointer( block_segment, 0 );
  send_buffer = (uint8_t *)dos_real_pointer( send_segment, 0 );
  memset( block, 0, BUFFERS_START );
  memcpy( (uint8_t *)block + RECEIVER_CODE, mtcp_receiver_code, mtcp_receiver_code_end - mtcp_receiver_code );
  block->buffer_length = PACKET_BUFFER_LEN;

  // (no buffers yet: the driver may deliver before the stack is ready)
  block->free_count = 0;
  Buffer_first = 0;
  block->ring_next = 0;
  Buffer_ring_next = &block->ring_next;
  Buffer_lowFreeCount = PACKET_BUFFERS;
  return 0;
}

void Buffer_startReceiving( void ) {
  mtcp_disable_ints( );
  for ( uint16_t i = 0; i < PACKET_BUFFERS; i++ ) {
    block->free_stack[i] = (uint16_t)(BUFFERS_START + i * BUFFER_STRIDE);
  }
  block->free_count = PACKET_BUFFERS;
  mtcp_enable_ints( );
}

void Buffer_free( const uint8_t *buffer ) {

  #ifdef IP_FRAGMENTS_ON
    if ( Ip::isIpBigPacket( buffer ) ) {
      Ip::returnBigPacket( (uint8_t *)buffer );
      return;
    }
  #endif

  mtcp_disable_ints( );
  block->free_stack[ block->free_count ] = (uint16_t)(buffer - (const uint8_t *)block);
  block->free_count++;
  mtcp_enable_ints( );
}

void Buffer_stopReceiving( void ) {
  if ( block ) block->free_count = 0;
}

void Buffer_stop( void ) {
  if ( block ) {
    Buffer_ring_next = &no_packets;
    dos_dos_free( block_selector );
    dos_dos_free( send_selector );
    block = NULL;
  }
}


// ---------- EtherType handlers

int8_t Packet_registerEtherType( EtherType val, void (*f)(uint8_t *packet, uint16_t) ) {
  if (Packet_EtherTypeHandlers == PACKET_HANDLERS ) return -1;
  Packet_EtherTypeVal[ Packet_EtherTypeHandlers ] = htons( val );
  Packet_EtherTypeHandler[ Packet_EtherTypeHandlers ] = f;
  Packet_EtherTypeHandlers++;
  return 0;
}

void Packet_registerDefault( void (*f)(uint8_t *packet, uint16_t) ) {
  Packet_typeUnhandled = f;
}

// the next packet on the ring, to its EtherType's handler (which frees it)
void Packet_process_internal( void ) {

  uint16_t offset = block->ring_offset[ Buffer_first ];
  uint16_t length = block->ring_length[ Buffer_first ];
  uint8_t *packet = (uint8_t *)block + offset;

  Buffer_first = (uint16_t)((Buffer_first + 1) % RING_SIZE);
  Packets_received = block->received;
  Packets_dropped = block->dropped;
  if ( Buffer_lowFreeCount > block->free_count ) Buffer_lowFreeCount = (uint8_t)block->free_count;

  EtherType packetType = ((EthHeader *)packet)->type;

  for ( uint8_t i = 0; i < Packet_EtherTypeHandlers; i++ ) {
    if ( Packet_EtherTypeVal[i] == packetType ) {
      Packet_EtherTypeHandler[i]( packet, length );
      return;
    }
  }
  if ( Packet_typeUnhandled ) {
    Packet_typeUnhandled( packet, length );
  }
  else {
    Buffer_free( packet );
  }
}


// ---------- the packet driver

// its interrupt's handler is a packet driver: "PKT DRVR" 3 bytes in
static int is_packet_driver( uint8_t number ) {
  const uint16_t *vector = (const uint16_t *)dos_real_pointer( 0, (uint16_t)(number * 4) );
  const char *eyeCatcher = (const char *)dos_real_pointer( vector[1], (uint16_t)(vector[0] + 3) );

  return (vector[0] || vector[1]) && memcmp( eyeCatcher, PKT_DRVR_EYE_CATCHER, 8 ) == 0;
}

// the first software interrupt from 0x60 to 0x80 a packet driver has
extern "C" uint8_t mtcp_find_packet_driver( void ) {
  for ( uint16_t number = 0x60; number <= 0x80; number++ ) {
    if ( is_packet_driver( (uint8_t)number ) ) return (uint8_t)number;
  }
  return 0;
}

static int call_driver( struct dos_registers *r ) {
  r->ss = r->sp = 0;
  r->flags = 0;
  if ( !dos_real_interrupt( Packet_int, r ) ) return -1;
  return (r->flags & DOS_CARRY) ? (int)((r->edx >> 8) & 0xFF) : 0;
}

int8_t Packet_init( uint8_t packetInt ) {
  return Packet_init2( packetInt, block_segment, RECEIVER_CODE );
}

int8_t Packet_init2( uint8_t packetInt, uint16_t recv_seg, uint16_t recv_off ) {

  if ( !is_packet_driver( packetInt ) ) return -1;
  Packet_int = packetInt;

  // access_type: class 1 (Ethernet), any type, number 0; every EtherType
  // (a type of length 0), to the receiver
  struct dos_registers r;
  memset( &r, 0, sizeof(r) );
  r.eax = 0x0201;
  r.ebx = 0xFFFF;
  r.edx = 0;
  r.ecx = 0;
  r.ds = send_segment;
  r.esi = 0;
  r.es = recv_seg;
  r.edi = recv_off;
  if ( call_driver( &r ) ) return -1;
  Packet_handle = (uint16_t)r.eax;
  return 0;
}

int8_t Packet_release_type( void ) {
  struct dos_registers r;

  if ( !Packet_int ) return 0;
  memset( &r, 0, sizeof(r) );
  r.eax = 0x0300;
  r.ebx = Packet_handle;
  int rc = call_driver( &r );
  Packet_int = 0;
  return rc ? -1 : 0;
}

void Packet_get_addr( uint8_t *target ) {
  struct dos_registers r;

  memset( &r, 0, sizeof(r) );
  r.eax = 0x0600;
  r.ebx = Packet_handle;
  r.ecx = 6;
  r.es = send_segment;
  r.edi = 0;
  call_driver( &r );
  memcpy( target, send_buffer, 6 );
}

void Packet_send_pkt( void *buffer, uint16_t bufferLen ) {

  struct dos_registers r;

  if ( bufferLen > BUFFER_STRIDE ) bufferLen = BUFFER_STRIDE;
  memcpy( send_buffer, buffer, bufferLen );
  // (Ethernet's minimum frame: padded, as drivers expect)
  if ( bufferLen < 60 ) {
    memset( send_buffer + bufferLen, 0, 60 - bufferLen );
    bufferLen = 60;
  }
  Packets_sent++;
  for ( int attempt = 0; attempt < 5; attempt++ ) {
    memset( &r, 0, sizeof(r) );
    r.eax = 0x0400;
    r.ecx = bufferLen;
    r.ds = send_segment;
    r.esi = 0;
    if ( call_driver( &r ) == 0 ) return;
    Packets_send_retries++;
  }
  Packets_send_errs++;
}

void Packet_dumpStats( FILE *stream ) {
  fprintf( stream, "Packets: sent %lu received %lu dropped %lu send errors %lu, low free buffers %u\n",
           (unsigned long)Packets_sent, (unsigned long)(block ? block->received : 0),
           (unsigned long)(block ? block->dropped : 0), (unsigned long)Packets_send_errs, Buffer_lowFreeCount );
}

uint8_t  Packet_getSoftwareInt( void ) { return Packet_int; }
uint16_t Packet_getHandle( void ) { return Packet_handle; }
void     Packet_setHandle( uint16_t handle ) { Packet_handle = handle; }
void     Packet_setInt( uint8_t number ) { Packet_int = number; }
