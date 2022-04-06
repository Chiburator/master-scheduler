/*
 * Copyright (c) 2017, RISE SICS.
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
 *         MasterNet, Master's minimal network layer, an adaptation of NullNet
 * \author
 *         Oliver Harms <oha@informatik.uni-kiel.de>
 *
 */

/**
 * \addtogroup master-net
 * @{
 */

#include "contiki.h"
#include "dev/leds.h" //TODOLIV: remove before publication
#include "net/packetbuf.h"
#include "net/netstack.h"
#include "net/master-net/master-net.h"

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "MASTER-N"
#define LOG_LEVEL LOG_LEVEL_DBG


uint8_t *masternet_buf;
uint16_t masternet_len;
//uint8_t command[2];

packet_data_t packets[QUEUEBUF_CONF_NUM];
uint8_t current_packet_index = 0;

static masternet_input_callback current_input_callback = NULL;
static mac_callback_t current_output_callback = NULL;
static masternet_config_callback config_callback = NULL;
/*--------------------------------------------------------------------*/
static void
init(void)
{
  current_input_callback = NULL;
}
/*--------------------------------------------------------------------*/
static void
input(void)
{
  if(current_input_callback != NULL) {
    //below: might be too verbose timing wise
    // LOG_INFO("received %u bytes from ", packetbuf_datalen());
    // LOG_INFO_LLADDR(packetbuf_addr(PACKETBUF_ADDR_SENDER));
    // LOG_INFO_("\n");
    current_input_callback(packetbuf_dataptr(), packetbuf_datalen(),
      packetbuf_addr(PACKETBUF_ADDR_SENDER), packetbuf_addr(PACKETBUF_ADDR_RECEIVER));
  }
}
/*--------------------------------------------------------------------*/
void
masternet_set_input_callback(masternet_input_callback callback)
{
  current_input_callback = callback;
}
/*--------------------------------------------------------------------*/
void
masternet_set_output_callback(mac_callback_t callback)
{
  current_output_callback = callback;
}
/*--------------------------------------------------------------------*/
void
masternet_set_config_callback(masternet_config_callback callback)
{
  config_callback = callback;
}
/*--------------------------------------------------------------------*/
static uint8_t
output(const linkaddr_t *dest)
{
  int framer_hdrlen;
  int max_payload;
  //command[0] = 0;

  current_packet_index = (current_packet_index + 1) % QUEUEBUF_CONF_NUM;

  leds_on(LEDS_YELLOW);

  packetbuf_clear();
  packetbuf_copyfrom(masternet_buf, masternet_len);
    
  if(config_callback != NULL) {
    master_packetbuf_config_t packet_configuration = config_callback();
    //set max_transmissions
    packetbuf_set_attr(PACKETBUF_ATTR_MAX_MAC_TRANSMISSIONS, packet_configuration.max_tx);
    packetbuf_set_attr(PACKETBUF_ATTR_PACKET_NUMBER, packet_configuration.packet_nbr);

#   if TSCH_FLOW_BASED_QUEUES
      //packetbuf set flow number
      packetbuf_set_attr(PACKETBUF_ATTR_FLOW_NUMBER, packet_configuration.flow_number);
#   endif /* TSCH_FLOW_BASED_QUEUES */

#   if TSCH_TTL_BASED_RETRANSMISSIONS
      //packetbuf set TTL & earliest tx slot
      packetbuf_set_attr(PACKETBUF_ATTR_TRANSMISSION_TTL, packet_configuration.ttl_slot_number);
      packetbuf_set_attr(PACKETBUF_ATTR_EARLIEST_TX_SLOT, packet_configuration.earliest_tx_slot);
#   endif /* TSCH_TTL_BASED_RETRANSMISSIONS */

  #if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
    //The command will be transformed to a uint8_t
    //command[0] = packet_configuration.command;
    //LOG_INFO("Set command to %d\n", packet_configuration.command);
    packets[current_packet_index].command = packet_configuration.command;
    packets[current_packet_index].packet_nbr = packet_configuration.packet_nbr;

    packetbuf_set_attr(PACKETBUF_ATTR_MAC_METADATA, packet_configuration.important_packet);
  #if TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES
    packetbuf_set_attr(PACKETBUF_ATTR_SEND_NBR, packet_configuration.send_to_nbr);
  #endif
  #endif
  }

  if(dest != NULL) {
    packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, dest);
  } else {
    packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &linkaddr_null);
  }
  packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &linkaddr_node_addr);

  framer_hdrlen = NETSTACK_FRAMER.length();
  if(framer_hdrlen < 0) {
    /* Framing failed, we assume the maximum header length */
    framer_hdrlen = FIXED_HDRLEN;
  }

  #if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
    //The command will be transformed to a uint8_t
    //command[1] = framer_hdrlen;
    packets[current_packet_index].hdr_len = framer_hdrlen;
  #endif

  max_payload = MAC_MAX_PAYLOAD - framer_hdrlen;

  if (masternet_len <= max_payload){
    //TODOLIV: below: might be too verbose timing wise
    //LOG_INFO("sending %u bytes to ", packetbuf_datalen());
    //LOG_INFO_LLADDR(packetbuf_addr(PACKETBUF_ADDR_RECEIVER));
    //LOG_INFO_("\n");
    leds_off(LEDS_YELLOW);
    NETSTACK_MAC.send(current_output_callback, &packets[current_packet_index]);
    //LOG_DBG("Masternet send to %i\n", packetbuf_addr(PACKETBUF_ADDR_RECEIVER)->u8[NODE_ID_INDEX]);
    return 1;
  } else {
    LOG_ERR("sending failed: %u bytes of %u possible bytes to ", masternet_len, max_payload);
    LOG_ERR_LLADDR(packetbuf_addr(PACKETBUF_ADDR_RECEIVER));
    LOG_ERR_("\n");
    leds_off(LEDS_YELLOW);
    packetbuf_clear();
    return 0;
  }
  
}
/*--------------------------------------------------------------------*/
const struct network_driver masternet_driver = {
  "masternet",
  init,
  input,
  output
};
/*--------------------------------------------------------------------*/
/** @} */
