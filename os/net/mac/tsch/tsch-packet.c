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
 * \file
 *         TSCH packet format management
 * \author
 *         Simon Duquennoy <simonduq@sics.se>
 *         Beshr Al Nahas <beshr@sics.se>
 */

/**
 * \addtogroup tsch
 * @{
 */

#include "contiki.h"
#include "net/packetbuf.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-packet.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/mac/tsch/tsch-security.h"
#include "net/mac/framer/frame802154.h"
#include "net/mac/framer/framer-802154.h"
#include "net/netstack.h"
#include "lib/ccm-star.h"
#include "lib/aes-128.h"

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "TSCH Pkt"
#define LOG_LEVEL LOG_LEVEL_INFO

/* The sequence number of the latest sent EB */
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
uint16_t sequence_number = 0;
uint8_t tsch_rank = 255;
uint8_t schedule_version = 0;
uint16_t schedule_packets = 0;
#endif /* TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY */

/*
 * We use a local packetbuf_attr array to collect necessary frame settings to
 * create an EACK because EACK is generated in the interrupt context where
 * packetbuf and packetbuf_attrs[] may be in use for another purpose.
 *
 * We have accessors of eackbuf_attrs: tsch_packet_eackbuf_set_attr() and
 * tsch_packet_eackbuf_attr(). For some platform, they might need to be
 * implemented as inline functions. However, for now, we don't provide the
 * inline option. Such an optimization is left to the compiler for a target
 * platform.
 */
static struct packetbuf_attr eackbuf_attrs[PACKETBUF_NUM_ATTRS];

/*---------------------------------------------------------------------------*/
static int
tsch_packet_eackbuf_set_attr(uint8_t type, const packetbuf_attr_t val)
{
  eackbuf_attrs[type].val = val;
  return 1;
}
/*---------------------------------------------------------------------------*/
/* Return the value of a specified attribute */
packetbuf_attr_t
tsch_packet_eackbuf_attr(uint8_t type)
{
  return eackbuf_attrs[type].val;
}
/*---------------------------------------------------------------------------*/
/* Construct enhanced ACK packet and return ACK length */
int tsch_packet_create_eack(uint8_t *buf, uint16_t buf_len,
                            const linkaddr_t *dest_addr, uint8_t seqno,
                            int16_t drift, int nack)
{
  frame802154_t params;
  struct ieee802154_ies ies;
  int hdr_len;
  int ack_len;

  if (buf == NULL)
  {
    return -1;
  }

  memset(eackbuf_attrs, 0, sizeof(eackbuf_attrs));

  tsch_packet_eackbuf_set_attr(PACKETBUF_ATTR_FRAME_TYPE, FRAME802154_ACKFRAME);
  tsch_packet_eackbuf_set_attr(PACKETBUF_ATTR_MAC_METADATA, 1);
  tsch_packet_eackbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, seqno);

  tsch_packet_eackbuf_set_attr(PACKETBUF_ATTR_MAC_NO_DEST_ADDR, 1);
#if TSCH_PACKET_EACK_WITH_DEST_ADDR
  if (dest_addr != NULL)
  {
    tsch_packet_eackbuf_set_attr(PACKETBUF_ATTR_MAC_NO_DEST_ADDR, 0);
    linkaddr_copy((linkaddr_t *)&params.dest_addr, dest_addr);
  }
#endif

  tsch_packet_eackbuf_set_attr(PACKETBUF_ATTR_MAC_NO_SRC_ADDR, 1);
#if TSCH_PACKET_EACK_WITH_SRC_ADDR
  tsch_packet_eackbuf_set_attr(PACKETBUF_ATTR_MAC_NO_SRC_ADDR, 0);
  linkaddr_copy((linkaddr_t *)&params.src_addr, &linkaddr_node_addr);
#endif

#if LLSEC802154_ENABLED
  if (tsch_is_pan_secured)
  {
    tsch_packet_eackbuf_set_attr(PACKETBUF_ATTR_SECURITY_LEVEL,
                                 TSCH_SECURITY_KEY_SEC_LEVEL_ACK);
    tsch_packet_eackbuf_set_attr(PACKETBUF_ATTR_KEY_ID_MODE,
                                 FRAME802154_1_BYTE_KEY_ID_MODE);
    tsch_packet_eackbuf_set_attr(PACKETBUF_ATTR_KEY_INDEX,
                                 TSCH_SECURITY_KEY_INDEX_ACK);
  }
#endif /* LLSEC802154_ENABLED */

  framer_802154_setup_params(tsch_packet_eackbuf_attr, 0, &params);
  hdr_len = frame802154_hdrlen(&params);

  memset(buf, 0, buf_len);

  /* Setup IE timesync */
  memset(&ies, 0, sizeof(ies));
  ies.ie_time_correction = drift;
  ies.ie_is_nack = nack;

  ack_len =
      frame80215e_create_ie_header_ack_nack_time_correction(buf + hdr_len,
                                                            buf_len - hdr_len, &ies);
  if (ack_len < 0)
  {
    return -1;
  }
  ack_len += hdr_len;

  frame802154_create(&params, buf);

  return ack_len;
}
/*---------------------------------------------------------------------------*/
/* Parse enhanced ACK packet, extract drift and nack */
int tsch_packet_parse_eack(const uint8_t *buf, int buf_size,
                           uint8_t seqno, frame802154_t *frame, struct ieee802154_ies *ies, uint8_t *hdr_len)
{
  uint8_t curr_len = 0;
  int ret;
  linkaddr_t dest;

  if (frame == NULL || buf_size < 0)
  {
    return 0;
  }
  /* Parse 802.15.4-2006 frame, i.e. all fields before Information Elements */
  if ((ret = frame802154_parse((uint8_t *)buf, buf_size, frame)) < 3)
  {
    return 0;
  }
  if (hdr_len != NULL)
  {
    *hdr_len = ret;
  }
  curr_len += ret;

  /* Check seqno */
  if (seqno != frame->seq)
  {
    return 0;
  }

  /* Check destination PAN ID */
  if (frame802154_check_dest_panid(frame) == 0)
  {
    return 0;
  }

  /* Check destination address (if any) */
  if (frame802154_extract_linkaddr(frame, NULL, &dest) == 0 ||
      (!linkaddr_cmp(&dest, &linkaddr_node_addr) && !linkaddr_cmp(&dest, &linkaddr_null)))
  {
    return 0;
  }

  if (ies != NULL)
  {
    memset(ies, 0, sizeof(struct ieee802154_ies));
  }

  if (frame->fcf.ie_list_present)
  {
    int mic_len = 0;
#if LLSEC802154_ENABLED
    /* Check if there is space for the security MIC (if any) */
    mic_len = tsch_security_mic_len(frame);
    if (buf_size < curr_len + mic_len)
    {
      return 0;
    }
#endif /* LLSEC802154_ENABLED */
    /* Parse information elements. We need to substract the MIC length, as the exact payload len is needed while parsing */
    if ((ret = frame802154e_parse_information_elements(buf + curr_len, buf_size - curr_len - mic_len, ies)) == -1)
    {
      return 0;
    }
    curr_len += ret;
  }

  if (hdr_len != NULL)
  {
    *hdr_len += ies->ie_payload_ie_offset;
  }

  return curr_len;
}
/*---------------------------------------------------------------------------*/
/* Create an EB packet */
int tsch_packet_create_eb(uint8_t *hdr_len, uint8_t *tsch_sync_ie_offset)
{
  struct ieee802154_ies ies;
  uint8_t *p;
  int ie_len;
  const uint16_t payload_ie_hdr_len = 2;

  packetbuf_clear();

  /* Prepare Information Elements for inclusion in the EB */
  memset(&ies, 0, sizeof(ies));

  /* Add TSCH timeslot timing IE. */
#if TSCH_PACKET_EB_WITH_TIMESLOT_TIMING
  {
    int i;
    ies.ie_tsch_timeslot_id = 1;
    for (i = 0; i < tsch_ts_elements_count; i++)
    {
      ies.ie_tsch_timeslot[i] = RTIMERTICKS_TO_US(tsch_timing[i]);
    }
  }
#endif /* TSCH_PACKET_EB_WITH_TIMESLOT_TIMING */

  /* Add TSCH hopping sequence IE */
#if TSCH_PACKET_EB_WITH_HOPPING_SEQUENCE
  if (tsch_hopping_sequence_length.val <= sizeof(ies.ie_hopping_sequence_list))
  {
    ies.ie_channel_hopping_sequence_id = 1;
    ies.ie_hopping_sequence_len = tsch_hopping_sequence_length.val;
    memcpy(ies.ie_hopping_sequence_list, tsch_hopping_sequence,
           ies.ie_hopping_sequence_len);
  }
#endif /* TSCH_PACKET_EB_WITH_HOPPING_SEQUENCE */

  /* Add Slotframe and Link IE */
#if TSCH_PACKET_EB_WITH_SLOTFRAME_AND_LINK
  {
    /* Send slotframe 0 with link at timeslot 0 */
    struct tsch_slotframe *sf0 = tsch_schedule_get_slotframe_by_handle(0);
    struct tsch_link *link0 = tsch_schedule_get_link_by_timeslot(sf0, 0);
    if (sf0 && link0)
    {
      ies.ie_tsch_slotframe_and_link.num_slotframes = 1;
      ies.ie_tsch_slotframe_and_link.slotframe_handle = sf0->handle;
      ies.ie_tsch_slotframe_and_link.slotframe_size = sf0->size.val;
      ies.ie_tsch_slotframe_and_link.num_links = 1;
      ies.ie_tsch_slotframe_and_link.links[0].timeslot = link0->timeslot;
      ies.ie_tsch_slotframe_and_link.links[0].channel_offset =
          link0->channel_offset;
      ies.ie_tsch_slotframe_and_link.links[0].link_options =
          link0->link_options;
    }
  }
#endif /* TSCH_PACKET_EB_WITH_SLOTFRAME_AND_LINK */

#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
  ies.ie_sequence_number = ++sequence_number;
  ies.ie_rank = tsch_rank;
  struct tsch_neighbor* time_source = tsch_queue_get_time_source();
  if(tsch_is_coordinator)
  {
    ies.ie_time_source =  linkaddr_node_addr.u8[NODE_ID_INDEX];
  }else{
    ies.ie_time_source = time_source->addr.u8[NODE_ID_INDEX];
  }

  ies.ie_schedule_version = schedule_version;
  ies.ie_schedule_packets = schedule_packets;
  memcpy(ies.ie_schedule_received, schedule_received, FRAME802154E_IE_SCHEDULE_BIT_VECTOR);
#endif /* TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY */

  p = packetbuf_dataptr();

  ie_len = frame80215e_create_ie_tsch_synchronization(p,
                                                      packetbuf_remaininglen(),
                                                      &ies);
  if (ie_len < 0)
  {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);

  ie_len = frame80215e_create_ie_tsch_timeslot(p,
                                               packetbuf_remaininglen(),
                                               &ies);
  if (ie_len < 0)
  {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);

  ie_len = frame80215e_create_ie_tsch_channel_hopping_sequence(p,
                                                               packetbuf_remaininglen(),
                                                               &ies);
  if (ie_len < 0)
  {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);

  ie_len = frame80215e_create_ie_tsch_slotframe_and_link(p,
                                                         packetbuf_remaininglen(),
                                                         &ies);
  if (ie_len < 0)
  {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);

#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
  ie_len = frame80215e_create_ie_tsch_neighbor_discovery(p,
                                                         packetbuf_remaininglen(),
                                                         &ies);
  if (ie_len < 0)
  {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);

  ie_len = frame80215e_create_ie_tsch_rank(p,
                                           packetbuf_remaininglen(),
                                           &ies);
  if (ie_len < 0)
  {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);

  ie_len = frame80215e_create_ie_time_source(p,
                                             packetbuf_remaininglen(),
                                             &ies);
  if (ie_len < 0)
  {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);
  
  ie_len = frame80215e_create_ie_schedule_version(p,
                                                  packetbuf_remaininglen(),
                                                  &ies);
  if (ie_len < 0)
  {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);

  ie_len = frame80215e_create_ie_schedule_packets(p,
                                                  packetbuf_remaininglen(),
                                                  &ies);
  if (ie_len < 0)
  {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);

  ie_len = frame80215e_create_ie_schedule_received(p,
                                                  packetbuf_remaininglen(),
                                                  &ies,
                                                  FRAME802154E_IE_SCHEDULE_BIT_VECTOR);
  if (ie_len < 0)
  {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);

#endif /* TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY */

#if 0
  /* Payload IE list termination: optional */
  ie_len = frame80215e_create_ie_payload_list_termination(p,
                                                          packetbuf_remaininglen(),
                                                          &ies);
  if(ie_len < 0) {
    return -1;
  }
  p += ie_len;
  packetbuf_set_datalen(packetbuf_datalen() + ie_len);
#endif
  ies.ie_mlme_len = packetbuf_datalen();

  /* make room for Payload IE header */
  //Shift data 2 places to the right. Set the payload MLME (MAC sublayer management entity) in the 2 free bytes
  memmove((uint8_t *)packetbuf_dataptr() + payload_ie_hdr_len,
          packetbuf_dataptr(), packetbuf_datalen());
  packetbuf_set_datalen(packetbuf_datalen() + payload_ie_hdr_len);
  ie_len = frame80215e_create_ie_mlme(packetbuf_dataptr(),
                                      packetbuf_remaininglen(),
                                      &ies);
  if (ie_len < 0)
  {
    return -1;
  }

  /* allocate space for Header Termination IE, the size of which is 2 octets */
  packetbuf_hdralloc(2);
  ie_len = frame80215e_create_ie_header_list_termination_1(packetbuf_hdrptr(),
                                                           packetbuf_remaininglen(),
                                                           &ies);
  if (ie_len < 0)
  {
    return -1;
  }

  packetbuf_set_attr(PACKETBUF_ATTR_FRAME_TYPE, FRAME802154_BEACONFRAME);
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_METADATA, 1);

  packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &linkaddr_node_addr);
  packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &tsch_eb_address);

#if LLSEC802154_ENABLED
  if (tsch_is_pan_secured)
  {
    packetbuf_set_attr(PACKETBUF_ATTR_SECURITY_LEVEL,
                       TSCH_SECURITY_KEY_SEC_LEVEL_EB);
    packetbuf_set_attr(PACKETBUF_ATTR_KEY_ID_MODE,
                       FRAME802154_1_BYTE_KEY_ID_MODE);
    packetbuf_set_attr(PACKETBUF_ATTR_KEY_INDEX,
                       TSCH_SECURITY_KEY_INDEX_EB);
  }
#endif /* LLSEC802154_ENABLED */

  if (NETSTACK_FRAMER.create() < 0)
  {
    return -1;
  }

  if (hdr_len != NULL)
  {
    *hdr_len = packetbuf_hdrlen();
  }

  /*
   * Save the offset of the TSCH Synchronization IE, which is expected to be
   * located just after the Payload IE header, needed to update ASN and join
   * priority before sending.
   */
  if (tsch_sync_ie_offset != NULL)
  {
    *tsch_sync_ie_offset = packetbuf_hdrlen() + payload_ie_hdr_len;
  }

  return packetbuf_totlen();
}

#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
int tsch_packet_create_unicast()
{
  //The payload ie_hdr_len is 2 bytes and the ie for ie_overhearing is 3 bytes
  uint8_t payload_ie_hdr_len = 2;
  uint8_t payload_ie_len = 3;
  uint8_t payload_ie_termination2 = 2;

  //Prepare the only ies we need for a unicast
  struct ieee802154_ies ies;
  memset(&ies, 0, sizeof(ies));
  ies.ie_overhearing = 1;
  ies.ie_mlme_len = payload_ie_len; //Set the length for the payload ies

  int ie_len = 0;

  /* make room for Payload IE header */
  //Shift data to the right
  memmove((uint8_t *)packetbuf_dataptr() + payload_ie_hdr_len + payload_ie_len + payload_ie_termination2, packetbuf_dataptr(), packetbuf_datalen());

  //Set the new packet buffer len
  packetbuf_set_datalen(packetbuf_datalen() + payload_ie_hdr_len + payload_ie_len + payload_ie_termination2);

  //Set the payload MLME (MAC sublayer management entity) in the first 2 bytes
  ie_len = frame80215e_create_ie_mlme(packetbuf_dataptr(),
                                      packetbuf_remaininglen(),
                                      &ies);
  if (ie_len < 0)
  {
    return -1;
  }

  //Set the IE for important packets (3 bytes)
  ie_len = frame80215e_create_ie_overhearing(packetbuf_dataptr() + payload_ie_hdr_len,
                                             packetbuf_remaininglen(),
                                             &ies);
  if (ie_len < 0)
  {
    return -1;
  }

  //Set the IE for important packets (3 bytes)
  ie_len = frame80215e_create_ie_payload_list_termination(packetbuf_dataptr() + payload_ie_hdr_len + payload_ie_len,
                                                          packetbuf_remaininglen(),
                                                          &ies);
  if (ie_len < 0)
  {
    return -1;
  }

  /* allocate space for Header Termination IE, the size of which is 2 octets */
  packetbuf_hdralloc(2);
  ie_len = frame80215e_create_ie_header_list_termination_1(packetbuf_hdrptr(),
                                                           packetbuf_remaininglen(),
                                                           &ies);
  if (ie_len < 0)
  {
    return -1;
  }

  return 1;
}
#endif

/*---------------------------------------------------------------------------*/
/* Update ASN in EB packet */
int tsch_packet_update_eb(uint8_t *buf, int buf_size, uint8_t tsch_sync_ie_offset)
{
  struct ieee802154_ies ies;
  ies.ie_asn = tsch_current_asn;
  ies.ie_join_priority = tsch_join_priority;
  frame80215e_create_ie_tsch_synchronization(buf + tsch_sync_ie_offset, buf_size - tsch_sync_ie_offset, &ies);
  return 1;
}
/*---------------------------------------------------------------------------*/
/* Parse a IEEE 802.15.4e TSCH Enhanced Beacon (EB) */
int tsch_packet_parse_eb(const uint8_t *buf, int buf_size,
                         frame802154_t *frame, struct ieee802154_ies *ies, uint8_t *hdr_len, int frame_without_mic)
{
  uint8_t curr_len = 0;
  int ret;

  if (frame == NULL || buf_size < 0)
  {
    return 0;
  }

  /* Parse 802.15.4-2006 frame, i.e. all fields before Information Elements */
  if ((ret = frame802154_parse((uint8_t *)buf, buf_size, frame)) == 0)
  {
    LOG_ERR("! parse_eb: failed to parse frame\n");
    return 0;
  }

  if (frame->fcf.frame_version < FRAME802154_IEEE802154_2015 || frame->fcf.frame_type != FRAME802154_BEACONFRAME)
  {
    LOG_INFO("! parse_eb: frame is not a valid TSCH beacon. Frame version %u, type %u, FCF %02x %02x\n",
             frame->fcf.frame_version, frame->fcf.frame_type, buf[0], buf[1]);
    LOG_INFO("! parse_eb: frame was from 0x%x/", frame->src_pid);
    LOG_INFO_LLADDR((const linkaddr_t *)&frame->src_addr);
    LOG_INFO_(" to 0x%x/", frame->dest_pid);
    LOG_INFO_LLADDR((const linkaddr_t *)&frame->dest_addr);
    LOG_INFO_("\n");
    return 0;
  }

  if (hdr_len != NULL)
  {
    *hdr_len = ret;
  }
  curr_len += ret;

  if (ies != NULL)
  {
    memset(ies, 0, sizeof(struct ieee802154_ies));
    ies->ie_join_priority = 0xff; /* Use max value in case the Beacon does not include a join priority */
  }
  if (frame->fcf.ie_list_present)
  {
    /* Calculate space needed for the security MIC, if any, before attempting to parse IEs */
    int mic_len = 0;
#if LLSEC802154_ENABLED
    if (!frame_without_mic)
    {
      mic_len = tsch_security_mic_len(frame);
      if (buf_size < curr_len + mic_len)
      {
        return 0;
      }
    }
#endif /* LLSEC802154_ENABLED */

    /* Parse information elements. We need to substract the MIC length, as the exact payload len is needed while parsing */
    if ((ret = frame802154e_parse_information_elements(buf + curr_len, buf_size - curr_len - mic_len, ies)) == -1)
    {
      LOG_ERR("! parse_eb: failed to parse IEs\n");
      return 0;
    }
    curr_len += ret;
  }

  if (hdr_len != NULL)
  {
    *hdr_len += ies->ie_payload_ie_offset;
  }

  return curr_len;
}
/*---------------------------------------------------------------------------*/
/** @} */
