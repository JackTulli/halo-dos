/*
   UTILS.CPP

   halo-dos: the parts of mTCP's Utils (UTILS.CPP) the DOS build's stack
   uses (its start is the game's: port/dos/dos/dos_net.cpp), and the clock
   (TIMER.CPP's ticks, from the game's own clock: no interrupt handler).

   mTCP: Copyright (C) 2005-2026 Michael B. Brutman (mbbrutman@gmail.com),
   GPLv3 (see LICENSE).
*/

#include <ctype.h>

#include "types.h"
#include "utils.h"
#include "packet.h"
#include "timer.h"
#include "arp.h"
#include "ip.h"
#include "tcp.h"

extern "C" {
#include "dos_dpmi.h"
}

// the BIOS's 18.2 ticks a second, from the game's clock (microseconds)
extern "C" uint32_t mtcp_timer_ticks( void ) {
  return (uint32_t)(dos_microseconds( ) / 54925ull);
}

void Utils::processPackets( void ) {
  PACKET_PROCESS_SINGLE;
  #ifdef COMPILE_TCP
  Tcp::drivePackets( );
  #endif
  #ifdef COMPILE_ARP
  Arp::driveArp( );
  #endif
}

char *Utils::getNextToken( char *input, char *target, uint16_t bufLen ) {

  if ( input == NULL ) {
    *target = 0;
    return NULL;
  }

  // Skip leading whitespace
  int l = strlen(input);
  int i=0;
  while ( (i<l) && (isspace(input[i])) ) {
    i++;
  }

  if ( i == l ) {
    *target=0;
    return NULL;
  }

  /*
  int j=0;
  // We are at the first non-space char
  for ( ; (i<l) && (!isspace(input[i])); i++,j++ ) {
    if ( j < bufLen ) target[j] = input[i];
  }
  */


  // State machine
  //
  // Normal        -> Quote                -> QuoteSeen
  // Normal        -> Space                -> Delimeter found, bail out
  // Normal        -> Normal Char          -> Normal: (Add char to str)
  // QuoteSeen     -> Normal Char or Space -> InQuoteRegion: (Add char to str)
  // QuoteSeen     -> Quote                -> Normal: (add quote to str)
  // InQuoteRegion -> Normal Char or Space -> InQuoteRegion: (Add char to str)
  // InQuoteRegion -> Quote                -> InQ_QSeen
  // InQ-QSeen     -> Normal Char          -> InQuoteRegion: (Add quote and char to str)
  // InQ-QSeen     -> Space                -> Delimeter found, bail out
  // InQ-QSeen     -> Quote                -> InQuoteRegion: (add quote to str)


  // We are at the first non-space character

  enum States { Normal, QuoteSeen, InQuoteRegion, InQuoteRegionQuoteSeen, DelimFound };

  States st = Normal;
  int j=0;

  for ( ; i<l; i++ ) {

    switch (st) {
      case Normal: {
        if ( input[i] == '"' ) { st = QuoteSeen; break; }
        if ( isspace( input[i] ) ) { st = DelimFound; break; }
        if ( j < bufLen) target[j++] = input[i];
        break;
      }
      case QuoteSeen: {
        if ( input[i] == '"' ) {
          st = Normal;
          if ( j < bufLen ) target[j++] = '"';
          break;
        }
        st = InQuoteRegion;
        if ( j < bufLen ) target[j++] = input[i];
        break;
      }
      case InQuoteRegion: {
        if ( input[i] == '"' ) {
          st = InQuoteRegionQuoteSeen;
          break;
        }
        if ( j < bufLen ) target[j++] = input[i];
        break;
      }
      case InQuoteRegionQuoteSeen: {
        if ( isspace( input[i] ) ) {
          st = DelimFound;
          break;
        }
        if ( input[i] == '"' ) {
          st = InQuoteRegion;
          if ( j < bufLen ) target[j++] = '"';
          break;
        }
        st = InQuoteRegion;
        if ( j < bufLen ) target[j++] = '"';
        if ( j < bufLen ) target[j++] = input[i];
        break;
      }
    }

    if ( st == DelimFound ) break;

  }

  if ( j < bufLen ) {
    target[j] = 0;
  }
  else {
    target[bufLen-1] = 0;
  }

  if ( i == l ) {
    return NULL;
  }

  return &input[i];
}
