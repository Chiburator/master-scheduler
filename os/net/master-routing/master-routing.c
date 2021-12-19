/*
 * Copyright (c) 2011, Swedish Institute of Computer Science.
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
 */

/**
 * \file
 *         Implementation of the Master Routing module.
 * \author
 *         Oliver Harms <oha@informatik.uni-kiel.de>
 *
 */

/**
 * \addtogroup master-routing
 * @{ TEST
 */

#include "contiki.h"
#include "master-routing.h"
#include "dev/leds.h"
#include "net/netstack.h"
#include "net/packetbuf.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/mac/tsch/tsch-private.h"
#include "sys/testbed.h"
#include "sys/hash-map.h"
#include "node-id.h"
#include "net/master-net/master-net.h"
// #include "cfs/cfs.h"
// #include "cfs/cfs-coffee.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <stdio.h>

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "MASTER-R"
#define LOG_LEVEL LOG_LEVEL_DBG

/*Deployment node count*/
#ifndef MASTER_SCHEDULE
#if TESTBED == TESTBED_COOJA
  static uint8_t deployment_node_count = NUM_COOJA_NODES;
#elif TESTBED == TESTBED_FLOCKLAB
  static uint8_t deployment_node_count = 27;
#elif TESTBED == TESTBED_KIEL
  static uint8_t deployment_node_count = 20;
#elif TESTBED == TESTBED_DESK
  static uint8_t deployment_node_count = 5;
#endif /* TESTBED */
#else
  static uint8_t deployment_node_count = NUM_COOJA_NODES;
#endif /* MASTER_SCHEDULE */

/** Master's routing packet with "header" */
typedef struct __attribute__((packed)) master_routing_packet_t
{
  uint8_t flow_number; // use as sender in case of neighbor discovery
  uint16_t packet_number;
#if TSCH_TTL_BASED_RETRANSMISSIONS && defined(MASTER_SCHEDULE)
  uint16_t ttl_slot_number;
  uint16_t earliest_tx_slot;
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS && defined(MASTER_SCHEDULE) */
  uint8_t data[MASTER_MSG_LENGTH];
} master_routing_packet_t;

#define FILENAME "Master/Schedules/meinTest.txt"

master_tsch_schedule_t schedules[NUM_COOJA_NODES] = {{0}};
uint8_t metric_received[NUM_COOJA_NODES] = {0};

static master_routing_packet_t mrp; // masternet_routing_packet   (mrp)

static uint8_t COMMAND_END = 1;

#ifdef MASTER_SCHEDULE
static hash_table_t forward_to;                           // forward to next node, later not needed anymore //TODO: different hash_table sizes?, or size of flow!
static hash_table_t last_received_relayed_packet_of_flow; // for routing layer duplicate detection
#if TSCH_TTL_BASED_RETRANSMISSIONS
static uint8_t first_tx_slot_in_flow[MASTER_NUM_FLOWS];
static uint8_t last_tx_slot_in_flow[MASTER_NUM_FLOWS];
static uint16_t last_sent_packet_asn = 0; // to be used only by sender
#else
static uint8_t sending_slots[MAX_NUMBER_TRANSMISSIONS];
static uint8_t num_sending_slots;
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS */

static uint8_t max_transmissions[MASTER_NUM_FLOWS];
static uint8_t schedule_length;

#endif /* MASTER_SCHEDULE */

static const uint8_t minimal_routing_packet_size = sizeof(master_routing_packet_t) - MASTER_MSG_LENGTH;
static const uint8_t maximal_routing_packet_size = sizeof(master_routing_packet_t);

static uint16_t own_packet_number = 0;
static uint8_t own_receiver; // TODO: check if still needed, or if receiver_of_flow can be used TODO:: Remove since now in schedules struct
static uint8_t is_sender = 0; //TODO:: Remove since now in schedules struct
//static uint8_t beacon_slot = 0;
enum phase current_state = ST_EB;
struct tsch_neighbor *next_dest = NULL;

#ifdef MASTER_SCHEDULE
// scheduled with Master
static struct tsch_slotframe *sf[MASTER_NUM_FLOWS + 1]; // 1 sf per flow + EB-sf
static uint8_t receiver_of_flow[MASTER_NUM_FLOWS + 1];
static uint8_t sender_of_flow[MASTER_NUM_FLOWS + 1];
#else
// Neighbor Discovery for Master
static struct tsch_slotframe *sf[3]; // 2 sf for ND + EB-sf
#endif /* MASTER_SCHEDULE */

static master_packetbuf_config_t sent_packet_configuration;

static struct ctimer install_schedule_timer;
static uint8_t started = 0;
static uint8_t is_configured = 0;

static master_routing_input_callback current_callback = NULL;
static mac_callback_t current_output_callback = NULL;

// Max neighbors calculates virtual EB and Broadcast in. remove this two
uint8_t etx_links[(TSCH_QUEUE_MAX_NEIGHBOR_QUEUES - 2) * 2];
#define MAX_CHARS_PER_ETX_LINK 8
char str[MAX_CHARS_PER_ETX_LINK * TSCH_QUEUE_MAX_NEIGHBOR_QUEUES + 1]; //'\0' at the end
char test[100];
/*-------------------------- Routing configuration --------------------------*/

#if MAC_CONF_WITH_TSCH
#if TESTBED == TESTBED_COOJA && CONTIKI_TARGET_SKY
static linkaddr_t coordinator_addr = {{MASTER_TSCH_COORDINATOR, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
#elif TESTBED == TESTBED_COOJA && CONTIKI_TARGET_COOJA
static linkaddr_t coordinator_addr = {{MASTER_TSCH_COORDINATOR, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
#elif TESTBED == TESTBED_FLOCKLAB && CONTIKI_TARGET_SKY
static linkaddr_t coordinator_addr = {{MASTER_TSCH_COORDINATOR, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
#elif TESTBED == TESTBED_KIEL && CONTIKI_TARGET_ZOUL
static linkaddr_t coordinator_addr = {{0x00, 0x12, 0x4B, 0x00, 0x00, 0x00, 0x00, MASTER_TSCH_COORDINATOR}};
#elif TESTBED == TESTBED_KIEL && CONTIKI_TARGET_SKY
static linkaddr_t coordinator_addr = {{MASTER_TSCH_COORDINATOR, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
#elif TESTBED == TESTBED_DESK && CONTIKI_TARGET_ZOUL
static linkaddr_t coordinator_addr = {{0x00, 0x12, 0x4B, 0x00, 0x00, 0x00, 0x00, MASTER_TSCH_COORDINATOR}};
#else
static linkaddr_t coordinator_addr = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, MASTER_TSCH_COORDINATOR}};
#endif
#endif /* MAC_CONF_WITH_TSCH */

/*Destination*/
#ifdef MASTER_SCHEDULE
static uint8_t destinations[NUM_COOJA_NODES];
#if (TESTBED == TESTBED_KIEL || TESTBED == TESTBED_DESK) && CONTIKI_TARGET_ZOUL
static linkaddr_t destination = {{0x00, 0x12, 0x4B, 0x00, 0x00, 0x00, 0x00, 0x00}};
#else
static linkaddr_t destination = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
#endif

#endif /* MASTER_SCHEDULE */

uint8_t
get_destination_index(uint8_t id)
{
#if TESTBED == TESTBED_COOJA
  return id - 1;
#elif TESTBED == TESTBED_FLOCKLAB
  if (id < 5)
  {
    return id - 1;
  }
  else if (id < 9)
  {
    return id - 2;
  }
  else if (id < 12)
  {
    return id - 3;
  }
  else if (id < 21)
  {
    return node_id - 4;
  }
  else if (id < 29)
  {
    return id - 5;
  }
  else
  {
    return id - 7;
  }
#elif TESTBED == TESTBED_KIEL
  if (id < 11)
  {
    return id - 1;
  }
  else
  {
    return id - 2;
  }
#elif TESTBED == TESTBED_DESK
  return id - 1;
#endif
}

void init_deployment()
{
#if TESTBED == TESTBED_COOJA && defined(MASTER_SCHEDULE)
  uint8_t cooja_node;
  for (cooja_node = 0; cooja_node < NUM_COOJA_NODES; ++cooja_node)
  {
    destinations[cooja_node] = cooja_node + 1;
  }
#endif
}
/*---------------------------------------------------------------------------*/
#ifdef MASTER_SCHEDULE
static void set_destination_link_addr(uint8_t destination_node_id)
{
  destination.u8[NODE_ID_INDEX] = destination_node_id;
}
#endif /* MASTER_SCHEDULE */
/*---------------------------------------------------------------------------*/
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
//Prepare the master_packetbuf_config_t for MASTER-NET
void prepare_forward_config(uint8_t etx_link, uint8_t command)
{
  if (etx_link % 10 > 0)
  {
    sent_packet_configuration.max_tx = (etx_link/ 10) + 1;
  }
  else
  {
    sent_packet_configuration.max_tx = etx_link / 10;
  }

  // Prepare packet to send metric to requester
  sent_packet_configuration.command = command;
}

//This function used only to switch the link during convergast. therefore most parameters are set
void prepare_link_for_metric_distribution(linkaddr_t* dest, uint16_t timeslot)
{
    if(linkaddr_cmp(&tsch_schedule_get_link_by_timeslot(sf[1], timeslot)->addr, dest) == 0)
    {
      tsch_schedule_remove_link_by_timeslot(sf[1], timeslot);
      tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, dest, timeslot, 0);
    }
}

//Depending on the state, send packets to time_source or a neihgbor to poll their metric
void handle_convergcast(int repeat)
{
  if(tsch_queue_get_time_source() == NULL)
  {
  LOG_ERR("TEST my time souce is: NULL\n");
  }else{
  LOG_ERR("TEST my time souce is: %d\n", tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX]);
  }

  if (current_state == ST_POLL_NEIGHBOUR)
  {
    // Prepare packet for get metrix command
    mrp.flow_number = node_id;
    mrp.packet_number = ++own_packet_number;
    mrp.data[0] = CM_GET_ETX_METRIC;
    masternet_len = minimal_routing_packet_size + sizeof(uint8_t);

    prepare_forward_config(next_dest->etx_link, CM_GET_ETX_METRIC);

    prepare_link_for_metric_distribution(&next_dest->addr, node_id - 1);

    LOG_ERR("Sending POLL request to %u with size %d\n", next_dest->addr.u8[NODE_ID_INDEX], masternet_len);
    LOG_DBG("Max transmissions just so the warning goes away %u", schedules[0].max_transmissions_len);

    NETSTACK_NETWORK.output(&next_dest->addr);
  }else
  {
    prepare_forward_config(tsch_queue_get_time_source()->etx_link, CM_ETX_METRIC);

    prepare_link_for_metric_distribution(&tsch_queue_get_time_source()->addr, node_id - 1);

    LOG_ERR("Sending ETX-Links to %u with size %d, flow_number %i, packet_num %i and retransmits = %i\n", tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX], 
                                                                                            masternet_len, mrp.flow_number, mrp.packet_number, sent_packet_configuration.max_tx);
    NETSTACK_NETWORK.output(&tsch_queue_get_time_source()->addr);
  }
}

// Get the next neighbour that has this node as his time source
int set_next_neighbour()
{
  do
  {
    next_dest = tsch_queue_next_nbr(next_dest);

    if (next_dest == NULL)
    {
      return 0;
    }
  } while (next_dest->time_source != linkaddr_node_addr.u8[NODE_ID_INDEX]);
  //LOG_ERR("Selecting Neighbor %u", next_dest->addr.u8[NODE_ID_INDEX]);
  return 1;
}

void print_metric(uint8_t *metric, uint8_t metric_owner, uint16_t len)
{
  int i;
  int string_offset = 0;
  for (i = 0; i < len; i += 2)
  {
    int node = *(metric + i);
    int first = *(metric + i + 1) / 10;
    int second = *(metric + i + 1) % 10;

    string_offset += sprintf(&str[string_offset], "%i:%i.%i", node, first, second);

    if (i + 2 < len)
    {
      string_offset += sprintf(&str[string_offset], ", ");
    }
  }

  LOG_INFO("ETX-Links - FROM %i; %s\n", metric_owner, str);
}

int calculate_etx_metric()
{
  tsch_set_eb_period(CLOCK_SECOND);
  struct tsch_neighbor *nbr = tsch_queue_first_nbr();
  int pos = 0;
  do
  {
    // Skip virtaul eb and broadcast neihbor
    if (linkaddr_cmp(&tsch_eb_address, &nbr->addr) || linkaddr_cmp(&tsch_broadcast_address, &nbr->addr))
    {
      nbr = tsch_queue_next_nbr(nbr);
      continue;
    }
    // This could fail if last_eb = first_eb, but this is unlikely if the time set for the eb phase is not extremly short
    float etx_link = 1.0 / (1.0 - ((float)nbr->missed_ebs / (nbr->last_eb - nbr->first_eb)));
    int etx_link_int = (int)(etx_link * 100);

    etx_links[pos] = (uint8_t)nbr->addr.u8[NODE_ID_INDEX];
    if (etx_link_int % 10 > 0)
    {
      etx_links[pos + 1] = (uint8_t)((etx_link_int / 10) + 1);
    }
    else
    {
      etx_links[pos + 1] = (uint8_t)(etx_link_int / 10);
    }
    nbr->etx_link = etx_links[pos + 1];
    // LOG_INFO("ETX-Links nbr %u data %u, %u, %u\n", nbr->addr.u8[NODE_ID_INDEX], nbr->missed_ebs, nbr->first_eb, nbr->last_eb);
    // LOG_INFO("ETX-Links calculate for %u, %u\n", etx_links[pos], etx_links[pos+1]);
    pos += 2;
    nbr = tsch_queue_next_nbr(nbr);
  } while (nbr != NULL);
 
  return pos; 
}

// Called when the first poll command to get this nodes metric is received
void prepare_etx_metric()
{
  //deactive tsch beacons since in a large network, they can fill our packet queue up
  tsch_eb_active = 0;
  LOG_ERR("EB deactivated\n");
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  int command = CM_ETX_METRIC;
  int len = calculate_etx_metric();
  memcpy(&(mrp.data), &command, sizeof(uint8_t));
  memcpy(&(mrp.data[1]), etx_links, len);
  masternet_len = minimal_routing_packet_size + sizeof(uint8_t) + len;
  handle_convergcast(0);
}

void poll_nbr_or_finish()
{
  if(set_next_neighbour())
  {
    LOG_ERR("Queue empty but polling neighbors are there\n");
    handle_convergcast(0);
  }else{
    //In case no queued packets and no more neighbors, mark ourself as finished and activate beacons
    current_state = ST_WAIT_FOR_SCHEDULE;
    tsch_eb_active = 1;
    LOG_ERR("Finished polling neighbors, EB activate\n");
  }
}

#endif /* TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY */
/*---------------------------------------------------------------------------*/
static void
master_install_schedule(void *ptr)
{
  LOG_TRACE("master_install_schedule \n");
  LOG_INFO("install schedule\n");
  tsch_set_eb_period(TSCH_EB_PERIOD);

#ifdef MASTER_SCHEDULE
  if (node_id == 1){
    int schedule_index = 0;
    schedule_length = 41;
    sender_of_flow[2] = 5;
    receiver_of_flow[2] = 2;
    sender_of_flow[1] = 2;
    receiver_of_flow[1] = 3;
    beacon_slot = 40;
  #if TSCH_TTL_BASED_RETRANSMISSIONS
    first_tx_slot_in_flow[1] = 3;
    last_tx_slot_in_flow[1] = 7;
    first_tx_slot_in_flow[0] = 0;
    last_tx_slot_in_flow[0] = 3;
  #endif /* TSCH_TTL_BASED_RETRANSMISSIONS */
    schedule_index = 1;
    schedules[schedule_index].own_transmission_flow = 1;
    schedules[schedule_index].is_sender = 1;
    schedules[schedule_index].own_receiver = 3;
    schedules[schedule_index].links_len = 6;
    schedules[schedule_index].links[0] = (scheduled_link_t){ .slotframe_handle= 1, .send_receive= 1, .timeslot= 0, .channel_offset= 0 };
    schedules[schedule_index].links[1] = (scheduled_link_t){ .slotframe_handle= 1, .send_receive= 1, .timeslot= 1, .channel_offset= 0 };
    schedules[schedule_index].links[2] = (scheduled_link_t){ .slotframe_handle= 1, .send_receive= 1, .timeslot= 2, .channel_offset= 0 };
    schedules[schedule_index].links[3] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 2, .timeslot= 5, .channel_offset= 0 };
    schedules[schedule_index].links[4] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 2, .timeslot= 6, .channel_offset= 0 };
    schedules[schedule_index].links[5] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 2, .timeslot= 7, .channel_offset= 0 };
    schedules[schedule_index].flow_forwards_len = 1;
    schedules[schedule_index].flow_forwards[0] = 1;
    schedules[schedule_index].flow_forwards[1] = 1;
    schedules[schedule_index].forward_to_len = 1;
    schedules[schedule_index].cha_idx_to_dest[0] = 0;
    schedules[schedule_index].cha_idx_to_dest[1] = 1;
    schedules[schedule_index].max_transmission[0] = 3;
    schedules[schedule_index].max_transmissions_len = 1;
    //TODO:: sending slots prüfen
    #if !TSCH_TTL_BASED_RETRANSMISSIONS
      sending_slots[0] = 0;
      sending_slots[1] = 1;
      sending_slots[2] = 2;
      num_sending_slots = 3;
    #endif /* !TSCH_TTL_BASED_RETRANSMISSIONS */
    schedule_index = 0;
    schedules[schedule_index].links_len = 8;
    schedules[schedule_index].links[0] = (scheduled_link_t){ .slotframe_handle= 1, .send_receive= 2, .timeslot= 0, .channel_offset= 0 };
    schedules[schedule_index].links[1] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 3, .timeslot= 5, .channel_offset= 0 };
    schedules[schedule_index].links[2] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 3, .timeslot= 6, .channel_offset= 0 };
    schedules[schedule_index].links[3] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 1, .timeslot= 7, .channel_offset= 0 };
    schedules[schedule_index].links[4] = (scheduled_link_t){ .slotframe_handle= 1, .send_receive= 3, .timeslot= 1, .channel_offset= 0 };
    schedules[schedule_index].links[5] = (scheduled_link_t){ .slotframe_handle= 1, .send_receive= 3, .timeslot= 2, .channel_offset= 0 };
    schedules[schedule_index].links[6] = (scheduled_link_t){ .slotframe_handle= 1, .send_receive= 1, .timeslot= 3, .channel_offset= 1 };
    schedules[schedule_index].links[7] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 2, .timeslot= 4, .channel_offset= 0 };
    schedules[schedule_index].flow_forwards_len = 2;
    schedules[schedule_index].flow_forwards[0] = 2;
    schedules[schedule_index].flow_forwards[1] = 2;
    schedules[schedule_index].flow_forwards[2] = 1;
    schedules[schedule_index].flow_forwards[3] = 3;
    schedules[schedule_index].forward_to_len = 3;
    schedules[schedule_index].cha_idx_to_dest[0] = 0;
    schedules[schedule_index].cha_idx_to_dest[1] = 2;
    schedules[schedule_index].cha_idx_to_dest[2] = 4;
    schedules[schedule_index].cha_idx_to_dest[3] = 3;
    schedules[schedule_index].cha_idx_to_dest[4] = 7;
    schedules[schedule_index].cha_idx_to_dest[5] = 4;
    schedules[schedule_index].max_transmission[1] = 3;
    schedules[schedule_index].max_transmission[0] = 3;
    schedules[schedule_index].max_transmissions_len = 2;
    schedule_index = 2;
    schedules[schedule_index].links_len = 3;
    schedules[schedule_index].links[0] = (scheduled_link_t){ .slotframe_handle= 1, .send_receive= 2, .timeslot= 1, .channel_offset= 0 };
    schedules[schedule_index].links[1] = (scheduled_link_t){ .slotframe_handle= 1, .send_receive= 2, .timeslot= 2, .channel_offset= 0 };
    schedules[schedule_index].links[2] = (scheduled_link_t){ .slotframe_handle= 1, .send_receive= 2, .timeslot= 3, .channel_offset= 1 };
    schedules[schedule_index].flow_forwards_len = 0;
    schedules[schedule_index].forward_to_len = 1;
    schedules[schedule_index].cha_idx_to_dest[0] = 0;
    schedules[schedule_index].cha_idx_to_dest[1] = 1;
    schedules[schedule_index].max_transmissions_len = 0;
    schedule_index = 4;
    schedules[schedule_index].own_transmission_flow = 2;
    schedules[schedule_index].is_sender = 1;
    schedules[schedule_index].own_receiver = 2;
    schedules[schedule_index].links_len = 3;
    schedules[schedule_index].links[0] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 1, .timeslot= 3, .channel_offset= 0 };
    schedules[schedule_index].links[1] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 1, .timeslot= 4, .channel_offset= 0 };
    schedules[schedule_index].links[2] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 1, .timeslot= 5, .channel_offset= 0 };
    schedules[schedule_index].flow_forwards_len = 1;
    schedules[schedule_index].flow_forwards[0] = 2;
    schedules[schedule_index].flow_forwards[1] = 4;
    schedules[schedule_index].forward_to_len = 1;
    schedules[schedule_index].cha_idx_to_dest[0] = 0;
    schedules[schedule_index].cha_idx_to_dest[1] = 4;
    schedules[schedule_index].max_transmission[1] = 3;
    schedules[schedule_index].max_transmissions_len = 1;
    #if !TSCH_TTL_BASED_RETRANSMISSIONS
      sending_slots[0] = 3;
      sending_slots[1] = 4;
      sending_slots[2] = 5;
      num_sending_slots = 3;
    #endif /* !TSCH_TTL_BASED_RETRANSMISSIONS */
    schedule_index = 3;
    schedules[schedule_index].links_len = 4;
    schedules[schedule_index].links[0] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 3, .timeslot= 4, .channel_offset= 0 };
    schedules[schedule_index].links[1] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 3, .timeslot= 5, .channel_offset= 0 };
    schedules[schedule_index].links[2] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 1, .timeslot= 6, .channel_offset= 0 };
    schedules[schedule_index].links[3] = (scheduled_link_t){ .slotframe_handle= 2, .send_receive= 2, .timeslot= 3, .channel_offset= 0 };
    schedules[schedule_index].flow_forwards_len = 1;
    schedules[schedule_index].flow_forwards[0] = 2;
    schedules[schedule_index].flow_forwards[1] = 1;
    schedules[schedule_index].forward_to_len = 2;
    schedules[schedule_index].cha_idx_to_dest[0] = 0;
    schedules[schedule_index].cha_idx_to_dest[1] = 1;
    schedules[schedule_index].cha_idx_to_dest[2] = 3;
    schedules[schedule_index].cha_idx_to_dest[3] = 5;
    schedules[schedule_index].max_transmission[1] = 3;
    schedules[schedule_index].max_transmissions_len = 1;
  }
#else
  LOG_INFO("Starting convergcast\n");
  //if (tsch_is_coordinator)
  //{
  //   int fd;
  //   int r;
  //   fd = cfs_open(FILENAME, CFS_READ | CFS_APPEND | CFS_WRITE);
  //   if(fd < 0) {
  //     LOG_ERR("READ failed to open %s\n", FILENAME);
  //   }
  //   uint8_t schedule_index = 1;
  //   // r = cfs_write(fd, &schedule_index, sizeof(schedule_index));
  //   // if(r != sizeof(schedule_index)) {
  //   //   printf("READ failed to write %d bytes to %s\n",
  //   //         (int)sizeof(schedule_index), FILENAME);
  //   //   cfs_close(fd);
  //   // }

  //   printf("READ schedule_index is %i\n", schedule_index);

  //     /* To read back the message, we need to move the file pointer to the
  //     beginning of the file. */
  //   if(cfs_seek(fd, 0, CFS_SEEK_SET) != 0) {
  //     printf("READ seek failed\n");
  //     cfs_close(fd);
  //   }
  //   schedule_index = 125;
  //   r = cfs_read(fd, &schedule_index, sizeof(schedule_index));
  //   LOG_INFO("READ r = %i with %i; supposed to read %i\n",r, schedule_index, sizeof(schedule_index));
  //   if(r != sizeof(schedule_index)) {
  //     LOG_ERR("READ failed to read %d bytes",(int)sizeof(uint8_t));
  //     cfs_close(fd);
  //   }
  //   cfs_close(fd);
  //   LOG_INFO("READ BINARY %i\n",schedule_index);
  //}
  if(tsch_is_coordinator)
  {
    int len = calculate_etx_metric();
    print_metric(etx_links, 1, len);
    metric_received[0] = 1;
    current_state = ST_POLL_NEIGHBOUR;
    next_dest = tsch_queue_first_nbr();
    handle_convergcast(0);
  }else{
    if (current_state == ST_BEGIN_GATHER_METRIC)
    {
      LOG_ERR("Starting prepare metric ");
      prepare_etx_metric();
    }
  }
#endif /* MASTER_SCHEDULE */
  started = 1;
  LOG_INFO("started\n");
  is_configured = 1;
  // if MASTER_SCHEDULE -> install schedule, else -> install ND schedule
  LOG_TRACE_RETURN("master_install_schedule \n");
}
/*---------------------------------------------------------------------------*/
void master_routing_set_input_callback(master_routing_input_callback callback)
{
  LOG_TRACE("master_routing_set_input_callback \n");
  if (started == 0)
  {
    init_master_routing();
  }
  current_callback = callback;
  LOG_TRACE_RETURN("master_routing_set_input_callback \n");
}
/*---------------------------------------------------------------------------*/
void master_routing_output_input_callback(mac_callback_t callback)
{
  LOG_TRACE("master_routing_output_input_callback \n");
  current_output_callback = callback;
  LOG_TRACE_RETURN("master_routing_output_input_callback \n");
}
/*---------------------------------------------------------------------------*/
// Callback for sent packets over TSCH.
void master_routing_output(void *data, int ret, int transmissions)
{
  LOG_INFO("master output \n");

  //int command = 0;
  //memcpy(&command, data, 1);

  packet_data_t *packet_data = (packet_data_t *)data;

  if(packet_data->command != CM_GET_ETX_METRIC && packet_data->command != CM_ETX_METRIC)
  {
    //LOG_INFO("Got command after sending %i\n", command);
    return;
  }

  // We requiere the ETX-metric, therefore try again
  if (ret != MAC_TX_OK )
  {
    LOG_ERR("Repeat request after %i transmits\n", transmissions);

    //int header_len = 0;
    //memcpy(&header_len, data + 1, 1);

    //int i;
    //int offset = 0;
    // uint8_t * testt = (uint8_t *)&mrp;
    // for (i = 0; i < sizeof(mrp); i++)
    // {
    //   offset += sprintf(&test[offset], "%i ", *(testt + i));
    // }
    // LOG_ERR("MRP contains before trasnmitting: %s\n", test);

    memset(&mrp, 0, sizeof(mrp));
    memcpy(&mrp, packetbuf_dataptr() + packet_data->hdr_len, packetbuf_datalen() - packet_data->hdr_len);

    // offset = 0;
    // for (i = 0; i < packetbuf_datalen() - header_len; i++)
    // {
    //   offset += sprintf(&test[offset], "%i ", *(testt + i));
    // }

    //TODO: Check if this works
    if(current_state == ST_POLL_NEIGHBOUR)
    {
      LOG_ERR("Current state is polling and dest is %d, with etx-metrix received? %i\n", next_dest->addr.u8[NODE_ID_INDEX], tsch_queue_get_nbr(&next_dest->addr)->etx_metric_received);
    }

    if(current_state == ST_POLL_NEIGHBOUR && tsch_queue_get_nbr(&next_dest->addr)->etx_metric_received)
    {
      LOG_ERR("Packet received from polling nbr %d, dont repeat request\n", next_dest->addr.u8[NODE_ID_INDEX]);
    }else{
      //LOG_ERR("MRP contains now: %s\n", test);
      LOG_ERR("MRP contains flownumber: %d with size %d\n", mrp.flow_number, packetbuf_datalen() - packet_data->hdr_len);
      handle_convergcast(1);
      return;
    }
  }

  // In case of coordinator, we end up here after ACK from a neighbour
  if (tsch_is_coordinator)
  {
    // Ignore callback from sent packets that are not poll requests
    if (packet_data->command != CM_GET_ETX_METRIC)
    {
      return;
    }

    // In case there are still neighbours left, poll the next, otherwise finish
    if (set_next_neighbour())
    {
      handle_convergcast(0);
    }
    else
    {
      current_state = ST_WAIT_FOR_SCHEDULE;
    }
  }
  else
  {
    // When we are not the coordinator and send our metric, we end up here after an ACK
    // get the first neighbor and poll him
    if (current_state == ST_BEGIN_GATHER_METRIC)
    {
      // Get the first neighbor that has this node as time_source
      next_dest = tsch_queue_first_nbr();
      while (next_dest->time_source != linkaddr_node_addr.u8[NODE_ID_INDEX])
      {
        next_dest = tsch_queue_next_nbr(next_dest);

        if (next_dest == NULL)
        {
          current_state = ST_WAIT_FOR_SCHEDULE;
          tsch_eb_active = 1;
          LOG_ERR("No neighbors for this node. EB activate\n");
          return;
        }
      }

      // Start Polling with first neighbor
      current_state = ST_POLL_NEIGHBOUR;
      handle_convergcast(0);
      return;
    }

    uint8_t is_empty = tsch_queue_is_empty(tsch_queue_get_time_source());

    // Once we send a Polling request, we prepare the next neighbor as target for the next round
    // and wait for the respons of current request
    if (current_state == ST_POLL_NEIGHBOUR)
    {
      //If There are packets in the queue for the time source, we need to forward packets
      if(is_empty)
      {
        poll_nbr_or_finish();
      }else{
        current_state = ST_SEND_METRIC;
        LOG_ERR("Packets in queue, change to SEND_METRIC\n");
      }
    }

    // We have packets for the time source available. set link back to time source
    if (current_state == ST_SEND_METRIC)
    {
      //In case the link to the time source already exists, dont remove and add it again
      if(is_empty)
      {
        current_state = ST_POLL_NEIGHBOUR;
        poll_nbr_or_finish();
      }else{
        prepare_link_for_metric_distribution(&tsch_queue_get_time_source()->addr, node_id - 1);
      }
    }
  }
}
/*---------------------------------------------------------------------------*/
void master_routing_input(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
  LOG_INFO("master_routing_input \n");
  // leds_on(LEDS_RED);

  if (len >= minimal_routing_packet_size && len <= maximal_routing_packet_size)
  {
    uint8_t forward_to_upper_layer = 0;
    memcpy(&mrp, data, len);
#ifndef MASTER_SCHEDULE
    int command = mrp.data[0];
    LOG_ERR("Got a packet from %u with flow_number %i with command %u\n", src->u8[NODE_ID_INDEX], mrp.flow_number, command);
    switch (command)
    {
    case CM_GET_ETX_METRIC:
      // Start the metric gathering.
      //TODO:: only send metric on the first receive. otherwise we send multiple times
      if(current_state == ST_EB)
      {
        current_state = ST_BEGIN_GATHER_METRIC;
        if (started)
        {
          LOG_ERR("Starting prepare metric by command\n");
          
          prepare_etx_metric();
        }
      }else{
        LOG_ERR("Do not send again since we already try to send\n");
      }
      break;
    case CM_ETX_METRIC:
      //In case we received the metric from out neighbor and he did not receive our ACK, he will resend. Drop the packet here
      if(mrp.flow_number == src->u8[NODE_ID_INDEX] && tsch_queue_get_nbr(src)->etx_metric_received)
      {
        LOG_ERR("Received already metric from %d = %d", mrp.flow_number, src->u8[NODE_ID_INDEX]);
        return;
      }

      tsch_queue_get_nbr(src)->etx_metric_received = 1;
      // Received metric from other nodes
      if (tsch_is_coordinator)
      {
        metric_received[ mrp.flow_number - 1] = 1;
        print_metric(&mrp.data[COMMAND_END], mrp.flow_number, len - minimal_routing_packet_size - COMMAND_END); //-3 for the mrp flow number and packet number and -command length
        int i;
        int done = 1;
        for(i = 0; i < deployment_node_count; i++)
        {
          if(metric_received[i] != 1)
          {
            done = 0;
          }
        }
        if(done)
        {
          LOG_ERR("ETX-Links finished!");
        }
      }

      else
      {
        LOG_ERR("Deactivate EB\n");
        tsch_eb_active = 0;
        
        masternet_len = len;

        // Response of the Polling request, forward the metric to own time source
        if(current_state == ST_WAIT_FOR_SCHEDULE && tsch_queue_is_empty(tsch_queue_get_time_source()))
        {        
          LOG_ERR("Polling finished but packet arrived. handle convergast!\n");
          handle_convergcast(0);
        }else{
          LOG_ERR("Add packet to time source queue\n");
          prepare_forward_config(tsch_queue_get_time_source()->etx_link, CM_ETX_METRIC);
          NETSTACK_NETWORK.output(&tsch_queue_get_time_source()->addr);
        }
      }
      break;
    default:
      break;
    }
#else // normal operation
    uint16_t received_asn = packetbuf_attr(PACKETBUF_ATTR_RECEIVED_ASN);

    // this node is receiver:
    if (node_id == receiver_of_flow[mrp.flow_number])
    {
      if (TSCH_SLOTNUM_LT((uint16_t)hash_map_lookup(&last_received_relayed_packet_of_flow, mrp.flow_number), mrp.packet_number))
      {                                                                                             // if old known one < new one
        hash_map_insert(&last_received_relayed_packet_of_flow, mrp.flow_number, mrp.packet_number); // update last received packet number
        LOG_INFO("received %u at ASN %u from flow %u\n", mrp.packet_number, received_asn, mrp.flow_number);
        forward_to_upper_layer = 1;
      }
      else
      {
        LOG_INFO("received %u at ASN %u duplicate from flow %u\n", mrp.packet_number, received_asn, mrp.flow_number);
      }
    }
    else
    { // forward
      uint8_t next_receiver = hash_map_lookup(&forward_to, mrp.flow_number);
      if (next_receiver != 0)
      {
        if (TSCH_SLOTNUM_LT((uint16_t)hash_map_lookup(&last_received_relayed_packet_of_flow, mrp.flow_number), mrp.packet_number))
        {                                                                                             // if old known one < new one
          hash_map_insert(&last_received_relayed_packet_of_flow, mrp.flow_number, mrp.packet_number); // update last received packet number
          set_destination_link_addr(next_receiver);
#if TSCH_FLOW_BASED_QUEUES
          sent_packet_configuration.flow_number = mrp.flow_number;
#endif /* TSCH_FLOW_BASED_QUEUES */
#if TSCH_TTL_BASED_RETRANSMISSIONS
          if (TSCH_SLOTNUM_LT((uint16_t)tsch_current_asn.ls4b, mrp.ttl_slot_number + 1))
          { // send only if time left for sending - we might already be in the last slot!
            // packetbuf set TTL
            sent_packet_configuration.ttl_slot_number = mrp.ttl_slot_number;
            sent_packet_configuration.earliest_tx_slot = mrp.earliest_tx_slot;
            // set max_transmissions
            sent_packet_configuration.max_tx = (uint16_t)TSCH_SLOTNUM_DIFF16(mrp.ttl_slot_number, (uint16_t)(tsch_current_asn.ls4b - 1)); //(uint16_t) (0xFFFF + 1 + nullnet_routing_packet.ttl_slot_number - (uint16_t) tsch_current_asn.ls4b); //include current asn
            masternet_len = len;                                                                                                          // send same length as received
            NETSTACK_NETWORK.output(&destination);
            LOG_INFO("relay %u at ASN %u from flow %u\n", mrp.packet_number, received_asn, mrp.flow_number);
          }
          else
          {
            LOG_INFO("relay %u at ASN %u from flow %u\n", mrp.packet_number, received_asn, mrp.flow_number);
            LOG_INFO("packet not enqueueing: next ASN %u, received ASN %u, TTL ASN %u\n", (uint16_t)tsch_current_asn.ls4b, received_asn, mrp.ttl_slot_number);
          }
#else
          sent_packet_configuration.max_tx = max_transmissions[mrp.flow_number - 1];
          masternet_len = len; // send same length as received
          NETSTACK_NETWORK.output(&destination);
          LOG_INFO("relay %u at ASN %u from flow %u\n", mrp.packet_number, received_asn, mrp.flow_number);
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS */
        }
        else
        {
          LOG_INFO("received %u duplicate from flow %u at ASN %u for relay\n", mrp.packet_number, mrp.flow_number, received_asn);
        }
      }
      else
      {
        LOG_INFO("No routing info for flow %u at node %u\n", mrp.flow_number, node_id);
      }
    }
#endif /* !MASTER_SCHEDULE */

    if (forward_to_upper_layer && len > minimal_routing_packet_size)
    {
      // TODO: exchange source by flow-source
      // upper layer input callback (&mrp.data, len-minimal_routing_packet_size)
#ifdef MASTER_SCHEDULE
      current_callback((void *)&mrp.data, len - minimal_routing_packet_size, sender_of_flow[mrp.flow_number], receiver_of_flow[mrp.flow_number]);
#else
      current_callback((void *)&mrp.data[COMMAND_END], len - minimal_routing_packet_size - COMMAND_END, src->u8[NODE_ID_INDEX], dest->u8[NODE_ID_INDEX]);
#endif /* MASTER_SCHEDULE */
    }
  }
  // leds_off(LEDS_RED);
  LOG_TRACE_RETURN("master_routing_input \n");
}
/*---------------------------------------------------------------------------*/
master_packetbuf_config_t
master_routing_sent_configuration()
{
  LOG_TRACE("master_routing_sent_configuration \n");
  LOG_TRACE_RETURN("master_routing_sent_configuration \n");
  return sent_packet_configuration;
}
/*---------------------------------------------------------------------------*/
int node_is_sender()
{
  return is_sender;
}
/*---------------------------------------------------------------------------*/
int get_node_receiver()
{
  return own_receiver;
}
/*---------------------------------------------------------------------------*/
int master_routing_configured()
{
  return is_configured;
}
/*---------------------------------------------------------------------------*/
int master_routing_send(const void *data, uint16_t datalen)
{
  LOG_TRACE("master_routing_send \n");
#ifndef MASTER_SCHEDULE
  LOG_WARN("No schedule configured yet!\n");
  return 0;
#else
  int own_transmission_flow = schedules[linkaddr_node_addr.u8[NODE_ID_INDEX]].own_transmission_flow;
  if (own_transmission_flow!= 0)
  {
    mrp.flow_number = own_transmission_flow;
    mrp.packet_number = ++own_packet_number;
    memcpy(&(mrp.data), data, datalen);

    // get current / next active ASN (tsch_current_asn)
    // get corresponding slotframe slot number (TSCH_ASN_MOD(tsch_current_asn, sf->size))
    struct tsch_slotframe *sf;
    uint16_t sf_size;
    uint16_t current_sf_slot;
    sf = tsch_schedule_get_slotframe_by_handle(own_transmission_flow);
    sf_size = ((uint16_t)((sf->size).val));
    current_sf_slot = TSCH_ASN_MOD(tsch_current_asn, sf->size);

#if TSCH_TTL_BASED_RETRANSMISSIONS
    mrp.ttl_slot_number = (uint16_t)tsch_current_asn.ls4b + sf_size - current_sf_slot + (uint16_t)last_tx_slot_in_flow[own_transmission_flow - 1];
    mrp.earliest_tx_slot = (uint16_t)tsch_current_asn.ls4b + sf_size - current_sf_slot + (uint16_t)first_tx_slot_in_flow[own_transmission_flow - 1]; // earliest slot in next slotframe
    if (TSCH_SLOTNUM_LT(mrp.earliest_tx_slot, (last_sent_packet_asn + schedule_length)))
    { // avoid duplicates in earliest ASN
      --own_packet_number;
      LOG_INFO("Too high sending frequency, try again later\n");
      LOG_TRACE_RETURN("master_routing_send \n");
      return 0;
    }
    last_sent_packet_asn = mrp.earliest_tx_slot;
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS */

    uint8_t next_receiver = hash_map_lookup(&forward_to, mrp.flow_number);
    if (next_receiver != 0)
    {
      set_destination_link_addr(next_receiver);

#if TSCH_FLOW_BASED_QUEUES
      sent_packet_configuration.flow_number = mrp.flow_number;
#endif /* TSCH_FLOW_BASED_QUEUES */

#if TSCH_TTL_BASED_RETRANSMISSIONS
      // packetbuf set TTL
      sent_packet_configuration.ttl_slot_number = mrp.ttl_slot_number;
      sent_packet_configuration.earliest_tx_slot = mrp.earliest_tx_slot;
      // set max_transmissions
      sent_packet_configuration.max_tx = (uint16_t)TSCH_SLOTNUM_DIFF16(mrp.ttl_slot_number, (uint16_t)(tsch_current_asn.ls4b - 1)); //(uint16_t) (0xFFFF + 1 + nullnet_routing_packet.ttl_slot_number - nullnet_routing_packet.earliest_tx_slot); //include earliest slot!
#else
      sent_packet_configuration.max_tx = max_transmissions[sent_packet_configuration.flow_number - 1];
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS */

      LOG_INFO("expected max tx: %u\n", sent_packet_configuration.max_tx);

      masternet_len = minimal_routing_packet_size + datalen;
      NETSTACK_NETWORK.output(&destination);

      // print sent data
#if TSCH_TTL_BASED_RETRANSMISSIONS
      LOG_INFO("sent %u at ASN %u till ASN %u from sender %u\n", mrp.packet_number, mrp.earliest_tx_slot, mrp.ttl_slot_number, node_id);
#else
      // calculate sending slot based on
      uint8_t tx_slot_idx;
      uint16_t earliest_tx_slot_asn;
      uint16_t earliest_slot_number_offset = 0xFFFF;
      uint16_t local_slot_number_offset;
      for (tx_slot_idx = 0; tx_slot_idx < num_sending_slots; ++tx_slot_idx)
      {
        if (sending_slots[tx_slot_idx] >= current_sf_slot)
        {
          local_slot_number_offset = sending_slots[tx_slot_idx] - current_sf_slot;
        }
        else
        {
          local_slot_number_offset = sending_slots[tx_slot_idx] + sf_size - current_sf_slot;
        }
        if (local_slot_number_offset < earliest_slot_number_offset)
        {
          earliest_slot_number_offset = local_slot_number_offset;
        }
      }
      earliest_tx_slot_asn = (uint16_t)tsch_current_asn.ls4b + earliest_slot_number_offset;

      LOG_INFO("sent %u at ASN %u from sender %u\n", mrp.packet_number, earliest_tx_slot_asn, node_id);
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS */
    }
    else
    {
      LOG_INFO("No routing info for flow %u\n", mrp.flow_number);
    }
    LOG_TRACE_RETURN("master_routing_send \n");
    return 1;
  }
  else
  {
    LOG_INFO("Node %u is no sender!\n", node_id);
    LOG_TRACE_RETURN("master_routing_send \n");
    return 0;
  }
#endif /* !MASTER_SCHEDULE */
}
/*---------------------------------------------------------------------------*/
int master_routing_sendto(const void *data, uint16_t datalen, uint8_t receiver)
{
  LOG_TRACE("master_routing_sendto \n");
  // LOG_INFO("send length %u to %u", datalen, receiver);
  if (receiver == own_receiver)
  {
    LOG_TRACE_RETURN("master_routing_sendto \n");
    return master_routing_send(data, datalen);
  }
  else
  {
    LOG_INFO("No routing inffo for receiver %u\n", receiver);
    LOG_TRACE_RETURN("master_routing_sendto \n");
    return 0;
  }
}
/*---------------------------------------------------------------------------*/
void init_master_routing(void)
{
  LOG_TRACE("init_master_routing \n");
  LOG_ERR("CHECK end and delimiter are %d %d", (uint8_t)END, (uint8_t)BUF_END_DELIMITER);
#if NETSTACK_CONF_WITH_MASTER_NET
  if (started == 0)
  {
    /* Initialize Testbed/Deployment */
    init_deployment();

    /* configure transmit power */
#if CONTIKI_TARGET_ZOUL && defined(MASTER_CONF_CC2538_TX_POWER)
    NETSTACK_RADIO.set_value(RADIO_PARAM_TXPOWER, MASTER_CONF_CC2538_TX_POWER);
#endif /* CONTIKI_TARGET_ZOUL && defined(MASTER_CONF_CC2538_TX_POWER) */

#if MAC_CONF_WITH_TSCH
    int is_coordinator = linkaddr_cmp(&coordinator_addr, &linkaddr_node_addr);
    tsch_set_coordinator(is_coordinator);
    // The Enhanced beacon timer
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
    tsch_set_eb_period((CLOCK_SECOND * ((TSCH_DEFAULT_TS_TIMESLOT_LENGTH / 1000) * deployment_node_count)) / 1000);
    LOG_INFO("Generate beacon every %i \n", ((TSCH_DEFAULT_TS_TIMESLOT_LENGTH / 1000) * deployment_node_count));
    if (is_coordinator)
      tsch_set_rank(0);
#else
    tsch_set_eb_period(CLOCK_SECOND/4);
#endif /* TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY */
#endif /* MAC_CONF_WITH_TSCH */

    /* Initialize MasterNet */
    masternet_buf = (uint8_t *)&mrp;
    masternet_len = sizeof(master_routing_packet_t);
    current_callback = NULL;

    /* Register MasterNet input/config callback */
    masternet_set_input_callback(master_routing_input); // TODOLIV
    masternet_set_output_callback(master_routing_output);
    masternet_set_config_callback(master_routing_sent_configuration);

    tsch_schedule_remove_all_slotframes();
  #if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
    sf[0] = tsch_schedule_add_slotframe(0, 1);                      // Listen on this every frame where the nodes doesnt send
    sf[1] = tsch_schedule_add_slotframe(1, deployment_node_count);  // send in this frame every "node_count"
    tsch_schedule_add_link(sf[0], LINK_OPTION_RX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, 0, 0);
    tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, node_id - 1, 0);

    /* wait for end of TSCH initialization phase, timed with MASTER_INIT_PERIOD */
    LOG_INFO("Time to run before convergcast = %i", ((TSCH_DEFAULT_TS_TIMESLOT_LENGTH / 1000) * deployment_node_count * TSCH_BEACON_AMOUNT) / 1000);
    ctimer_set(&install_schedule_timer, (CLOCK_SECOND * (TSCH_DEFAULT_TS_TIMESLOT_LENGTH / 1000) * deployment_node_count * TSCH_BEACON_AMOUNT) / 1000, master_install_schedule, NULL);
  #else
    sf[0] = tsch_schedule_add_slotframe(0, 1);
    tsch_schedule_add_link(sf[0], LINK_OPTION_TX | LINK_OPTION_RX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, 0, 0);

    /* wait for end of TSCH initialization phase, timed with MASTER_INIT_PERIOD */
    ctimer_set(&install_schedule_timer, MASTER_INIT_PERIOD, master_install_schedule, NULL);
  #endif /* TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY */
  }
#else
  LOG_ERR("can't init master-routing: master-net not configured\n");
#endif /* NETSTACK_CONF_WITH_MASTER_NET */
  LOG_TRACE_RETURN("init_master_routing \n");
}
/*---------------------------------------------------------------------------*/
/** @} */
