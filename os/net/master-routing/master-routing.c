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
#include "net/mac/tsch/tsch-slot-operation.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/mac/tsch/tsch-private.h"
#include "sys/testbed.h"
#include "node-id.h"
#include "net/master-net/master-net.h"
#include "master-schedule.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <stdio.h>

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "MASTER-R"
#define LOG_LEVEL LOG_LEVEL_DBG

#define WRITE32(buf, val) \
  do { ((uint8_t *)(buf))[0] = (val) & 0xff; \
       ((uint8_t *)(buf))[1] = ((val) >> 8) & 0xff; \
       ((uint8_t *)(buf))[2] = ((val) >> 16) & 0xff; \
       ((uint8_t *)(buf))[3] = ((val) >> 24) & 0xff;} while(0);

#define READ32(buf, var) \
  (var) = ((uint8_t *)(buf))[0] | ((uint8_t *)(buf))[1] << 8 | ((uint8_t *)(buf))[2] << 16 | ((uint8_t *)(buf))[3] << 24

//Calculate the worst case package count = (Nodes * Schedule_size_per_node + universall config) / payload_size  
//In case of Kiel, we miss node 11 and have node 21 instead -> expand array by 1 node     
#if TESTBED == TESTBED_KIEL
#define MAX_PACKETS_PER_SCHEDULE (((NUM_COOJA_NODES + 1) * (4*TSCH_SCHEDULE_MAX_LINKS + 2*MASTER_NUM_FLOWS + 3) + 5*MASTER_NUM_FLOWS + 7) / MASTER_MSG_LENGTH)
#else 
#define MAX_PACKETS_PER_SCHEDULE ((NUM_COOJA_NODES * (4*TSCH_SCHEDULE_MAX_LINKS + 2*MASTER_NUM_FLOWS + 3) + 5*MASTER_NUM_FLOWS + 7) / MASTER_MSG_LENGTH)
#endif

//The bit-array to mark received packets
#if MAX_PACKETS_PER_SCHEDULE % 32 != 0
  uint32_t received_packets_as_bit_array[(MAX_PACKETS_PER_SCHEDULE / 32) + 1];
#else
  uint32_t received_packets_as_bit_array[MAX_PACKETS_PER_SCHEDULE / 32];
#endif

//Process for distributor node after the schedule is received over serial input.
//Once we are associated to the network again, a poll for an event will be sent
PROCESS(distributor_callback, "Master distributor node callback");

/*Deployment node count*/
#if TESTBED == TESTBED_COOJA
  static const uint8_t deployment_node_count = NUM_COOJA_NODES;

  //The schedule will be transfered between nodes and used to switch to a new schedule
  master_tsch_schedule_t schedule = {0};
  //Keep track of the received etx-metrics using this bit array
#if MAX_PACKETS_PER_SCHEDULE % 32 != 0
  uint32_t metric_received[(MAX_PACKETS_PER_SCHEDULE / 32) + 1];
#else
  uint32_t metric_received[MAX_PACKETS_PER_SCHEDULE / 32];
#endif
 static const uint8_t last_node_id = NUM_COOJA_NODES;
#elif TESTBED == TESTBED_FLOCKLAB
  static const uint8_t deployment_node_count = 27;
  master_tsch_schedule_t schedule = {0};
  uint32_t metric_received[27] = {0};
#elif TESTBED == TESTBED_KIEL
  static const uint8_t deployment_node_count = 20;
  master_tsch_schedule_t schedule = {0};
#if MAX_PACKETS_PER_SCHEDULE % 32 != 0
  uint32_t metric_received[MAX_PACKETS_PER_SCHEDULE / 32]; 
#else
  uint32_t metric_received[(MAX_PACKETS_PER_SCHEDULE / 32) + 1]; 
#endif
  static const uint8_t last_node_id = 21;
#elif TESTBED == TESTBED_DESK
  static const uint8_t deployment_node_count = 5;
  uint32_t metric_received[5] = {0};
#endif /* TESTBED */

/*Destination*/
#if (TESTBED == TESTBED_KIEL || TESTBED == TESTBED_DESK) && CONTIKI_TARGET_ZOUL
static linkaddr_t destination = {{0x00, 0x12, 0x4B, 0x00, 0x00, 0x00, 0x00, 0x00}};
#else
static linkaddr_t destination = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
#endif

master_tsch_schedule_universall_config_t schedule_config = {0};

static master_routing_packet_t mrp; // masternet_routing_packet   (mrp)

//The index for the end of the command
static uint8_t COMMAND_END = 1;

//This variable is used set once a boradcast for a missing metric is receiv
//In order to ignore multiple broadcasts from different nodes, ignore broadcasts for a few seconds usig a timer
uint8_t ingore_missing_metric_requests = 0;
static struct ctimer ignore_metric_timer;

#if TSCH_TTL_BASED_RETRANSMISSIONS
static uint16_t last_sent_packet_asn = 0; // to be used only by sender
#else
static uint8_t sending_slots[MAX_NUMBER_TRANSMISSIONS];
static uint8_t num_sending_slots;
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS */

static const uint8_t minimal_routing_packet_size = sizeof(master_routing_packet_t) - MASTER_MSG_LENGTH;
static const uint8_t maximal_routing_packet_size = sizeof(master_routing_packet_t);

static uint16_t own_packet_number = 0;
static uint8_t schedule_packet_number = 1;

//Represent the current state of the node
enum phase current_state = ST_EB;

//This variabled is used for convergcast, in order to know if neighbors exists that need to be polled
struct tsch_neighbor *next_dest = NULL;

// scheduled with Master
static struct tsch_slotframe *sf[MASTER_NUM_FLOWS + 1]; // 1 sf per flow + EB-sf

//The configuration that Master-net will evaluate
static master_packetbuf_config_t sent_packet_configuration;

//Callbacks for the application layer once an input packet is received
static master_routing_input_callback current_callback = NULL;

//Time to start the installation of the schedule once received
static struct ctimer install_schedule_timer;

//Timer to send a new request for a missing packet once time out occurs
static struct ctimer missing_response_to_request_timer;

//Timer for the CPAN to ask the network when a metric is missing
static struct ctimer missing_metric_timer;
//Stop printing metrics once all are complete
uint8_t metric_complete = 0;

//Array used to calculate and save the metric
uint8_t etx_links[3 * TSCH_QUEUE_MAX_NEIGHBOR_QUEUES - 2];

//Array used to print the etx-metric for MASTER
#define MAX_CHARS_PER_ETX_LINK 12
char str[MAX_CHARS_PER_ETX_LINK * TSCH_QUEUE_MAX_NEIGHBOR_QUEUES + 1]; //'\0' at the end
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

static double get_percent_of_second(enum tsch_timeslot_timing_elements timing_element)
{
  return (tsch_timing[timing_element] * 1.0) / RTIMER_SECOND;
}

uint8_t get_tx_slot(uint8_t id)
{
#if TESTBED == TESTBED_COOJA
  return id - 1;
# elif TESTBED == TESTBED_KIEL
  if (id < 11){
    return id - 1;
  } else {
    return id - 2;
  }
#endif
}

/* The callback after the distributor nodes received a schedule and left the network. Wait for the signal from TSCH once associated to a network*/
PROCESS_THREAD(distributor_callback, ev, data)
{
  PROCESS_BEGIN();
  while(1) {
    PROCESS_YIELD();
    if(ev == tsch_associated_to_network) {
      LOG_DBG("Associated to network again\n");
      tsch_set_eb_period(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * 2 * CLOCK_SECOND);
      master_schedule_loaded_callback();
    }
  }
  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
static void set_destination_link_addr(uint8_t destination_node_id)
{
  destination.u8[NODE_ID_INDEX] = destination_node_id;
}

/* Callback after a timeout to receive missing metric requests again */
static void activate_missing_metric_requests()
{
  ingore_missing_metric_requests = 0;
}

/* Check if all nodes received the schedule in the network */
int check_received_schedules_by_nodes()
{
  int i;
  int nodes_to_count = deployment_node_count;
  uint8_t nodes_have_schedule = 0;

#if TESTBED == TESTBED_KIEL
  nodes_to_count++;
#endif

  for(i = 0; i < nodes_to_count; i++)
  {
    if(isBitSet1Byte(schedule_received, i))
    {
      nodes_have_schedule++;
    }
  }

  if(nodes_have_schedule == deployment_node_count)
  {
    tsch_reset_schedule_received_callback();
    return 1;
  }else{
    return 0;
  }
}

#if TSCH_TTL_BASED_RETRANSMISSIONS
/* In case of TTL, set a ttl slot number and the earliest tx slot 
 * during neighbor discovery and schedule distribution
 */
void set_ttl_retransmissions()
{
  uint16_t sf_size;
  sf_size = ((uint16_t)((sf[1]->size).val));
  sent_packet_configuration.ttl_slot_number = (uint16_t)tsch_current_asn.ls4b + 10*sf_size;
  sent_packet_configuration.earliest_tx_slot = (uint16_t)tsch_current_asn.ls4b; 
}
#endif //TSCH_TTL_BASED_RETRANSMISSIONS

/* Once the schedule is distributed, reset the flag that was used while the etx-metric was gathered */
void reset_nbr_metric_received()
{
  struct tsch_neighbor *n = tsch_queue_first_nbr();

  while(n != NULL)
  {
    n->etx_metric_received = 0;
    n = tsch_queue_next_nbr(n);
  }
}

/* Every noded will send beacons on the timeslot that is their node id - 1 */
uint8_t get_beacon_slot()
{
  return get_tx_slot(node_id);
}

/* Prepare the master_packetbuf_config_t for MASTER-NET */
void setup_packet_configuration(uint16_t etx_link, uint8_t command, uint16_t packet_number, uint8_t overhearing_active)
{
  if (etx_link % 1000 > 0)
  {
    sent_packet_configuration.max_tx = (etx_link / 1000) + 1;
  }
  else
  {
    sent_packet_configuration.max_tx = etx_link / 1000;
  }

  // Prepare packet to send metric to requestera
  sent_packet_configuration.command = command;
  sent_packet_configuration.packet_nbr = packet_number;
  sent_packet_configuration.overhearing_active = overhearing_active;
# if TSCH_FLOW_BASED_QUEUES
  sent_packet_configuration.flow_number = node_id;
# endif /* TSCH_FLOW_BASED_QUEUES */
# if TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES
  sent_packet_configuration.send_to_nbr = 1;
# endif /* TSCH_FLOW_BASED_QUEUES */
#if TSCH_TTL_BASED_RETRANSMISSIONS
  set_ttl_retransmissions();
#endif
}

uint8_t fill_schedule_packet_from_flash(int bytes_in_packet, int packet_number)
{
  uint8_t schedule_payload_size = MASTER_MSG_LENGTH - bytes_in_packet;
  int flash_offset = packet_number * schedule_payload_size - schedule_payload_size;

  LOG_DBG("--- Fill Packet %i offset %i -------\n", packet_number, flash_offset);

  uint8_t bytes_to_write = MASTER_MSG_LENGTH - bytes_in_packet;
  if(bytes_to_write > bytes_in_flash - flash_offset)
  {
    bytes_to_write = bytes_in_flash - flash_offset;
  }
  uint8_t bytes_from_flash = read_flash(&mrp.data[bytes_in_packet], flash_offset, bytes_to_write);

  if(bytes_from_flash == 255)
  {
    LOG_ERR("Could not read from flash.");
    return 2;
  }

  bytes_in_packet += bytes_from_flash;

  masternet_len = bytes_in_packet + minimal_routing_packet_size;

  return (flash_offset + bytes_from_flash) == bytes_in_flash;
}

uint8_t unpack_schedule_to_flash(int packet_len)
{
  uint8_t packet_number = mrp.data[1];
  uint8_t schedule_payload_size = MASTER_MSG_LENGTH - 2;
  int offset = packet_number * schedule_payload_size - schedule_payload_size;

  LOG_DBG("--- Read Packet %i from %i for %i bytes-------\n", packet_number, offset, packet_len - 2);

  return write_to_flash_offset(&mrp.data[2], offset, packet_len - 2);
}

/* The callback from master-schedule, once a full schedule is written to flash memory */
void master_schedule_loaded_callback()
{
  LOG_INFO("Schedule loaded. Start Distribution\n");

  //As the CPAN, the schedule is already complete after we arrive at this callback.
  //Initialize distribution as CPAN
  handle_state_change(ST_SCHEDULE_DIST);

  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  mrp.data[0] = CM_SCHEDULE;
  mrp.data[1] = schedule_packet_number;
  setBit(received_packets_as_bit_array, schedule_packet_number);

  fill_schedule_packet_from_flash(2, schedule_packet_number);

  setup_packet_configuration(10, CM_SCHEDULE, mrp.packet_number, 0);

  NETSTACK_NETWORK.output(&tsch_broadcast_address);
}

/* Install the schedule for the network */
void install_schedule(){

  //  Only remove the slotframe where we listen on every slot, if all nodes have the schedule
  if(!check_received_schedules_by_nodes())
  {
    return;
  }

  //Ignore command that might arrive during installation of the schedule
  handle_state_change(ST_IGNORE_COMMANDS);

  //TODO:: remove at the end
  if(tsch_queue_get_time_source() != NULL)
  {
    printf("Install schedule at asn %d. My time source is %d\n", (int)tsch_current_asn.ls4b, tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX]);
  }else{
    printf("Install schedule at asn %d.\n", (int)tsch_current_asn.ls4b);
  }

  //Load the general config and the schedule
  if(!read_from_flash_by_id(&schedule_config, &schedule, node_id - 1))
  {
    schedule.links_len = 0;
    schedule.own_transmission_flow = 0;
    schedule.own_receiver = 0;
    LOG_DBG("No schedule for me!\n");
  }

  sf[0] = tsch_schedule_get_slotframe_by_handle(0);
  if (sf[0]){
    tsch_schedule_remove_slotframe(sf[0]);
  }
  sf[0] = tsch_schedule_add_slotframe(0, MASTER_EBSF_PERIOD);
  tsch_schedule_add_link(sf[0], LINK_OPTION_TX | LINK_OPTION_RX, LINK_TYPE_ADVERTISING_ONLY, &tsch_broadcast_address, 0, 0);

  //Always remove the old schedule where a node sends at node_id-1 their packets
  int i;
  for(i=1; i <= schedule_config.slot_frames; i++)
  {
    sf[i] = tsch_schedule_get_slotframe_by_handle(i);
    if (sf[i]){
      tsch_schedule_remove_slotframe(sf[i]);
    }
    sf[i] = tsch_schedule_add_slotframe(i, schedule_config.schedule_length);
  }  

  tsch_queue_reset();

  uint8_t link_idx;

  for (link_idx = 0; link_idx < schedule.links_len; ++link_idx){
    struct tsch_slotframe *sf = tsch_schedule_get_slotframe_by_handle(schedule.links[link_idx].slotframe_handle);

    if(schedule.links[link_idx].send_receive == LINK_OPTION_RX)
    {
      destination.u8[NODE_ID_INDEX] = 0; 
    }else{
      destination.u8[NODE_ID_INDEX] = get_forward_dest_by_slotframe(&schedule, link_idx);
    }

    tsch_schedule_add_link(sf, schedule.links[link_idx].send_receive, LINK_TYPE_NORMAL, &destination, schedule.links[link_idx].timeslot, schedule.links[link_idx].channel_offset);
  }

  //Make sure to always have at least 1 more link available than the node with the most links in the schedule requieres. E.G. with 60 links at node 3, make 61 links as size
  tsch_schedule_add_link(sf[1], LINK_OPTION_TX | LINK_OPTION_RX, LINK_TYPE_ADVERTISING_ONLY, &tsch_broadcast_address, schedule_config.schedule_length - 1, 0);
  
  //Too large schedules result in watchdog timeout!!!
  //tsch_schedule_print();

  reset_nbr_metric_received();

  tsch_set_eb_period(TSCH_EB_PERIOD);

  handle_state_change(ST_SCHEDULE_INSTALLED);

  LOG_INFO("Schedule installed!\n");
}

/* Change the link if required to a destination address at a timeslot */
void change_link(int link_type, const linkaddr_t* dest, uint16_t timeslot)
{
  if(linkaddr_cmp(&tsch_schedule_get_link_by_timeslot(sf[1], timeslot)->addr, dest) == 0)
  {
    tsch_schedule_add_link(sf[1], LINK_OPTION_TX, link_type, dest, timeslot, 0);
  }
}

/* Send a poll request to a neighbor to receive the etx-metric from this node and his neighbors */
void convergcast_poll_neighbor(enum commands command, struct tsch_neighbor * dest)
{
  // Prepare packet for get metric command
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  mrp.data[0] = command;
  masternet_len = minimal_routing_packet_size + sizeof(uint8_t);

  setup_packet_configuration(dest->etx_link, command, mrp.packet_number, 0);

  change_link(LINK_TYPE_ADVERTISING, &dest->addr, get_beacon_slot());

  //LOG_INFO("Sending POLL request to %u with size %d\n", dest->addr.u8[NODE_ID_INDEX], masternet_len);
  NETSTACK_NETWORK.output(&dest->addr);
}

/* Forward the nodes own and other received etx-metrics to the time source */
void convergcast_forward_to_timesource()
{
  setup_packet_configuration(tsch_queue_get_time_source()->etx_link, CM_ETX_METRIC_SEND, mrp.packet_number, 0);

  change_link(LINK_TYPE_ADVERTISING, &tsch_queue_get_time_source()->addr, get_beacon_slot());

  //LOG_INFO("Sending ETX-Links to %u with size %d, flow_number %i, packet_num %i and retransmits = %i\n", tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX],  masternet_len, mrp.flow_number, mrp.packet_number, sent_packet_configuration.max_tx);
  NETSTACK_NETWORK.output(&tsch_queue_get_time_source()->addr);
}

/* Depending on the state, send packets to time_source or poll a neighbor for their metric */
void handle_convergcast()
{
  if (current_state == ST_POLL_NEIGHBOUR)
  {
    convergcast_poll_neighbor(CM_ETX_METRIC_GET, next_dest);
  }else
  {
    convergcast_forward_to_timesource();
  }
}

/* Log the received etx-metric */
void print_metric(uint8_t *metric, uint8_t metric_owner, uint16_t len)
{
  int i;
  int string_offset = 0;

  memset(str, 0, MAX_CHARS_PER_ETX_LINK * TSCH_QUEUE_MAX_NEIGHBOR_QUEUES + 1);
  for (i = 0; i < len; i += 3)
  {
    uint8_t node = *(metric + i);
    uint16_t etx_link = (*(metric + i + 1) << 8) + *(metric + i + 2);
    uint16_t first = etx_link / 1000;
    uint16_t second = etx_link % 1000;

    string_offset += sprintf(&str[string_offset], "%i:%i.%i", node, first, second);

    if (i + 3 < len)
    {
      string_offset += sprintf(&str[string_offset], ", ");
    }
  }

  printf("ETX-Links - FROM %i; %s\n", metric_owner, str);
}

/* Put the already calculated metrics to an array */
int calculate_etx_metric()
{
  struct tsch_neighbor *nbr = tsch_queue_first_nbr();
  int pos = 0;
  memset(etx_links, 0, (TSCH_QUEUE_MAX_NEIGHBOR_QUEUES - 2) * 2);

  while (nbr != NULL)
  {
    etx_links[pos] = (uint8_t)nbr->addr.u8[NODE_ID_INDEX];
    pos++;
    etx_links[pos] = (nbr->etx_link >> 8) & 0xff;
    etx_links[pos + 1] = nbr->etx_link & 0xff;

    pos += 2;

    nbr = tsch_queue_next_nbr(nbr);
  } 

  return pos; 
}

/*---------------------------------------------------------------------------*/
/* Activate metric gathering for the Network. This has to be started by the CPAN*/
static void master_start_metric_gathering(void *ptr)
{
  LOG_INFO("Starting metric gathering\n");
  tsch_set_eb_period(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * 2 * CLOCK_SECOND);

  handle_state_change(ST_POLL_NEIGHBOUR);

  int len = calculate_etx_metric();
  print_metric((uint8_t *) etx_links, node_id, len);
  setBit(metric_received, node_id - 1);

  has_next_neighbor();
  
  handle_convergcast();
}

/*Before starting metric gathering, leave enough time for the network to propagate all time source changes through beacons */
static void finalize_neighbor_discovery(void *ptr)
{
  LOG_DBG("Deactivate neighbor switching. nbrs = %i\n", tsch_queue_count_nbr());
  tsch_change_time_source_active = 0;

  if(tsch_is_coordinator)
  {
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
    /* wait for end of TSCH initialization phase, timed with MASTER_INIT_PERIOD */
    uint8_t cycles = 50; //How many beacon cycles should be left before gathering starts
    ctimer_set(&install_schedule_timer, CLOCK_SECOND * deployment_node_count * cycles * get_percent_of_second(tsch_ts_timeslot_length), master_start_metric_gathering, NULL);
#else
    ctimer_set(&install_schedule_timer, MASTER_INIT_PERIOD, master_install_schedule, NULL);
#endif
  }
}
/*---------------------------------------------------------------------------*/
/* Set the callback from a higher application layer to receive packets */ 
void master_routing_set_input_callback(master_routing_input_callback callback)
{
  if (current_state != ST_SCHEDULE_INSTALLED)
  {
    init_master_routing();
  }
  current_callback = callback;
}

/* Handle the case when retransmits are requested and the requested packet was transmitted */
void handle_retransmit()
{
  //Check if there are other neighbors containing packets for retransmitting
  struct tsch_neighbor* nbr = tsch_queue_first_nbr();

  while(nbr != NULL && tsch_queue_packet_count(&nbr->addr) == 0)
  {
    nbr = tsch_queue_next_nbr(nbr);
  }

  if(nbr != NULL && tsch_queue_packet_count(&nbr->addr) != 0)
  {
    change_link(LINK_TYPE_ADVERTISING, &nbr->addr, get_beacon_slot());
  }else{
    handle_state_change(ST_SCHEDULE_RECEIVED);
    change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());
  }
}

/* Check for missing packets. If all packets are received, finish the schedule gathering. Otherwise send next request for a missing packet */
void request_retransmit_or_finish(const linkaddr_t * destination)
{
  //Only search for packets if we still dont have the whole schedule
  if(current_state != ST_SCHEDULE_OLD)
  {
    return;
  }

  //In a retransmit scenario, we already know how many packets we require for the whole schedule
  int missing_packet = getMissingPacket(received_packets_as_bit_array, schedule_packets);

  if(missing_packet == -1)
  { 
    //We may receive a packet that we requestet without ACK. Remove the requesting packet
    if(tsch_queue_packet_count(destination) > 0)
    {
      tsch_queue_free_packet(tsch_queue_remove_packet_from_queue(tsch_queue_get_nbr(destination)));
    }

    handle_state_change(ST_SCHEDULE_RECEIVED);
    schedule_version++;

    LOG_DBG("Received all packets. Version %d with %d packets\n", schedule_version, schedule_packets);

    setBit1Byte(schedule_received, node_id - 1);

    change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());

    ctimer_stop(&missing_response_to_request_timer);
  }else{

    //Dont add multiple requests when receiving important packets while trying to send this packet
    if(tsch_queue_is_packet_in_nbr_queue(tsch_queue_get_nbr(destination), missing_packet))
    {
      return;
    }

    //Prepare the packets for output
    mrp.flow_number = node_id;
    mrp.packet_number = ++own_packet_number;
    mrp.data[0] = CM_SCHEDULE_RETRANSMIT_REQ;
    mrp.data[1] = missing_packet;

    setup_packet_configuration(tsch_queue_get_nbr(destination)->etx_link, CM_SCHEDULE_RETRANSMIT_REQ, mrp.data[1], 0);

    masternet_len = minimal_routing_packet_size + 2;

    NETSTACK_NETWORK.output(destination);
    ctimer_restart(&missing_response_to_request_timer);
  }
}

/*---------------------------------------------------------------------------*/

void missing_response()
{
  change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());
  handle_state_change(ST_WAIT_FOR_SCHEDULE);
}

/* The callback for TSCH eb_received. Check if the network received the schedule on all nodes */
void master_schedule_received_callback(uint8_t *schedule_received_from_others, uint8_t len)
{
  //To save time, dont even try to compare the received schedules if this is not event the current phase
  if (current_state >= ST_SCHEDULE_RETRANSMITTING && current_state <= ST_SCHEDULE_RECEIVED)
  {
    int i;

    //Check the current state ofthe network. if someone knows about another node having all parts of the schedule, mark this node too
    for(i=0; i< last_node_id;i++)
    {
      if(!isBitSet1Byte(schedule_received, i) && isBitSet1Byte(schedule_received_from_others, i))
      {
        setBit1Byte(schedule_received, i);
      }
    }

    if(check_received_schedules_by_nodes())
    {
      LOG_INFO("All nodes have a schedule. install\n");
      tsch_set_eb_period((uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * CLOCK_SECOND));
      ctimer_set(
        &install_schedule_timer, 
        (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * MASTER_BEACONS_AFTER_SCHEDULE_DIST * CLOCK_SECOND), 
        install_schedule, 
        NULL);
    }
  }
}

void master_schedule_difference_callback(linkaddr_t * nbr, uint8_t nbr_schedule_version, uint16_t nbr_schedule_packets)
{
  //Dont get triggered by every beacon to start packet requesting
  if(current_state == ST_SCHEDULE_OLD)
  {
    return;
  }

  if(nbr_schedule_version > schedule_version)
  {
    //Only try to request packets from nodes with a reasonable ETX
    if(tsch_queue_get_nbr(nbr)->etx_link > 2000)
    {
      return;
    }

    //Remember how many packets the new version of the schedule has.
    schedule_packets = nbr_schedule_packets;
    int missing_packet = getMissingPacket(received_packets_as_bit_array, schedule_packets);

    //The schedule was complete, just not received in order.
    if(missing_packet == -1)
    {
      handle_state_change(ST_SCHEDULE_RECEIVED);
      schedule_version++;

      setBit1Byte(schedule_received, node_id - 1);

      LOG_DBG("Received all packets. Version %d with %d packets\n", schedule_version, schedule_packets);
      return;
    }
    LOG_DBG("Higher schedule from a nbr detected (%d %d)\n", nbr_schedule_version, schedule_version);

    handle_state_change(ST_SCHEDULE_OLD);

    mrp.flow_number = node_id;
    mrp.packet_number = ++own_packet_number;
    mrp.data[0] = CM_SCHEDULE_RETRANSMIT_REQ;
    mrp.data[1] = missing_packet;
    setup_packet_configuration(tsch_queue_get_nbr(nbr)->etx_link, CM_SCHEDULE_RETRANSMIT_REQ, mrp.data[1], 0);
    change_link(LINK_TYPE_ADVERTISING, nbr, get_beacon_slot());

    masternet_len = minimal_routing_packet_size + 2;

    NETSTACK_NETWORK.output(nbr);

    //Set up a timer for missing responses. We might get a request throught hat will be remoed by installing the schedule
    //In this case, we will simple time out and start listening for beacons and send a request again.
    ctimer_set(&missing_response_to_request_timer, CLOCK_SECOND * 5, missing_response, NULL);
  }
}

/* On a timeout, send a braodcast for the node IDs that we miss the metric for*/
static void missing_metric_timeout(void *ptr)
{
  handle_state_change(ST_SEARCH_MISSING_METRIC);
  int i;
  int missing_nodes = 0;
  int nodes_to_count = deployment_node_count;

  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  mrp.data[0] = CM_ETX_METRIC_MISSING;

#if TESTBED == TESTBED_KIEL
  nodes_to_count++;
#endif
  for(i = 1; i < nodes_to_count; i++)
  {    
    if(isBitSet(metric_received, i) == 0)
    {
      //If the metric is missing, include the missing node id into the packet
      mrp.data[2 + missing_nodes] = i + 1;
      missing_nodes++;
    }
  }

  //How many nodes are missing
  mrp.data[1] = missing_nodes;

  setup_packet_configuration(10, mrp.data[0] , mrp.packet_number, 0);

  change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());

  masternet_len = minimal_routing_packet_size + missing_nodes + 2;
  NETSTACK_NETWORK.output(&tsch_broadcast_address);

  ctimer_restart(&missing_metric_timer);
}

/*---------------------------------------------------------------------------*/
/* Depending on the state, set other flags for TSCH and MASTER */
void handle_state_change(enum phase new_state)
{
  switch (new_state)
  {
  case ST_BEGIN_GATHER_METRIC:
      LOG_DBG("changed state ST_BEGIN_GATHER_METRIC\n");
      break;
  case ST_POLL_NEIGHBOUR:
      LOG_DBG("changed state ST_POLL_NEIGHBOUR\n");
      break;
  case ST_SEND_METRIC:
      LOG_DBG("changed state ST_SEND_METRIC\n");
      break;
  case ST_POLL_MISSING_METRIC:
      LOG_DBG("changed state ST_POLL_MISSING_METRIC\n");
      break;
  case ST_SEARCH_MISSING_METRIC:
      LOG_DBG("changed state ST_SEARCH_MISSING_METRIC\n");
      break;
  case ST_WAIT_FOR_SCHEDULE:
      LOG_DBG("changed state ST_WAIT_FOR_SCHEDULE\n");
      break;
  case ST_SCHEDULE_DIST:
      LOG_DBG("changed state ST_SCHEDULE_DIST\n");
      break;
  case ST_SCHEDULE_OLD:
      LOG_DBG("changed state ST_SCHEDULE_OLD\n");
      break;
  case ST_SCHEDULE_RETRANSMITTING:
      LOG_DBG("changed state ST_SCHEDULE_RETRANSMITTING\n");
      break;
  case ST_SCHEDULE_RECEIVED:
      LOG_DBG("changed state ST_SCHEDULE_RECEIVED\n");
      break;
  case ST_SCHEDULE_INSTALLED:
      LOG_DBG("changed state ST_SCHEDULE_INSTALLED\n");
      break;
  case ST_IGNORE_COMMANDS:
      LOG_DBG("changed state ST_IGNORE_COMMANDS\n");
      break;
  default:
    break;
  }
  current_state = new_state;
}

/* Get the next neighbour that has this node as his time source */
int has_next_neighbor()
{
  //We are starting the search for a nbr. find first dest adress
  if(next_dest == NULL)
  {
    return 0;
  }

  if(linkaddr_cmp(&next_dest->addr, &tsch_broadcast_address))
  {
    next_dest = tsch_queue_first_nbr();
    while (next_dest->time_source != linkaddr_node_addr.u8[NODE_ID_INDEX])
    {
      next_dest = tsch_queue_next_nbr(next_dest);

      if (next_dest == NULL)
      {
        return 0;
      }
    }
  }else{
    do
    {
      next_dest = tsch_queue_next_nbr(next_dest);

      if (next_dest == NULL)
      {
        return 0;
      }

    } while (next_dest->time_source != node_id);
  }

  return 1;
}

/* Handle the cases where the packets are distributed the first time in the network.
 * We do not handle retransmits here already since there is a speciall callback for this case.
 */
int handle_schedule_distribution_state_changes(enum commands command, uint16_t len)
{
  int result = -1;

  //A packet with the schedule and we are missing this packet
  if((command == CM_SCHEDULE) && (isBitSet(received_packets_as_bit_array, mrp.data[1]) == 0))
  {
    handle_state_change(ST_SCHEDULE_DIST);
    command_input_schedule_packet(command, len);
    result = 1;
  }

  //The last packet off the schedule and we are missing this packet
  if((command == CM_SCHEDULE_END) && (isBitSet(received_packets_as_bit_array, mrp.data[1]) == 0))
  {
    handle_state_change(ST_SCHEDULE_DIST);
    command_input_schedule_last_packet(command, len);
    result = 1;
  }

  //A retransmit of a packet.
  //Treat retransmits as normal packets until we realise that we have an old schedule
  if((command == CM_SCHEDULE_RETRANSMIT) && (isBitSet(received_packets_as_bit_array, mrp.data[1]) == 0))
  {
    handle_state_change(ST_SCHEDULE_DIST);

    //If we miss this packet, add the packet to the schedule
    if(isBitSet(received_packets_as_bit_array, mrp.data[1]) == 0)
    {
      LOG_DBG("---------------- Packet Retransmit Nr.%d \n", mrp.data[1]);
      command_input_schedule_new_packet(len);
    }
    result = 1;
  }

  return result;
}

/* Handle the callback for the initial schedule distribution by the CPAN */
void callback_input_schedule_send()
{
  //Only CPAN is sending packet after packet. Other Nodes only receive
  if(node_id != MASTER_TSCH_DISTRIBUTOR)
  {
    //LOG_INFO("Nothing to do after resending a schedule as a normal node\n");
    return;
  }

  schedule_packet_number++;
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  mrp.data[0] = CM_SCHEDULE;
  mrp.data[1] = schedule_packet_number;
  setBit(received_packets_as_bit_array, schedule_packet_number);

  if(fill_schedule_packet_from_flash(2, schedule_packet_number) == 1)
  {
    schedule_version++;
    schedule_packets = schedule_packet_number;

    //Let other nodes know by beacon, of how many nodes we know, that they finished receiving the schedule
    LOG_DBG("Finished. Set byte at %i\n", node_id);
    setBit1Byte(schedule_received, node_id - 1);

    mrp.data[0] = CM_SCHEDULE_END;
    setup_packet_configuration(10, CM_SCHEDULE_END, mrp.packet_number, 0);
  }else if(fill_schedule_packet_from_flash(2, schedule_packet_number) == 2)
  {
    own_packet_number--;
    setup_packet_configuration(10, CM_SCHEDULE, mrp.packet_number, 0);
  }else{
    setup_packet_configuration(10, CM_SCHEDULE, mrp.packet_number, 0);
  }

  NETSTACK_NETWORK.output(&tsch_broadcast_address);
}

/* Logic for the convergast that is depending on the next state */
void callback_convergcast(enum phase next_state)
{
  //In case the link to the time source already exists, dont remove and add it again
  switch (next_state)
  {
  case ST_SEND_METRIC:
    //Change to time source
    change_link(LINK_TYPE_ADVERTISING, &tsch_queue_get_time_source()->addr, get_beacon_slot());
    break;

  case ST_POLL_NEIGHBOUR:
    handle_convergcast();
    break;
  case ST_WAIT_FOR_SCHEDULE:
    //As the coordinator, set a timer when polling is finished. In case metric do not arrive, start searching for missing packets
    if(tsch_is_coordinator)
    {
      ctimer_set(&missing_metric_timer, CLOCK_SECOND * 20, missing_metric_timeout, NULL);
    }
    change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());
    break;  
  default:
    break;
  }
}

/* Handle the incoming data and pass it to the upper layer */
void command_input_data_received(uint16_t received_asn, uint16_t len)
{
  //filter out duplicate packets by remebering the last received packet for a flow
  if (TSCH_SLOTNUM_LT((uint16_t)schedule_config.last_received_relayed_packet_of_flow[mrp.flow_number - 1], mrp.packet_number))
  {                                                                                             // if old known one < new one
    schedule_config.last_received_relayed_packet_of_flow[mrp.flow_number - 1] = mrp.packet_number; // update last received packet number
    LOG_INFO("received %u at ASN %u (current asn %lu) from flow %u\n", mrp.packet_number, received_asn,  tsch_current_asn.ls4b, mrp.flow_number);
    current_callback((void *)&mrp.data[COMMAND_END], len - minimal_routing_packet_size, schedule_config.sender_of_flow[mrp.flow_number - 1], schedule_config.receiver_of_flow[mrp.flow_number - 1]);  
  }
  else
  {
    LOG_INFO("received %u at ASN %u duplicate from flow %u\n", mrp.packet_number, received_asn, mrp.flow_number);
  }  
}

/* Forward the packet to the next node in the flow */
void command_input_data_forwarding(struct master_tsch_schedule_t* schedule, uint16_t received_asn, uint16_t len)
{
  // forward Packet
  uint8_t next_receiver = get_forward_dest(schedule, mrp.flow_number);
  if (next_receiver != 0)
  {
    if (TSCH_SLOTNUM_LT((uint16_t)schedule_config.last_received_relayed_packet_of_flow[mrp.flow_number - 1], mrp.packet_number))
    {                                                                                             // if old known one < new one
      schedule_config.last_received_relayed_packet_of_flow[mrp.flow_number - 1] = mrp.packet_number; // update last received packet number
      set_destination_link_addr(next_receiver);
    #if TSCH_FLOW_BASED_QUEUES
      sent_packet_configuration.flow_number = mrp.flow_number;
    #endif /* TSCH_FLOW_BASED_QUEUES */

    #if TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES
      sent_packet_configuration.send_to_nbr = 0;
    #endif    
    
    sent_packet_configuration.command = CM_DATA;
    sent_packet_configuration.packet_nbr = mrp.packet_number;
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
        LOG_INFO("relay packet %u. received at ASN %u (current asn %lu) from flow %u\n", mrp.packet_number, received_asn, tsch_current_asn.ls4b , mrp.flow_number);
      }
      else
      {
        LOG_INFO("packet %i not enqueueing: next ASN %u, received ASN %u, TTL ASN %u for flow %i\n", mrp.packet_number, (uint16_t)tsch_current_asn.ls4b, received_asn, mrp.ttl_slot_number, mrp.flow_number);
      }
    #else
      sent_packet_configuration.max_tx = get_max_transmissions(schedule, mrp.flow_number);
      masternet_len = len; // send same length as received
      NETSTACK_NETWORK.output(&destination);
      LOG_INFO("relay %u at ASN %lu (received at %u) from flow %u\n", mrp.packet_number, tsch_current_asn.ls4b, received_asn, mrp.flow_number);
    #endif /* TSCH_TTL_BASED_RETRANSMISSIONS */
    }
    else
    {
      printf("received %u duplicate from flow %u at ASN %u for relay\n", mrp.packet_number, mrp.flow_number, received_asn);
    }
  }
  else
  {
    LOG_INFO("No routing info for flow %u at node %u\n", mrp.flow_number, node_id);
  }
}

/* Handle a retransmitted packet*/
void command_input_schedule_retransmitt(uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
  //Get the schedule packet number
  int retransmitted_packet = mrp.data[1];

  //While parsing and retransmitting, the src addr becomes null. Save the address localy
  const linkaddr_t source_addr = *src;

  //If we miss this packet, add the packet to the schedule
  if(isBitSet(received_packets_as_bit_array, retransmitted_packet) == 0)
  {
    LOG_DBG("---------------- Packet Retransmit Nr.%d \n", retransmitted_packet);
    command_input_schedule_new_packet(len);
  }

  //If this packet was send to us, check if other packets are missing and start the next request
  if(linkaddr_cmp(&linkaddr_node_addr, dest))
  {
    //Remove packets that have a request for this packet.
    if(tsch_queue_is_packet_in_nbr_queue(tsch_queue_get_nbr(&source_addr), retransmitted_packet))
    {
      tsch_queue_free_packet(tsch_queue_remove_packet_from_queue(tsch_queue_get_nbr(&source_addr)));
    }
    request_retransmit_or_finish(&source_addr); 

  }else{
    //In case this packet was received but was for another node, dont request the next missing packet

    int missing_packet = getMissingPacket(received_packets_as_bit_array, schedule_packets);

    if(missing_packet == -1)
    { 
      handle_state_change(ST_SCHEDULE_RECEIVED);
      ctimer_stop(&missing_response_to_request_timer);
      change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());

      schedule_version++;
      setBit1Byte(schedule_received, node_id - 1);

      LOG_DBG("Received all packets. Version %d with %d packets\n", schedule_version, schedule_packets);
    }
  }
}

/* Handle behaviour in case of a request for a retransmit from another node */
void command_input_schedule_retransmitt_request(const linkaddr_t *src)
{
  //While parsing and retransmitting, the src addr becomes null. Save the address localy
  const linkaddr_t source_addr = *src;

  /* Small optimization: Due to missing ACK's, a node that sends requests to us for retransmit might send the same 
   * request multiple times. Since only 1 request by each node will be send at a time, check if the queue for the requester contains the
   * packet that was requestet.
  */
  if(tsch_queue_is_packet_in_nbr_queue(tsch_queue_get_nbr(&source_addr), mrp.data[1]))
  {
    return;
  }

  handle_state_change(ST_SCHEDULE_RETRANSMITTING);

  //We received a request to retransmit a packet. enter the retransmit state and prepare the packet.
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  mrp.data[0] = CM_SCHEDULE_RETRANSMIT;
  int missing_packet = mrp.data[1];

  if(fill_schedule_packet_from_flash(2, missing_packet) == 2)
  {
    LOG_ERR("Could not send retransmit for packet %i\n",missing_packet);
    return;
  }

  setup_packet_configuration(tsch_queue_get_nbr(&source_addr)->etx_link, CM_SCHEDULE_RETRANSMIT, mrp.data[1], 1);

  NETSTACK_NETWORK.output(&source_addr);

  //If we receive requests to retransmit parts of the schedule, we add them to the queue of the neighbor
  //When changing the link to a requester for a retransmit, check if we are looking at the broadcast address:
  //If broadcast address -> this is the first request for a retransmit. Change to requester and the packet will be sent
  //If not broadcast address -> we already send a retransmit to another nbr. Link change will be perfmored later in the callback
  if(linkaddr_cmp(&tsch_schedule_get_link_by_timeslot(sf[1], get_beacon_slot())->addr, &tsch_broadcast_address) != 0)
  {
    tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &source_addr, get_beacon_slot(), 0);
  }
}

/* Unpack a schedule packet and mark it as received */
void command_input_schedule_new_packet(uint16_t len)
{
  //Unpack into the schedule structure
  if(unpack_schedule_to_flash(len - minimal_routing_packet_size))
  {
    //Check the packets in the bit vector to later find missing packets
    setBit(received_packets_as_bit_array, mrp.data[1]);
  }
}

/* Handle a packet containing the last part of the TSCH schedule */
void command_input_schedule_last_packet(enum commands command, uint16_t len)
{
  LOG_DBG("---------------- Packet Nr.%d (last packet + forward)\n", mrp.data[1]);

  command_input_schedule_new_packet(len);

  //Check if a packet is missing
  schedule_packets = mrp.data[1];
  int missing_packet = getMissingPacket(received_packets_as_bit_array, schedule_packets);

  //If all packets are received, set the schedule version and the state. Active EB's again to signal the new schedule version
  if(missing_packet == -1)
  { 
    handle_state_change(ST_SCHEDULE_RECEIVED);
    schedule_version++;
    setBit1Byte(schedule_received, node_id - 1);

    LOG_DBG("Received all packets. Version %d with %d packets\n", schedule_version, schedule_packets);
  }else{
    LOG_DBG("Missing some packets. wait for beacons\n");
  }
  tsch_change_time_source_active = 1;

  setup_packet_configuration(10, command, mrp.packet_number, 0);

  masternet_len = len;
  NETSTACK_NETWORK.output(&tsch_broadcast_address);

}

/* Handle a packet containing a part of the TSCH schedule */
void command_input_schedule_packet(enum commands command, uint16_t len)
{
  LOG_DBG("---------------- Packet Nr.%d (forward packet)\n", mrp.data[1]);

  command_input_schedule_new_packet(len);

  setup_packet_configuration(10, command, mrp.packet_number, 0);
  masternet_len = len;
  NETSTACK_NETWORK.output(&tsch_broadcast_address);
}

/* Search the missing node ids. If this node is in the list, return 1. Otherwise return 0
*/
int missing_metric_from_myself()
{
  uint8_t len = mrp.data[1];
  uint8_t missing_metric_from_node[TSCH_QUEUE_MAX_NEIGHBOR_QUEUES];
  memcpy(missing_metric_from_node, &mrp.data[2], len);

  int i;
  for(i=0; i < len; i++)
  {
    //Missing metric from me
    if(node_id == missing_metric_from_node[i])
    {
      return 1;
    }
  }

  return 0;
}

/* Search the neighbors for missing node metrics. 1 if a neighbor is set as the next_dest. 0 otherwise
*/
struct tsch_neighbor * has_missing_node_as_nbr()
{
  //First check if we have any of the nodes as neighbors that are missing
  uint8_t len = mrp.data[1];
  uint8_t missing_metric_from_node[TSCH_QUEUE_MAX_NEIGHBOR_QUEUES];
  memcpy(missing_metric_from_node, &mrp.data[2], len);

  struct tsch_neighbor * n = tsch_queue_first_nbr();
  struct tsch_neighbor * current_choice = NULL;
  while(n != NULL)
  {
    int i;
    for(i=0; i < len; i++)
    {
      //If this neighbor is one of the missing nodes, we need to poll him
      if(n->addr.u8[NODE_ID_INDEX] == missing_metric_from_node[i] && n->etx_link < 2000)
      {
        if(current_choice == NULL)
        {
          current_choice = n;
          break;
        }else{
          //Choose the neighbor with the best ETX-link for polling
          if(n->rank < current_choice->rank)
          {
            current_choice = n;
            break;
          }
        }
      }
    }
    n = tsch_queue_next_nbr(n);
  }

  return current_choice;
}

/* The distributor node received a request from the CPAN to prepare the serial input for the schedule.
  First we send a notification that we are ready */
void command_input_prepare_distributor()
{
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  uint8_t command = CM_START_DIST_COMMIT;
  memcpy(&(mrp.data), &command, sizeof(uint8_t));

  masternet_len = minimal_routing_packet_size + sizeof(uint8_t);

  setup_packet_configuration(tsch_queue_get_time_source()->etx_link, command, mrp.packet_number, 0);

  change_link(LINK_TYPE_NORMAL, &tsch_queue_get_time_source()->addr, get_beacon_slot());

  NETSTACK_NETWORK.output(&tsch_queue_get_time_source()->addr);
}

/* We received a command indicating the missing ETX-links. */
void command_input_missing_metric(int len)
{
  struct tsch_neighbor * to_poll = has_missing_node_as_nbr();

  //We found some neighbor
  if(to_poll != NULL)
  {
    handle_state_change(ST_POLL_MISSING_METRIC);
    convergcast_poll_neighbor(CM_ETX_METRIC_MISSING, to_poll);
  }else{
    //No neighbor found. Forward the broadcast
    handle_state_change(ST_SEARCH_MISSING_METRIC);

    mrp.flow_number = node_id;
    mrp.packet_number = ++own_packet_number;

    setup_packet_configuration(10, mrp.data[0] , mrp.packet_number, 0);

    change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());

    masternet_len = len;
    NETSTACK_NETWORK.output(&tsch_broadcast_address);
  }
}

/* Handle the poll from a neighbor node demanding the ETX-metric.
 * CPAN will start the gathering. Other Nodes will react and poll their neighbors.
*/
void command_input_get_metric()
{
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  int command = CM_ETX_METRIC_SEND;

  int len = calculate_etx_metric();

  memcpy(&(mrp.data), &command, sizeof(uint8_t));
  memcpy(&(mrp.data[1]), etx_links, len);
  masternet_len = minimal_routing_packet_size + sizeof(uint8_t) + len;

  handle_convergcast();
}

/* Behaviour for the CPAN when a metric is received. */
void command_input_send_metric_CPAN(uint16_t len, const linkaddr_t *src)
{
  if(metric_complete)
  {
    return;
  }

  //While in neighbor discovery mode, flow number = node_id
  setBit(metric_received, mrp.flow_number - 1);

  int nodes_to_count = deployment_node_count;
  int i;
  int finished_nodes = 0;
#if TESTBED == TESTBED_KIEL
  nodes_to_count++;
#endif
  for(i = 0; i < nodes_to_count; i++)
  {
    if(isBitSet(metric_received, i))
    {
      finished_nodes++;
    }
  }

  //Once all metrics are received, stop the timer to handle missing metrics and start listening for the schedule transmission
  print_metric(&mrp.data[COMMAND_END], mrp.flow_number, len - minimal_routing_packet_size - COMMAND_END);

  //We are done and now send a message to the distributor node
  if(finished_nodes == deployment_node_count)
  {
    ctimer_stop(&missing_metric_timer);
    metric_complete = 1;

    handle_state_change(ST_WAIT_FOR_DIST_COMMIT);
    //Send a message to prepare the schedule distributor for the reception of the Schedule
    mrp.flow_number = node_id;
    mrp.packet_number = ++own_packet_number;
    uint8_t command = CM_START_DIST_COMMIT;
    memcpy(&(mrp.data), &command, sizeof(uint8_t));

    masternet_len = minimal_routing_packet_size + sizeof(uint8_t);

    struct tsch_neighbor *distributor_node = tsch_queue_get_nbr_by_node_id(MASTER_TSCH_DISTRIBUTOR);

    if(distributor_node == NULL)
    {
      LOG_ERR("Network configuration wrong. Distributor node not in range!\n");
      tsch_disassociate();
      return;
    }

    setup_packet_configuration(distributor_node->etx_link, command, mrp.packet_number, 0);

    change_link(LINK_TYPE_NORMAL, &distributor_node->addr, get_beacon_slot());

    NETSTACK_NETWORK.output(&distributor_node->addr);
  }else{
    //We are not done. reset the timer and wait for more packets
    if(current_state == ST_WAIT_FOR_SCHEDULE)
    {
      ctimer_restart(&missing_metric_timer);
    }
  }
}

/* Behaviour for a node that is not the CPAN if a metric arrives.*/
void command_input_send_metric_Node(uint16_t len, const linkaddr_t *src)
{
  masternet_len = len;

  // Response of the Polling request, forward the metric to own time source
  if(current_state == ST_WAIT_FOR_SCHEDULE && tsch_queue_is_empty(tsch_queue_get_time_source()))
  {        
    LOG_DBG("Packet arrived, forward!\n");
    handle_convergcast();
  }else{
    LOG_DBG("Add packet to time source queue %i\n", tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX]);
    setup_packet_configuration(tsch_queue_get_time_source()->etx_link, CM_ETX_METRIC_SEND, mrp.packet_number, 0);
    NETSTACK_NETWORK.output(&tsch_queue_get_time_source()->addr);
  }
}

/* Handle packet containing the ETX-metric of a node 
  return 1 if the command could be handled, 0 otherwise*/
int command_input_send_metric(uint16_t len, const linkaddr_t *src)
{
  /* Small optimization: only works for direct neighbors */
  //In case we received the metric from our neighbor and he did not receive our ACK, he will resend the metric again -> Drop the packet here
  //The first check is to see if this packet is a metric from the sender or a forward from a node further away. 
  //In the second case, we might not have this neighbor and do not check if the packet should be dropet
  struct tsch_neighbor * nbr = tsch_queue_get_nbr(src);
  if(mrp.flow_number == src->u8[NODE_ID_INDEX] && nbr != NULL && nbr->etx_metric_received)
  {
    return 0;
  }

  //Set the flag for received metric
  if(nbr != NULL)
  {
    nbr->etx_metric_received = 1;
  }

  // Received metric from other nodes
  if (tsch_is_coordinator)
  {
    command_input_send_metric_CPAN(len, src);
  }else
  {
    command_input_send_metric_Node(len, src);
  }
  return 1;
}

/* Handle the transitions between states after a callback */
void transition_to_new_state_after_callback(enum commands command, int has_packets_to_forward)
{
  //We only end up in this function after a succesfull transmission and acknowledgment of a packet
  enum phase next_state = 0;

  //State machine (after sent packet)
  switch (command)
  {
    
    //A packet was sent succesfully
    case CM_ETX_METRIC_SEND:
      //If there are packets left, keep sending the packets until the queue is empty. Otherwise poll neighbors
      //Only poll neighbors if there are any neighbors left to poll. Otherwise we are done.
      next_state = has_packets_to_forward ? ST_SEND_METRIC : has_next_neighbor() ? ST_POLL_NEIGHBOUR : ST_WAIT_FOR_SCHEDULE;

      handle_state_change(next_state);
      callback_convergcast(next_state);
      break;

    //Poll command sent succesfully
    case CM_ETX_METRIC_GET:
      //If there are packets left, keep sending the packets until the queue is empty. Otherwise poll neighbors
      //Only poll neighbors if there are any neighbors left to poll. Otherwise we are done.
      next_state = has_packets_to_forward ? ST_SEND_METRIC : has_next_neighbor() ? ST_POLL_NEIGHBOUR : ST_WAIT_FOR_SCHEDULE;

      handle_state_change(next_state);
      callback_convergcast(next_state);
      break;

    //We sent a POLL after a missing request succesfully
    case CM_ETX_METRIC_MISSING:
      //Check if we are dont or if we have a packet to forward
      next_state = has_packets_to_forward ? ST_SEND_METRIC : ST_WAIT_FOR_SCHEDULE;

      handle_state_change(next_state);
      callback_convergcast(next_state);
      break;

    //We send as the CPAN/Distributor node the message to start schedule upload.
    case CM_START_DIST_COMMIT:
      //As the CPAN, we are in a dedicated state waiting for an answer.
      //As the Distributor, we react to a succesfull transmission and start the serial input process
      if(node_id == MASTER_TSCH_DISTRIBUTOR)
      {
        change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());
        printf("leaving the network\n");
        stop_tsch_timer();
        handle_state_change(ST_WAIT_FOR_SCHEDULE);
        tsch_queue_reset();
        process_start(&serial_line_schedule_input, NULL);
        process_start(&distributor_callback, NULL);
      }
      break;

    //We sent a part of the schedule succesfully
    case CM_SCHEDULE:
      callback_input_schedule_send();
      break;

    //We sent the last part the schedule succesfully
    case CM_SCHEDULE_END:   
      if(getMissingPacket(received_packets_as_bit_array, schedule_packets) == -1)
      {
        //Once the schedule is broadcastet completly, start sending EB's with new version and packet number.
        //Otherwise wait at this state. At some point we will receive beacons with the schedule version.
        handle_state_change(ST_SCHEDULE_RECEIVED);
      }
      break;

    //We requested a retransmit succesfully
    case CM_SCHEDULE_RETRANSMIT_REQ:

      //If we are still missing parts
      if(current_state == ST_SCHEDULE_OLD){

        //Check if we received the packet that we just requested already though "overhearing"
        if(isBitSet(received_packets_as_bit_array, packetbuf_attr(PACKETBUF_ATTR_PACKET_NUMBER)))
        {
          //When sending a packet, the packetbuffer is reset in Master-NET. Save the destination address as a copy
          const linkaddr_t dst = *packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
          request_retransmit_or_finish(&dst); 
        }else{
          //We are still missing the packet that we requested succesfully. 
          //Wait x seconds before searching for new nbrs with the whole schedule
          ctimer_restart(&missing_response_to_request_timer);
        }
      }
      break;

    //We just retransmited a packet succesfully
    case CM_SCHEDULE_RETRANSMIT:
      if(current_state == ST_SCHEDULE_RETRANSMITTING)
      {
        handle_retransmit();
      }
      break;

    default:
      LOG_INFO("Dont react to callback for command %d\n", command);
      break;
  }
}

/* handle the transition between states after a packet input */
int transition_to_new_state(enum commands command, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
  int result = 1;
  
  if(current_state != ST_SCHEDULE_INSTALLED)
  {
    LOG_DBG("Received a packet from %d with len %d and command %d. packets in queue %i\n", src->u8[NODE_ID_INDEX], len, command, tsch_queue_global_packet_count());
  }

  //State machine
  switch (current_state)
  {
  //The initial state of MASTER
  case ST_EB:

    //We received a request to send our metric.
    if(command == CM_ETX_METRIC_GET)
    {
      tsch_set_eb_period(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * 2 * CLOCK_SECOND);
      handle_state_change(ST_SEND_METRIC);
      command_input_get_metric();
    }
    
    //We only want to react to missing metric requests from nodes that are not our time source
    //If they are our time source and they did not poll us, they are not receiving our packets.
    if(command == CM_ETX_METRIC_MISSING && linkaddr_cmp(src, &tsch_queue_get_time_source()->addr) == 0){
      tsch_set_eb_period(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * 2 * CLOCK_SECOND);
      tsch_queue_update_time_source(src);
      handle_state_change(ST_SEND_METRIC);
      command_input_get_metric();
    }else {
      result = -1;
    }
    break;

  //We are currently polling neighbors for metrics
  case ST_POLL_NEIGHBOUR:

    //If a metric arrives, forward it or add it to the queue
    if(command == CM_ETX_METRIC_SEND)
    {
      command_input_send_metric(len, src);
    }
    
    //Only for Distributor node!
    //We received a request to prepare for serial input
    if(command == CM_START_DIST_COMMIT)
    {
      command_input_prepare_distributor();
    }else{
      result = -1;
    }
    break;

  //We are forwarding metrics right now
  case ST_SEND_METRIC:

    //If we receive more metrics, add them to the queue to forward or print them
    if(command == CM_ETX_METRIC_SEND)
    {
      command_input_send_metric(len, src);
    }

    //We received a request for missing metrics. Send our metric
    if(command == CM_ETX_METRIC_MISSING)
    {
      LOG_DBG("Switch to nbr %i after missing metric request\n", src->u8[NODE_ID_INDEX]);
      tsch_queue_update_time_source(src);
      command_input_get_metric();
    }

    //Only for Distributor node!
    //We received a request to prepare for serial input
    if(command == CM_START_DIST_COMMIT)
    {
      command_input_prepare_distributor();
    }else{
      result = -1;
    }
    break;

  //We are currently searching for missing metrics
  case ST_POLL_MISSING_METRIC:

    //Forward the metric that we received or print it
    if(command == CM_ETX_METRIC_SEND)
    {
      command_input_send_metric(len, src);
    }
    
    //Only for Distributor node!
    //We received a request to prepare for serial input
    if(command == CM_START_DIST_COMMIT)
    {
      command_input_prepare_distributor();
    }else{
      result = -1;
    }
    break;

  //Only the CPAN will be in this state!
  case ST_WAIT_FOR_DIST_COMMIT:

    //The distributor node send us a message indicating he is finished
    if(command == CM_START_DIST_COMMIT)
    {
      //This line is used from MASTER to start the schedule upload. Print it always!!
      printf("ETX-Links finished!\n");
      handle_state_change(ST_WAIT_FOR_SCHEDULE);
      change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());
    }
    break;

  //In this state we wait for the schedule distribution. Packets containing the ETX-metric to forward can still arrive
  case ST_WAIT_FOR_SCHEDULE:

    //Only for Distributor node!
    //We received a request to prepare for serial input
    if(command == CM_START_DIST_COMMIT)
    {
      command_input_prepare_distributor();

    }else if(command == CM_ETX_METRIC_SEND)
    {
      //We are waiting for the schedule but a packet to forward arrived.
      int change_state = command_input_send_metric(len, src);
      if(!tsch_is_coordinator && change_state)
      {
        handle_state_change(ST_SEND_METRIC);
      }

    }else if(command == CM_ETX_METRIC_MISSING && ingore_missing_metric_requests == 0){
      //A request for missing parts of the etx-metric arrived and we are not ignored this requests

      //As the coordinator we ignore the broadcasts. This is only a repeat of our initial message
      if(tsch_is_coordinator)
      {
        return 0;
      }

      //Stop receiving more packets asking for the missing schedule since message is propagated by flooding the network
      ingore_missing_metric_requests = 1;
      ctimer_set(&ignore_metric_timer, CLOCK_CONF_SECOND * 5, activate_missing_metric_requests, NULL);

      //If we are one of the nodes that is missing, send the metric
      if(missing_metric_from_myself())
      {
        tsch_queue_update_time_source(src);
        command_input_get_metric();
      }else{
        //Otherwise, look up the neighbors if we know one of the missing nodes
        command_input_missing_metric(len);
      }

    }else{
      //Check if we received a packet with a part of the schedule
      result = handle_schedule_distribution_state_changes(command, len);
    }
    break;

  //We are currently receiving schedules
  case ST_SCHEDULE_DIST:
    //Check if this is a schedule packet
    result = handle_schedule_distribution_state_changes(command, len);
    break;

  //We have now received all parts of the schedule
  case ST_SCHEDULE_RECEIVED:

    //Check if we received a request for retransmit
    if(command == CM_SCHEDULE_RETRANSMIT_REQ)
    {
      command_input_schedule_retransmitt_request(src);
    }else{
      result = -1;
    }
    break;

  //We are currently retransmitting a packet
  case ST_SCHEDULE_RETRANSMITTING:

    //Check if we received a request for retransmit
    if(command == CM_SCHEDULE_RETRANSMIT_REQ)
    {
      command_input_schedule_retransmitt_request(src);
    }else{
      result = -1;
    }
    break;

  //We have an old schedule and wait for retransmits right now
  case ST_SCHEDULE_OLD:

    //Receive a retransmitting packet
    if(command == CM_SCHEDULE_RETRANSMIT)
    {
      command_input_schedule_retransmitt(len, src, dest);
    }else{
      result = -1;
    }
    break;

  //Our schedule is installed. We handle only data packets at this point
  case ST_SCHEDULE_INSTALLED:
    if(command == CM_DATA)
    {
      uint16_t received_asn = packetbuf_attr(PACKETBUF_ATTR_RECEIVED_ASN); //The rx slot when the packet was received in slot-operation
      struct master_tsch_schedule_t* schedule = get_own_schedule();

      // This node is the receiver of the flow
      if (node_id == schedule_config.receiver_of_flow[mrp.flow_number - 1])
      {
        command_input_data_received(received_asn, len);
      }else{
        //Forward the packet through the flow
        command_input_data_forwarding(schedule, received_asn, len);
      }
    }else{
      result = -1;
    }
    break;

  default:
    LOG_DBG("Something went wrong!\n");
    result = -1;
    break;
  }

  return result;
}

/* Handle the incoming packets from TSCH */
void master_routing_input(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
  //Ignore packets that have an invalid size
  if (len < minimal_routing_packet_size || len > maximal_routing_packet_size)
  {
    LOG_ERR("Invalid packet size %d!", len);
    return;
  }

  //Save the buffer into the packet struct and get the command inside the packet
  memcpy(&mrp, data, len);
  int command = mrp.data[0];

  //In case this is an invalid command for the current state, return
  if(transition_to_new_state(command, len, src, dest) == -1)
  {
    LOG_DBG("Invalid command %d during state %d\n", command, current_state);
    return;
  }
}

/*---------------------------Callback after sending packets and their main functions--------------------------------*/ 
/* Handle callbacks for the schedule distribution */
void handle_callback_commands_divergcast(enum commands command, int ret, int transmissions)
{
  if(ret == MAC_TX_ERR)
  {
    //We could not add a packet to queue. Drop the packet
    if(current_state == ST_SCHEDULE_OLD)
    {
      handle_state_change(ST_SCHEDULE_DIST);
      change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());
      ctimer_stop(&missing_response_to_request_timer);
    }else if(current_state == ST_SCHEDULE_RETRANSMITTING) {
      handle_retransmit();
    }
    return;
  }

  //This will only happen in unicast -> retransmit/retransmit_request
  if (ret != MAC_TX_OK)
  {
    //Currently we set 9 bytes for "overhearing"
    int ie_offset = 0;
    if(packetbuf_attr(PACKETBUF_ATTR_MAC_METADATA))
    {
      ie_offset = 9;
    }

    //Get the packet back into the mrp struct for them packetbuffer
    uint16_t hrd_len = packetbuf_attr(PACKETBUF_ATTR_PACKET_HDR_SIZE);
    memset(&mrp, 0, sizeof(mrp));
    memcpy(&mrp, packetbuf_dataptr() + hrd_len + ie_offset, packetbuf_datalen() - hrd_len - ie_offset);

    //datalen for incoming packets = hdr + data. for outgoing (error on adding packet to queue) it is only the data len
    //hdr_len is 0 for incoming packtes. otherwise it is the header size for outgoing.
    //dataptr: for outgoing packets-> pointer to payload. for incoming packets -> pointer to header
    linkaddr_t dest_resend = *packetbuf_addr(PACKETBUF_ADDR_RECEIVER);

    //Small optimization: in case we did not receive an ACK but the nbr send us already the requestet packet, drop the request
    if(command == CM_SCHEDULE_RETRANSMIT_REQ)
    {
      if(isBitSet(received_packets_as_bit_array, mrp.data[1]))
      {
        LOG_DBG("We already have this %d packet. Dropp request for it\n", mrp.data[1]);
        request_retransmit_or_finish(&dest_resend); 
        return;
      }else{
        LOG_DBG("Bad link quality. drop search and wait for new beacon from other neighbor\n");
        handle_state_change(ST_SCHEDULE_DIST);
        change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());
        ctimer_stop(&missing_response_to_request_timer);
        return;
      }
    }
    if(command == CM_SCHEDULE_RETRANSMIT)
    {
      setup_packet_configuration(tsch_queue_get_nbr(&dest_resend)->etx_link, CM_SCHEDULE_RETRANSMIT, mrp.data[1], 1);

      masternet_len = packetbuf_datalen() - hrd_len - ie_offset;

      NETSTACK_NETWORK.output(&dest_resend);
      return;
    }
  }

  //No error occured. Handle the callback after a packet was sent succesfully
  transition_to_new_state_after_callback(command, 0);
}

/* Handle callbacks for the ETX-metric gathering */
void handle_callback_commands_convergcast(enum commands command, int ret, int transmissions)
{
  //In case of an error, retransmit the packet again
  if (ret != MAC_TX_OK)
  {
    struct tsch_neighbor * nbr = tsch_queue_get_nbr(packetbuf_addr(PACKETBUF_ADDR_RECEIVER));
    if(nbr == NULL)
    {
      return;
    }

    /* Small optimizaton: If we poll a neighbor and dont receive ACKS, but already received this neighbors etx-metric, stop sending requests*/
    if(current_state == ST_POLL_NEIGHBOUR && nbr->etx_metric_received)
    {
      LOG_DBG("Packet received while polling neighbor %d, dont repeat request!\n", nbr->addr.u8[NODE_ID_INDEX]);

    }else if(current_state == ST_POLL_MISSING_METRIC && command == CM_ETX_METRIC_MISSING){
      LOG_DBG("Did not reach the src. drop packet\n");
      return; 

    }else if(current_state == ST_SEND_METRIC){
      /* Small optimization: Sometimes we end up sending the link to a neighbor that has a horrible ETX-link. 
       In this case, we might get a request to send the metric to someone else and we will then change the timesource.
       If we have a different time source in the callback, this case happened and we throw away the packet. */
      if(!tsch_is_coordinator && !linkaddr_cmp(&tsch_queue_get_time_source()->addr, &nbr->addr))
      {
        return;
      }
      uint16_t hrd_len = packetbuf_attr(PACKETBUF_ATTR_PACKET_HDR_SIZE);
      memset(&mrp, 0, sizeof(mrp));
      memcpy(&mrp, packetbuf_dataptr() + hrd_len, packetbuf_datalen() - hrd_len);
      masternet_len = packetbuf_datalen() - hrd_len;

      handle_convergcast();
      return;
    }
  }

  if(tsch_is_coordinator)
  {
    transition_to_new_state_after_callback(command, 0);
  }else{
    transition_to_new_state_after_callback(command, tsch_queue_is_empty(tsch_queue_get_time_source()) == 0);
  }
}

// Callback for sent packets over TSCH.
void master_routing_output_callback(void *data, int ret, int transmissions)
{
  //Dont react to packets during the installation and schedule installed state
  //We will flush the queue during schedule installation leading to callbacks beeing fired
  if(current_state == ST_IGNORE_COMMANDS || current_state == ST_SCHEDULE_INSTALLED)
  {
    return;
  }

  //Get the command from the packet that we sent
  enum commands command = packetbuf_attr(PACKETBUF_ATTR_PACKET_COMMAND);

  //Ignore the following command types
  if(command == CM_DATA || command == CM_NO_COMMAND || command == CM_END)
  {
    return;
  }

  LOG_DBG("Sent Packet! Current state: %d with command %d and return value %d for receiver %i\n", current_state, command, ret, packetbuf_addr(PACKETBUF_ADDR_RECEIVER)->u8[NODE_ID_INDEX]);

  //Handle the metric gathering callback once a packet is sent or an error on sent occured
  if(command >= CM_ETX_METRIC_GET && command <= CM_ETX_METRIC_SEND)
  {
    handle_callback_commands_convergcast(command, ret, transmissions);
  }

  //Handle the metric distribution callback once a packet is sent or an error on sent occured
  if(command >= CM_SCHEDULE && command <= CM_SCHEDULE_END)
  {
    handle_callback_commands_divergcast(command, ret, transmissions);
  }

  //Only for CPAN and Distributor node
  if(command == CM_START_DIST_COMMIT)
  {
    if(ret != MAC_TX_OK){
      //In the rare case where the acknoweldgement of the CPAN went missing and we try to resend the commit,
      //while we already received the message from the distributor node, that he left the network, stop resending
      if(current_state != ST_WAIT_FOR_DIST_COMMIT)
      {
        return;
      }

      setup_packet_configuration(tsch_queue_get_nbr(packetbuf_addr(PACKETBUF_ADDR_RECEIVER))->etx_link, CM_START_DIST_COMMIT, mrp.packet_number, 0);

      masternet_len = minimal_routing_packet_size + sizeof(uint8_t);
      
      NETSTACK_NETWORK.output(packetbuf_addr(PACKETBUF_ADDR_RECEIVER));
    }else{
      transition_to_new_state_after_callback(command, 0);
    }
  }
}

/*------------------------------------------------------------------------------------------------------*/ 
master_packetbuf_config_t
master_routing_sent_configuration()
{
  return sent_packet_configuration;
}
/*---------------------------------------------------------------------------*/
int master_routing_configured()
{
  return current_state == ST_SCHEDULE_INSTALLED;
}
/*---------------------------------------------------------------------------*/
int master_routing_send(const void *data, uint16_t datalen)
{
  //In case we dont have the schedule configured, dont do anything
  if(!master_routing_configured())
  {
    LOG_WARN("No schedule configured yet!\n");
    return 0;
  }

  struct master_tsch_schedule_t* schedule = get_own_schedule();
  if (schedule->own_transmission_flow != 0)
  {
    mrp.flow_number = schedule->own_transmission_flow;
    mrp.packet_number = ++own_packet_number;
    mrp.data[0] = CM_DATA;
    memcpy(&mrp.data[1], data, datalen);

    // get current / next active ASN (tsch_current_asn)
    // get corresponding slotframe slot number (TSCH_ASN_MOD(tsch_current_asn, sf->size))
    struct tsch_slotframe *sf;
    uint16_t sf_size;
    uint16_t current_sf_slot;
    sf = tsch_schedule_get_slotframe_by_handle(schedule->own_transmission_flow);
    sf_size = ((uint16_t)((sf->size).val));
    current_sf_slot = TSCH_ASN_MOD(tsch_current_asn, sf->size);


#if TSCH_TTL_BASED_RETRANSMISSIONS
    mrp.ttl_slot_number = (uint16_t)tsch_current_asn.ls4b + sf_size - current_sf_slot + (uint16_t)schedule_config.last_tx_slot_in_flow[schedule->own_transmission_flow - 1];
    mrp.earliest_tx_slot = (uint16_t)tsch_current_asn.ls4b + sf_size - current_sf_slot + (uint16_t)schedule_config.first_tx_slot_in_flow[schedule->own_transmission_flow - 1]; // earliest slot in next slotframe

    if (TSCH_SLOTNUM_LT(mrp.earliest_tx_slot, (last_sent_packet_asn + schedule_config.schedule_length)))
    { // avoid duplicates in earliest ASN
      --own_packet_number;
      LOG_INFO("Too high sending frequency, try again later\n");
      return 0;
    }
    last_sent_packet_asn = mrp.earliest_tx_slot;
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS */

    uint8_t next_receiver = get_forward_dest(schedule, mrp.flow_number);
    if (next_receiver != 0)
    {
      set_destination_link_addr(next_receiver);

#if TSCH_FLOW_BASED_QUEUES
      sent_packet_configuration.flow_number = mrp.flow_number;
#endif /* TSCH_FLOW_BASED_QUEUES */

#if TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES
      sent_packet_configuration.send_to_nbr = 0;
#endif    

#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
      sent_packet_configuration.overhearing_active = 0;
#endif

#if TSCH_TTL_BASED_RETRANSMISSIONS
      // packetbuf set TTL
      sent_packet_configuration.ttl_slot_number = mrp.ttl_slot_number;
      sent_packet_configuration.earliest_tx_slot = mrp.earliest_tx_slot;
      // set max_transmissions
      sent_packet_configuration.max_tx = (uint16_t)TSCH_SLOTNUM_DIFF16(mrp.ttl_slot_number, (uint16_t)(tsch_current_asn.ls4b - 1)); //(uint16_t) (0xFFFF + 1 + nullnet_routing_packet.ttl_slot_number - nullnet_routing_packet.earliest_tx_slot); //include earliest slot!
#else
      sent_packet_configuration.max_tx = get_max_transmissions(schedule, sent_packet_configuration.flow_number);
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS */

      masternet_len = minimal_routing_packet_size + COMMAND_END + datalen;
      sent_packet_configuration.command = CM_DATA;
      sent_packet_configuration.packet_nbr = mrp.packet_number;
      NETSTACK_NETWORK.output(&destination);

#if TSCH_TTL_BASED_RETRANSMISSIONS
      LOG_INFO("sent %u at ASN %u till ASN %u  (current asn %lu) for flow %u\n", mrp.packet_number, mrp.earliest_tx_slot, mrp.ttl_slot_number, tsch_current_asn.ls4b, mrp.flow_number);
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

      printf("sent %u at ASN %u from sender %u\n", mrp.packet_number, earliest_tx_slot_asn, node_id);
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS */
    }
    else
    {
      LOG_INFO("No routing info for flow %u\n", mrp.flow_number);
    }
    return 1;
  }
  else
  {
    LOG_INFO("Node %u is no sender!\n", node_id);
    return 0;
  }
}
/*---------------------------------------------------------------------------*/
int master_routing_sendto(const void *data, uint16_t datalen, uint8_t receiver)
{
  if (receiver == get_node_receiver())
  {
    return master_routing_send(data, datalen);
  }
  else
  {
    return 0;
  }
}
/*---------------------------------------------------------------------------*/
void init_master_routing(void)
{
#if NETSTACK_CONF_WITH_MASTER_NET
  LOG_INFO("Start master!\n");
  /* configure transmit power */
#if CONTIKI_TARGET_ZOUL && defined(MASTER_CONF_CC2538_TX_POWER)
  NETSTACK_RADIO.set_value(RADIO_PARAM_TXPOWER, MASTER_CONF_CC2538_TX_POWER);
#endif /* CONTIKI_TARGET_ZOUL && defined(MASTER_CONF_CC2538_TX_POWER) */

#if MAC_CONF_WITH_TSCH
  int is_coordinator = linkaddr_cmp(&coordinator_addr, &linkaddr_node_addr);
  tsch_set_coordinator(is_coordinator);
  // The Enhanced beacon timer
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
//
  tsch_set_eb_period((uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * CLOCK_SECOND));
  LOG_DBG("Generate beacon every %lu ms \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * CLOCK_SECOND));

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
  masternet_set_output_callback(master_routing_output_callback);
  masternet_set_config_callback(master_routing_sent_configuration);
  tsch_set_schedule_difference_callback(master_schedule_difference_callback);
  tsch_set_schedule_received_callback(master_schedule_received_callback);

  tsch_schedule_remove_all_slotframes();
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
  uint32_t array_size;
  if(MAX_PACKETS_PER_SCHEDULE % 32 != 0)
  {
    array_size = (MAX_PACKETS_PER_SCHEDULE / 32) + 1;
  }else{
    array_size = MAX_PACKETS_PER_SCHEDULE / 32;
  }

  resetBitVector(received_packets_as_bit_array, array_size);
  resetBitVector(metric_received, array_size);

  sf[0] = tsch_schedule_add_slotframe(0, 1);                      // Listen on this every frame where the nodes doesnt send

  //Make slotframes always uneven
  if(deployment_node_count % 2)
  {
    sf[1] = tsch_schedule_add_slotframe(1, deployment_node_count); 
  }else{
    sf[1] = tsch_schedule_add_slotframe(1, deployment_node_count + 1);  
  }

  tsch_schedule_add_link(sf[0], LINK_OPTION_RX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, 0, 0);
  tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot(), 0);

  LOG_DBG("Time to run before convergcast = %lu sek \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT));
  LOG_DBG("Time to run before convergcast = %lu ticks \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT * CLOCK_SECOND));
  ctimer_set(&install_schedule_timer, (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * (TSCH_BEACON_AMOUNT + 10) * CLOCK_SECOND), finalize_neighbor_discovery, NULL);

  next_dest = tsch_queue_get_nbr(&tsch_broadcast_address);
#else
  sf[0] = tsch_schedule_add_slotframe(0, 1);
  tsch_schedule_add_link(sf[0], LINK_OPTION_TX | LINK_OPTION_RX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, 0, 0);

  /* wait for end of TSCH initialization phase, timed with MASTER_INIT_PERIOD */
  ctimer_set(&install_schedule_timer, MASTER_INIT_PERIOD, master_install_schedule, NULL);
#endif /* TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY */
#else
  LOG_ERR("can't init master-routing: master-net not configured\n");
#endif /* NETSTACK_CONF_WITH_MASTER_NET */
}
/*---------------------------------------------------------------------------*/
/** @} */
