/*
 * Copyright (c) 2014, SICS Swedish ICT.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

/**
 * \addtogroup tsch
 * @{
*/

#ifndef __TSCH_PACKET_H__
#define __TSCH_PACKET_H__

/********** Includes **********/

#include "contiki.h"
#include "net/packetbuf.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/framer/frame802154.h"
#include "net/mac/framer/frame802154e-ie.h"

/******** Configuration *******/

/* TSCH EB: include timeslot timing Information Element? */
#ifdef TSCH_PACKET_CONF_EB_WITH_TIMESLOT_TIMING
#define TSCH_PACKET_EB_WITH_TIMESLOT_TIMING TSCH_PACKET_CONF_EB_WITH_TIMESLOT_TIMING
#else
#define TSCH_PACKET_EB_WITH_TIMESLOT_TIMING 0
#endif

/* TSCH EB: include hopping sequence Information Element? */
#ifdef TSCH_PACKET_CONF_EB_WITH_HOPPING_SEQUENCE
#define TSCH_PACKET_EB_WITH_HOPPING_SEQUENCE TSCH_PACKET_CONF_EB_WITH_HOPPING_SEQUENCE
#else
#define TSCH_PACKET_EB_WITH_HOPPING_SEQUENCE 0
#endif

/* TSCH EB: include slotframe and link Information Element? */
#ifdef TSCH_PACKET_CONF_EB_WITH_SLOTFRAME_AND_LINK
#define TSCH_PACKET_EB_WITH_SLOTFRAME_AND_LINK TSCH_PACKET_CONF_EB_WITH_SLOTFRAME_AND_LINK
#else
#define TSCH_PACKET_EB_WITH_SLOTFRAME_AND_LINK 0
#endif

/* Include source address in ACK? */
#ifdef TSCH_PACKET_CONF_EACK_WITH_SRC_ADDR
#define TSCH_PACKET_EACK_WITH_SRC_ADDR TSCH_PACKET_CONF_EACK_WITH_SRC_ADDR
#else
#define TSCH_PACKET_EACK_WITH_SRC_ADDR 0
#endif

/* Include destination address in ACK? */
#ifdef TSCH_PACKET_CONF_EACK_WITH_DEST_ADDR
#define TSCH_PACKET_EACK_WITH_DEST_ADDR TSCH_PACKET_CONF_EACK_WITH_DEST_ADDR
#else
#define TSCH_PACKET_EACK_WITH_DEST_ADDR 1 /* Include destination address
by default, useful in case of duplicate seqno */
#endif

/********** Constants *********/

/* Max TSCH packet lenght */
#define TSCH_PACKET_MAX_LEN MIN(127,PACKETBUF_SIZE)

/********** Variables *********/

/* The sequence number of the latest sent EB */
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
extern uint16_t sequence_number;
extern uint8_t tsch_rank;
extern uint8_t schedule_version;
extern uint16_t schedule_packets;
#endif /* TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY */

/********** Functions *********/

/* Construct enhanced ACK packet and return ACK length */
int tsch_packet_create_eack(uint8_t *buf, uint16_t buf_size,
                            const linkaddr_t *dest_addr, uint8_t seqno,
                            int16_t drift, int nack);
/* Parse enhanced ACK packet, extract drift and nack */
int tsch_packet_parse_eack(const uint8_t *buf, int buf_size,
    uint8_t seqno, frame802154_t *frame, struct ieee802154_ies *ies, uint8_t *hdr_len);
/* Create an EB packet */
int tsch_packet_create_eb(uint8_t *hdr_len, uint8_t *tsch_sync_ie_ptr);
/* Update ASN in EB packet */
int tsch_packet_update_eb(uint8_t *buf, int buf_size, uint8_t tsch_sync_ie_offset);
/* Parse EB and extract ASN and join priority */
int tsch_packet_parse_eb(const uint8_t *buf, int buf_size,
    frame802154_t *frame, struct ieee802154_ies *ies,
    uint8_t *hdrlen, int frame_without_mic);

#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
/** @brief Add an information elements to the unicast payload if possible.
 * 
 * @return 1 if the IE was added. -1 otherwise.
*/
int tsch_packet_create_unicast();
#endif

#endif /* __TSCH_PACKET_H__ */
/** @} */
