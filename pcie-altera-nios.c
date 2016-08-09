/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * 
 * Copyright (c) 2015-2018 Colin Rothwell
 * Copyright (c) 2015-2018 A. Theodore Markettos
 * 
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract FA8750-10-C-0237
 * ("CTSRD"), as part of the DARPA CRASH research programme.
 * 
 * We acknowledge the support of Arm Ltd.
 * 
 * We acknowledge the support of EPSRC.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdint.h>
#include "system.h"
#include "pcie.h"
#include "mask.h"
#include "pcie-debug.h"
#include "pciefpga.h"
#include "beri-io.h"
#include "log.h"
#include "pcie-backend.h"

#include "sys/alt_timestamp.h"
#include "io.h"

volatile uint8_t *physmem;
volatile uint8_t *led_phys_mem;

void
initialise_leds()
{
#define LED_BASE    0x7F006000LL
#define LED_LEN     0x1

#ifdef BERIBSD
    led_phys_mem = open_io_region(LED_BASE, LED_LEN);
#else
    led_phys_mem = (volatile uint8_t *) LED_BASE;

#undef LED_LEN
#undef LED_BASE
#endif
}

int
pcie_hardware_init(int argc, char **argv, volatile uint8_t **physmem)
{
#ifdef BERIBSD
  *physmem = open_io_region(PCIEPACKET_REGION_BASE, PCIEPACKET_REGION_LENGTH);
#else
  *physmem = (volatile uint8_t *) PCIEPACKET_REGION_BASE;
#endif
  initialise_leds();
  return 0;
}

unsigned long
read_hw_counter()
{
  unsigned long retval;
  retval = alt_timestamp();
  return retval;
}



/* tlp_len is length of the buffer in bytes. */
/* Return -1 if 1024 attempts to poll the buffer fail. */
int
wait_for_tlp(volatile TLPQuadWord *tlp, int tlp_len, uint64_t end_time)
{

  /* Real approach: no POSTGRES */
  volatile PCIeStatus pciestatus;
  volatile TLPDoubleWord pciedata1, pciedata0;
  TLPDoubleWord *tlpd = (TLPDoubleWord *) tlp;
  volatile int ready;
  int i = 0; // i is "length of TLP so far received in doublewords.
  /*printf("in wait for tlp end time ");*/
  /*write_uint_64(end_time, '0');*/
  /*printf(" curr time ");*/
  /*write_uint_64(read_hw_counter(), '0');*/
  /*printf("\n");*/
  /*printf("curr time ");*/
  /*write_uint_64(read_hw_counter(), '0');*/
  /*printf("\n");*/

  do {
    ready = IORD(PCIEPACKETRECEIVER_0_BASE, PCIEPACKETRECEIVER_READY);
    if (read_hw_counter() >= end_time) {
      return -1;
    }
  } while (ready == 0);
  /*printf("ready\n");*/

  do {
    pciestatus.word = IORD(PCIEPACKETRECEIVER_0_BASE,
      PCIEPACKETRECEIVER_STATUS);
    // start at the beginning of the buffer once we get start of packet
    if (pciestatus.bits.startofpacket) {
      i = 0;
    }
    pciedata1 = IORD(PCIEPACKETRECEIVER_0_BASE,PCIEPACKETRECEIVER_UPPER32);
    pciedata0 = IORD(PCIEPACKETRECEIVER_0_BASE,PCIEPACKETRECEIVER_LOWER32DEQ);
    tlpd[i++] = pciedata0;
    tlpd[i++] = pciedata1;
    if ((i * 4) > tlp_len) {
      writeString("TLP RECV OVERFLOW\r\n");
//      PDBG("ERROR: TLP Larger than buffer.");
      return -1;
    }
/*
    write_uint_32_hex(pciestatus.word, ' ');
    write_uint_32(pciestatus.bits.pad1, ' '); putchar('P');
    write_uint_32(pciestatus.bits.byteenable, ' '); putchar('N');
    write_uint_32(pciestatus.bits.startofpacket, ' '); putchar('S');
    write_uint_32(pciestatus.bits.endofpacket, ' '); putchar('Z');
    write_uint_32(pciestatus.bits.pad2, ' '); putchar('p');
    write_uint_32(i,' '); putchar('i');
*/
    if (read_hw_counter() >= end_time) {
      return -1;
    }
  } while (!pciestatus.bits.endofpacket);
  /*printf("returning from wait\n");*/

  return (i * 4);

}

void
drain_pcie_core()
{
  while (IORD(PCIEPACKETRECEIVER_0_BASE, PCIEPACKETRECEIVER_READY)) {
    IORD(PCIEPACKETRECEIVER_0_BASE, PCIEPACKETRECEIVER_STATUS);
    IORD(PCIEPACKETRECEIVER_0_BASE,PCIEPACKETRECEIVER_UPPER32);
    IORD(PCIEPACKETRECEIVER_0_BASE,PCIEPACKETRECEIVER_LOWER32DEQ);
    for (int i = 0; i < (1 << 10); ++i) {
      asm("nop");
    }
  }
}


/* tlp is a pointer to the tlp, tlp_len is the length of the tlp in bytes. */
/* returns 0 on success. */
int
send_tlp(TLPQuadWord *header, int header_len, TLPQuadWord *data, int data_len,
  enum tlp_data_alignment data_alignment)
{
  /* Special case for:
   * 3DW, Unaligned data. Send qword of remaining header dword, first data.
   *   Construct qwords from unaligned data and send.
   */
#define WR_STATUS(STATUS) \
  do {                                  \
    IOWR(PCIEPACKETTRANSMITTER_0_BASE, PCIEPACKETTRANSMITTER_STATUS,  \
      STATUS);                            \
  } while (0)

#define WR_DATA(DATA) \
  do {                                  \
    IOWR(PCIEPACKETTRANSMITTER_0_BASE,PCIEPACKETTRANSMITTER_UPPER32,  \
      ((DATA)>>32LL));                        \
    IOWR(PCIEPACKETTRANSMITTER_0_BASE, PCIEPACKETTRANSMITTER_LOWER32SEND,\
      ((DATA) & 0xFFFFFFFFLL));                   \
  } while (0)

  int byte_index;
  volatile PCIeStatus statusword;
  TLPQuadWord sendqword;
  TLPDoubleWord *data_dword = (TLPDoubleWord *)data;

  statusword.word = 0;
  statusword.bits.startofpacket = 1;
  WR_STATUS(statusword.word);
  WR_DATA(header[0]);

  statusword.word = 0;
  // Stops the TX queue from draining whilst we're filling it.
  IOWR(PCIEPACKETTRANSMITTER_0_BASE, PCIEPACKETTRANSMITTER_QUEUEENABLE, 0);

  if (header_len == 12 && data_alignment == TDA_UNALIGNED) {
    sendqword = header[1] << 32;
    if (data_len > 0) {
      sendqword |= data_dword[0];
    }
    statusword.bits.endofpacket = (data_len <= 4);
    WR_STATUS(statusword.word);
    WR_DATA(sendqword);
    for (byte_index = 4; byte_index < data_len; byte_index += 8) {
      statusword.bits.endofpacket = ((byte_index + 8) >= data_len);
      sendqword = (TLPQuadWord)(data_dword[byte_index / 4]) << 32;
      sendqword |= data_dword[(byte_index / 4) + 1];
      WR_STATUS(statusword.word);
      WR_DATA(sendqword);
    }
  } else {
    statusword.bits.endofpacket = (data_len == 0);
    WR_STATUS(statusword.word);
    WR_DATA(header[1]);
    for (byte_index = 0; byte_index < data_len; byte_index += 8) {
      statusword.bits.endofpacket = ((byte_index + 8) >= data_len);
      WR_STATUS(statusword.word);
      WR_DATA(data[byte_index / 8]);
    }
  }

  // Release queued data
  IOWR(PCIEPACKETTRANSMITTER_0_BASE, PCIEPACKETTRANSMITTER_QUEUEENABLE, 1);

  return 0;
#undef WR_STATUS
#undef WR_DATA
}


/* tlp is a pointer to the tlp, tlp_len is the length of the tlp in bytes. */
/* returns 0 on success. */
int
send_tlp_unaligned(volatile TLPQuadWord *tlp, int tlp_len)
{
  int quad_word_index;
  volatile PCIeStatus statusword;
  TLPDoubleWord upperword=0, lowerword=0;

  //log_log(LS_SEND_LENGTH, LIF_INT_32, tlp_len, true);

  assert(tlp_len / 8 < 64);

  // Stops the TX queue from draining whilst we're filling it.
  IOWR(PCIEPACKETTRANSMITTER_0_BASE, PCIEPACKETTRANSMITTER_QUEUEENABLE, 0);

  int ceil_tlp_len = tlp_len + 7;

  for (quad_word_index = 0; quad_word_index < (ceil_tlp_len / 8);
      ++quad_word_index) {
    statusword.word = 0;
    statusword.bits.startofpacket = (quad_word_index == 0);
    statusword.bits.endofpacket =
      ((quad_word_index + 1) >= (ceil_tlp_len / 8));

    if ((quad_word_index+1) >= tlp_len)
      upperword = 0;
    else
      upperword = (tlp[quad_word_index]>>32);

    lowerword = tlp[quad_word_index] & 0xFFFFFFFF;

    // Write status word.
    IOWR(PCIEPACKETTRANSMITTER_0_BASE, PCIEPACKETTRANSMITTER_STATUS,
      statusword.word);
    // write upper 32 bits
    IOWR(PCIEPACKETTRANSMITTER_0_BASE,PCIEPACKETTRANSMITTER_UPPER32,
      upperword);
    // write lower 32 bits and send word
    IOWR(PCIEPACKETTRANSMITTER_0_BASE, PCIEPACKETTRANSMITTER_LOWER32SEND,
      lowerword);
    //printf("Sending upper word %08x lower word %08x status %08x\n", upperword, lowerword, statusword.word);
  }
  // Release queued data
  IOWR(PCIEPACKETTRANSMITTER_0_BASE, PCIEPACKETTRANSMITTER_QUEUEENABLE, 1);

  record_time();
  //log_log(LS_PACKET_SENT, LIF_NONE, 0, true);

  return 0;
}

void
close_connections()
{
}

