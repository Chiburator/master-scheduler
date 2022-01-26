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

/*Deployment node count*/
#if TESTBED == TESTBED_COOJA
  static const uint8_t deployment_node_count = NUM_COOJA_NODES;
  master_tsch_schedule_t schedules[NUM_COOJA_NODES] = {0};
  uint8_t metric_received[NUM_COOJA_NODES] = {0};
#elif TESTBED == TESTBED_FLOCKLAB
  static const uint8_t deployment_node_count = 27;
  master_tsch_schedule_t schedules[27] = {0};
    uint8_t metric_received[27] = {0};
#elif TESTBED == TESTBED_KIEL
  static const uint8_t deployment_node_count = 20;
  master_tsch_schedule_t schedules[20] = {0};
  uint8_t metric_received[21] = {0}; //node 11 is missing and instead node 21 exists
#elif TESTBED == TESTBED_DESK
  static const uint8_t deployment_node_count = 5;
  master_tsch_schedule_t schedules[5] = {0};
    uint8_t metric_received[5] = {0};
#endif /* TESTBED */

/*Destination*/
#if (TESTBED == TESTBED_KIEL || TESTBED == TESTBED_DESK) && CONTIKI_TARGET_ZOUL
static linkaddr_t destination = {{0x00, 0x12, 0x4B, 0x00, 0x00, 0x00, 0x00, 0x00}};
#else
static linkaddr_t destination = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
#endif

master_tsch_schedule_universall_config_t schedule_config = {0};

static master_routing_packet_t mrp; // masternet_routing_packet   (mrp)

static uint8_t COMMAND_END = 1;

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
static uint8_t last_schedule_packet_number_received = 0;
static uint8_t schedule_complete = 0;

//static uint8_t beacon_slot = 0;
enum phase current_state = ST_EB;
struct tsch_neighbor *next_dest = NULL;

// scheduled with Master
static struct tsch_slotframe *sf[MASTER_NUM_FLOWS + 1]; // 1 sf per flow + EB-sf

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
//char test[100];
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

/*---------------------------------------------------------------------------*/
static void set_destination_link_addr(uint8_t destination_node_id)
{
  destination.u8[NODE_ID_INDEX] = destination_node_id;
}

/*---------------------------------------------------------------------------*/
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY

uint8_t last_schedule_id_started = 0;
uint16_t last_byte_filled = 0;
uint16_t remaining_len_last_packet = 0;
uint8_t finished = 0;

#if TSCH_TTL_BASED_RETRANSMISSIONS
void set_ttl_retransmissions()
{
  uint16_t sf_size;
  sf_size = ((uint16_t)((sf[1]->size).val));
  sent_packet_configuration.ttl_slot_number = (uint16_t)tsch_current_asn.ls4b + 10*sf_size;
  sent_packet_configuration.earliest_tx_slot = (uint16_t)tsch_current_asn.ls4b; 
}
#endif //TSCH_TTL_BASED_RETRANSMISSIONS

//Once the schedule is distributed, reset the flag that was used while the etx-metric was gathered
void reset_nbr_metric_received()
{
  struct tsch_neighbor *n = tsch_queue_first_nbr();

  while(n != NULL)
  {
    n->etx_metric_received = 0;
    n = tsch_queue_next_nbr(n);
  }
}

uint8_t fill_packet(int bytes_in_packet)
{
  printf("Start writting at: %d\n", bytes_in_packet);
  //TODO:: if the schedule is already installed, dont save the time. Also only CPAN should save the time?
  if(finished)
  {
    mrp.data[0] = CM_SCHEDULE_END;
    sent_packet_configuration.command = CM_SCHEDULE_END;

    struct tsch_asn_t schedule_start;
    schedule_start.ls4b = tsch_current_asn.ls4b;
    schedule_start.ms1b = tsch_current_asn.ms1b;

    //10 cycles * node amount
    TSCH_ASN_INC(schedule_start, 10*5);

    mrp.data[bytes_in_packet] = tsch_current_asn.ms1b;
    bytes_in_packet++;
    WRITE32(&mrp.data[bytes_in_packet], tsch_current_asn.ls4b);
    bytes_in_packet += 4;

    mrp.data[bytes_in_packet] = schedule_start.ms1b;
    bytes_in_packet++;
    WRITE32(&mrp.data[bytes_in_packet], schedule_start.ls4b);
    bytes_in_packet += 4;
    return 1;
  }
  uint8_t schedule_finished = 0;

  mrp.data[bytes_in_packet] = last_schedule_id_started;             //schedule id where unpacking starts again
  mrp.data[bytes_in_packet + 1] = (last_byte_filled >> 8) & 255;    //where writting data was stoped at last iteration in the last schedule id
  mrp.data[bytes_in_packet + 2] = last_byte_filled & 255; 
  bytes_in_packet += 3;

  uint16_t schedule_len = 2 + 2 * MASTER_NUM_FLOWS + 1 + schedules[last_schedule_id_started].links_len * sizeof(scheduled_link_t);
  while(last_schedule_id_started < deployment_node_count)
  {
    //Send packet if even the length of the packets has not enough space
    if((MASTER_MSG_LENGTH - bytes_in_packet) < 2)
    {
      break;
    }

    //remove the bytes from the last packets from the total length
    schedule_len -= last_byte_filled;

    //add the length to the packet
    mrp.data[bytes_in_packet] = (schedule_len >> 8) & 255;    //packet length
    mrp.data[bytes_in_packet + 1] = schedule_len & 255; 
    bytes_in_packet += 2;

    //The first cas is where a whole schedule for a node fits into the packet. The second case i only a part of the schedule is in this packet
    if((MASTER_MSG_LENGTH - bytes_in_packet) >= schedule_len)
    {

      memcpy(&mrp.data[bytes_in_packet], ((uint8_t *)&schedules[last_schedule_id_started]) + last_byte_filled, schedule_len);
      printf("Packet contains whole schedule for id:%d. Wrote bytes from %d to %d (total %d)\n", last_schedule_id_started, last_byte_filled, last_byte_filled + schedule_len, schedule_len);
      printf("Packet contains %d links \n", schedules[last_schedule_id_started].links_len);
      bytes_in_packet += schedule_len;
      last_schedule_id_started++;
      last_byte_filled = 0;
      schedule_finished = 1;
      schedule_len = 2 + 2 * MASTER_NUM_FLOWS + 1 + schedules[last_schedule_id_started].links_len * sizeof(scheduled_link_t);

    }else{
      //if the length of the packet does not fit, leave the rest of the packet empty
      memcpy(&(mrp.data[bytes_in_packet]), (uint8_t *)&schedules[last_schedule_id_started] + last_byte_filled, (MASTER_MSG_LENGTH - bytes_in_packet));
      printf("Packet contains part of schedule for id:%d. Wrote bytes from %d to %d (total %d)\n", last_schedule_id_started, last_byte_filled, last_byte_filled + (MASTER_MSG_LENGTH - bytes_in_packet), (MASTER_MSG_LENGTH - bytes_in_packet));
      printf("Packet contains %d links \n", schedules[last_schedule_id_started].links_len);
      last_byte_filled = (MASTER_MSG_LENGTH - bytes_in_packet);
      bytes_in_packet += last_byte_filled;
      schedule_finished = 0;
      break;
    }
  }
  printf("Packet contains out\n");

  //Check if we finished writing the whole schedule with this packet.
  if(last_schedule_id_started == deployment_node_count && schedule_finished)
  {
    finished = 1;
  }

  uint8_t done = 0;
  //The last packet contains 2 asn. 
  //1. asn at this moment when the packet is created
  //2. asn offset from this moment when the network should switch to the new schedule.
  if(finished && ((MASTER_MSG_LENGTH - bytes_in_packet) >= 10))
  {
    mrp.data[0] = CM_SCHEDULE_END;
    sent_packet_configuration.command = CM_SCHEDULE_END;

    struct tsch_asn_t schedule_start;
    schedule_start.ls4b = tsch_current_asn.ls4b;
    schedule_start.ms1b = tsch_current_asn.ms1b;

    //10 cycles * node amount
    TSCH_ASN_INC(schedule_start, 10 * 5);

    mrp.data[bytes_in_packet] = tsch_current_asn.ms1b;
    bytes_in_packet++;
    WRITE32(&mrp.data[bytes_in_packet], tsch_current_asn.ls4b);
    bytes_in_packet += 4;

    mrp.data[bytes_in_packet] = schedule_start.ms1b;
    bytes_in_packet++;
    WRITE32(&mrp.data[bytes_in_packet], schedule_start.ls4b);
    bytes_in_packet += 4;

    if(tsch_is_coordinator)
    {
      //The time passed since the asn for the start was calculated
      LOG_ERR("time passed since the schedule was calculated %d  and offset in asn when to start %d (time %d ms)\n", tsch_current_asn.ls4b, schedule_start.ls4b, (CLOCK_SECOND * schedule_start.ls4b * deployment_node_count) / 1000);
      //TODO:: calculate start of installation for the schedule. 
      ctimer_set(&install_schedule_timer, (CLOCK_SECOND * schedule_start.ls4b * deployment_node_count) / 1000, install_schedule, NULL);
    }
    done = 1;
  }

  masternet_len = bytes_in_packet + minimal_routing_packet_size;

  #if TSCH_TTL_BASED_RETRANSMISSIONS
  set_ttl_retransmissions();
  #endif

  NETSTACK_NETWORK.output(&tsch_broadcast_address); //TODO:: find out how to send this via broadcast. deactivate EB again?

  return done;
}

void unpack_packet(int packet_len)
{
  //check if this is the first packet
  uint8_t unpack_asns = 0;
  int unpack_at_index = 2; //0 = command, 1 = packet number, >= 2 data
  printf("Unpack packt of len: %d \n", packet_len);
  if(mrp.data[1] == 1)
  {
    schedule_config.schedule_length = mrp.data[2];
    schedule_config.slot_frames = mrp.data[3];
    memcpy(schedule_config.sender_of_flow, &(mrp.data[4]), MASTER_NUM_FLOWS);
    memcpy(schedule_config.receiver_of_flow, &(mrp.data[4 + MASTER_NUM_FLOWS]), MASTER_NUM_FLOWS);
    memcpy(schedule_config.first_tx_slot_in_flow, &(mrp.data[4 + 2*MASTER_NUM_FLOWS]), MASTER_NUM_FLOWS);
    memcpy(schedule_config.last_tx_slot_in_flow, &(mrp.data[4 + 3*MASTER_NUM_FLOWS]), MASTER_NUM_FLOWS);
    unpack_at_index += 2 + 4*MASTER_NUM_FLOWS;
    printf("first packet contained universal config. unpack %d bytes \n", unpack_at_index);
  }
  uint8_t schedule_index = mrp.data[unpack_at_index];
  unpack_at_index++;
  uint16_t last_byte_written = mrp.data[unpack_at_index] << 8;
  last_byte_written += mrp.data[unpack_at_index + 1];
  unpack_at_index += 2;
  //printf("unpacked bytes %d\n", unpack_at_index);
  
  if(mrp.data[0] == CM_SCHEDULE_END)
  {
    packet_len = packet_len - 10;
    unpack_asns = 1;
  }
 
  while(unpack_at_index < packet_len)
  {
    //Get the len of the current schedule
    uint16_t schedule_len = mrp.data[unpack_at_index] << 8;
    schedule_len += mrp.data[unpack_at_index + 1];
    unpack_at_index += 2;
    //printf("Schedule index %d = %d and last byte written %d\n", schedule_index, schedule_len, last_byte_written);
    if(schedule_len + unpack_at_index <= packet_len)
    {
      memcpy((uint8_t *)&schedules[schedule_index] + last_byte_written, &mrp.data[unpack_at_index], schedule_len);
      printf("Packet contains full schedule to unpack for schedule %d. start from %d to %d (totall %d)\n", schedule_index, unpack_at_index, unpack_at_index + schedule_len, schedule_len);
      schedule_index++;
      unpack_at_index += schedule_len;
      last_byte_written = 0;

    }else{
      //in case this is a part of a packet, keep track how many bytes to read for the next packet.
      //remaining_len_last_packet = (unpack_at_index + schedule_len) - packet_len;
      //Unpack the remaining bytes into the schedule.
      memcpy((uint8_t *)&schedules[schedule_index] + last_byte_written, &(mrp.data[unpack_at_index]), packet_len - unpack_at_index);
      printf("Packet contains full schedule to unpack for schedule %d. start from %d to %d (totall %d)\n", schedule_index, unpack_at_index, unpack_at_index + packet_len - unpack_at_index, packet_len - unpack_at_index);
      unpack_at_index += packet_len - unpack_at_index;
    }
  }

  if(unpack_asns == 1)
  {
    LOG_ERR("Packet contains asn\n");

    struct tsch_asn_t asn_on_creation;
    asn_on_creation.ms1b = mrp.data[unpack_at_index];
    unpack_at_index++;
    READ32(&mrp.data[unpack_at_index], asn_on_creation.ls4b);
    unpack_at_index += 4;

    struct tsch_asn_t schedule_start;
    schedule_start.ms1b = mrp.data[unpack_at_index];
    unpack_at_index++;
    READ32(&mrp.data[unpack_at_index], schedule_start.ls4b);
    unpack_at_index += 4;
    //The time passed since the asn for the start was calculated
    int time_passed = TSCH_ASN_DIFF(tsch_current_asn, asn_on_creation);
    asn_on_creation.ls4b = time_passed;
    int start_offset = TSCH_ASN_DIFF(schedule_start, asn_on_creation);
    LOG_ERR("time passed since the schedule was calculated %d  and offset in asn when to start %d (time %d ms)\n", time_passed, start_offset, (CLOCK_SECOND * start_offset * deployment_node_count) / 1000);
    //TODO:: calculate start of installation for the schedule. 
    ctimer_set(&install_schedule_timer, (CLOCK_SECOND * start_offset * deployment_node_count) / 1000, install_schedule, NULL);
  }
}

void master_schedule_loaded_callback()
{
  LOG_ERR("Schedule loaded");
  //As the Coordiantor has the schedule, he will ignore packets received by other nodes
  last_schedule_packet_number_received = 255;
  //As the Coordinator, the schedule is already complete after we arrive at this callback
  schedule_complete = 1;
  //deactivate EB since EB packets are a higher priority and will block the distribution of the schedule
  tsch_eb_active = 0;
  //Enter state for schedule distribution
  current_state = ST_DIST_SCHEDULE;
  //TODO:: fix this later to max_tx = worst etx of all nbrs
  sent_packet_configuration.max_tx = 1;
  //Signal for receiver, that this is not the last packet
  sent_packet_configuration.command = CM_SCHEDULE;

  //Start with universal config 34 bytes + 2 bytes for the command and packet number
  int packet_bytes_filled = 4 + 4*MASTER_NUM_FLOWS; 
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  mrp.data[0] = CM_SCHEDULE;
  mrp.data[1] = schedule_packet_number;
  schedule_packet_number++;

  mrp.data[2] = schedule_config.schedule_length;
  mrp.data[3] = schedule_config.slot_frames;
  memcpy(&(mrp.data[4]), schedule_config.sender_of_flow, MASTER_NUM_FLOWS);
  memcpy(&(mrp.data[4 + MASTER_NUM_FLOWS]), schedule_config.receiver_of_flow, MASTER_NUM_FLOWS);
  memcpy(&(mrp.data[4 + 2*MASTER_NUM_FLOWS]), schedule_config.first_tx_slot_in_flow, MASTER_NUM_FLOWS);
  memcpy(&(mrp.data[4 + 3*MASTER_NUM_FLOWS]), schedule_config.last_tx_slot_in_flow, MASTER_NUM_FLOWS);

  fill_packet(packet_bytes_filled);
}

void install_schedule(){
  LOG_INFO("Install schedule now\n");
  int i;
  //TODO:: This has to be changed later
  sf[0] = tsch_schedule_get_slotframe_by_handle(0);
  if (sf[0]){
    tsch_schedule_remove_slotframe(sf[0]);
  }
  for(i=1; i <= schedule_config.slot_frames; i++)
  {
    sf[i] = tsch_schedule_get_slotframe_by_handle(i);
    if (sf[i]){
      tsch_schedule_remove_slotframe(sf[i]);
    }
    sf[i] = tsch_schedule_add_slotframe(i, schedule_config.schedule_length);
  }  

  uint8_t link_idx;
  struct master_tsch_schedule_t* schedule = get_own_schedule();
  for (link_idx = 0; link_idx < schedule->links_len; ++link_idx){
    struct tsch_slotframe *sf = tsch_schedule_get_slotframe_by_handle(schedule->links[link_idx].slotframe_handle);

    if(schedule->links[link_idx].send_receive == LINK_OPTION_RX)
    {
      destination.u8[NODE_ID_INDEX] = 0; 
    }else{
      destination.u8[NODE_ID_INDEX] = get_forward_dest_by_slotframe(schedule, link_idx);
    }

    tsch_schedule_add_link(sf, schedule->links[link_idx].send_receive, LINK_TYPE_NORMAL, &destination, schedule->links[link_idx].timeslot, schedule->links[link_idx].channel_offset);
  }
  //TODO:: this has to me changed later
  tsch_schedule_add_link(sf[1], LINK_OPTION_TX | LINK_OPTION_RX, LINK_TYPE_ADVERTISING_ONLY, &tsch_broadcast_address, 40, 0); 

  tsch_schedule_print();
  tsch_eb_active = 1;
  is_configured = 1;
  LOG_INFO("SCHEDULE INSTALLED!!\n");
}

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

//This function is used only to switch the links during convergast. therefore most parameters are set
void prepare_link_for_metric_distribution(const linkaddr_t* dest, uint16_t timeslot)
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
    LOG_ERR("nbr %d with source %d, my id %d\n", next_dest->addr.u8[NODE_ID_INDEX], next_dest->time_source, node_id);
  } while (next_dest->time_source != node_id);

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
  struct tsch_neighbor *nbr = tsch_queue_first_nbr();
  int pos = 0;
  while (nbr != NULL)
  {
    // This could fail if last_eb = first_eb, e.G. realy rare reception of packets from far nodes
    if(nbr->last_eb == nbr->first_eb)
    {
      nbr = tsch_queue_next_nbr(nbr);
      continue;
    }

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
  } 
  //LOG_ERR("DONE \n");
  return pos; 
}

// Called when the first poll command to get this nodes metric is received
void prepare_etx_metric()
{
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  int command = CM_ETX_METRIC;
  // GPIO_SET_PIN(GPIO_D_BASE, 0x04);
  // GPIO_CLR_PIN(GPIO_D_BASE, 0x04);
  // GPIO_SET_PIN(GPIO_D_BASE, 0x04);
  // GPIO_CLR_PIN(GPIO_D_BASE, 0x04);
  // GPIO_SET_PIN(GPIO_D_BASE, 0x04);
  // GPIO_CLR_PIN(GPIO_D_BASE, 0x04);
  int len = calculate_etx_metric();
  // GPIO_SET_PIN(GPIO_D_BASE, 0x04);
  // GPIO_CLR_PIN(GPIO_D_BASE, 0x04);
  // GPIO_SET_PIN(GPIO_D_BASE, 0x04);
  // GPIO_CLR_PIN(GPIO_D_BASE, 0x04);
  // GPIO_SET_PIN(GPIO_D_BASE, 0x04);
  // GPIO_CLR_PIN(GPIO_D_BASE, 0x04);
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
    prepare_link_for_metric_distribution(&tsch_broadcast_address, node_id - 1);
    LOG_ERR("Finished polling neighbors, EB activate\n");
  }
}

#endif /* TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY */
/*---------------------------------------------------------------------------*/
static void
master_install_schedule(void *ptr)
{
  LOG_INFO("install schedule\n");
  tsch_eb_active = 0;
  LOG_ERR("EB deactivated\n");
  LOG_INFO("Starting convergcast\n");

  if(tsch_is_coordinator)
  {
    int len = calculate_etx_metric();
    print_metric(etx_links, node_id, len);
    metric_received[node_id - 1] = 1;
    current_state = ST_POLL_NEIGHBOUR;
    LOG_INFO("Have %d nbrs\n", tsch_queue_count_nbr());

    next_dest = tsch_queue_first_nbr();
    while (next_dest != NULL && next_dest->time_source != node_id)
    {
      next_dest = tsch_queue_next_nbr(next_dest);
    }
    
    handle_convergcast(0);
  }else{
    LOG_ERR("My time src is %d and nbr size is %d with %d flows\n", tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX], TSCH_QUEUE_MAX_NEIGHBOR_QUEUES, MASTER_NUM_FLOWS);
    if (current_state == ST_BEGIN_GATHER_METRIC)
    {
      LOG_ERR("Starting prepare metric\n");
      prepare_etx_metric();
    }
  }

  started = 1;
  LOG_INFO("started\n");
}

/*Before starting metric gathering, leave enough time for the network to propagate all time source changes through beacons
*/
static void finalize_neighbor_discovery(void *ptr)
{
  LOG_ERR("neighbor changing deactivated. Start gathering soon\n");
  tsch_change_time_source_active = 0;
  uint8_t cycles = 20; //How many beacon cycles should be left before gathering starts
  #if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
    /* wait for end of TSCH initialization phase, timed with MASTER_INIT_PERIOD */
  ctimer_set(&install_schedule_timer, (CLOCK_SECOND * (TSCH_DEFAULT_TS_TIMESLOT_LENGTH / 1000) * deployment_node_count * cycles) / 1000, master_install_schedule, NULL);
  #else
  ctimer_set(&install_schedule_timer, MASTER_INIT_PERIOD, master_install_schedule, NULL);
  #endif
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

void handle_covergcast_callback(packet_data_t *packet_data, int ret, int transmissions)
{
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
      tsch_eb_active = 1;
      prepare_link_for_metric_distribution(&tsch_broadcast_address, node_id - 1);
      LOG_ERR("No neighbors left. EB activate\n");
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
          prepare_link_for_metric_distribution(&tsch_broadcast_address, node_id - 1);
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
      //If There are no packets in the queue for the time source, poll next nbr. otherwise we need to forward packets
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

void handle_divergcast_callback(packet_data_t *packet_data)
{
  //distribution for tsch_coordinator
  if(tsch_is_coordinator)
  {  
    //Only forward next packet if schedule is already complete.
    if(packet_data->command == CM_SCHEDULE)
    {
      LOG_INFO("Callback SCHEDULE\n");
      mrp.flow_number = node_id;
      mrp.packet_number = ++own_packet_number;
      mrp.data[0] = CM_SCHEDULE;
      mrp.data[1] = schedule_packet_number;
      schedule_packet_number++;
      if(fill_packet(2))
      {
        current_state = ST_SCHEDULE_INSTALLED;
      }
    }

    // if(packet_data->command == CM_SCHEDULE_END)
    // {
    //   LOG_INFO("Callback SCHEDULE END. Start Installing\n");

    //   install_schedule();
    //   reset_nbr_metric_received();
    // }
  }else{

    //TODO:: later
    // if(schedule_complete == 0)
    // {

    // }

    // if(schedule_complete == 0 && is_configured == 0)
    // {
    //   LOG_INFO("Callback SCHEDULE END. Start Installing\n");
    //   install_schedule();
    //   reset_nbr_metric_received();
    // }
  }
}

/*---------------------------------------------------------------------------*/
// Callback for sent packets over TSCH.
void master_routing_output(void *data, int ret, int transmissions)
{
  LOG_INFO("master callback for sent packets \n");

  packet_data_t *packet_data = (packet_data_t *)data;
  LOG_ERR("Current state: %d with command %d and return value %d\n", current_state, packet_data->command, ret);

  if(packet_data->command == CM_DATA || packet_data->command == CM_NO_COMMAND || packet_data->command == CM_END)
  {
    //LOG_INFO("Got command after sending %i\n", command);
    return;
  }

  if(packet_data->command < CM_SCHEDULE)
  {
    handle_covergcast_callback(packet_data, ret, transmissions);
  }

  if(packet_data->command == CM_SCHEDULE || packet_data->command == CM_SCHEDULE_END)
  {
    handle_divergcast_callback(packet_data);
  }
}
/*---------------------------------------------------------------------------*/
void master_routing_input(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
  if (len >= minimal_routing_packet_size && len <= maximal_routing_packet_size)
  {
    uint8_t forward_to_upper_layer = 0;
    memcpy(&mrp, data, len);
    int command = mrp.data[0];

    if(is_configured == 0)
    {
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
          //in nbr discovery, flow number = node_id
          metric_received[ mrp.flow_number - 1] = 1;
          print_metric(&mrp.data[COMMAND_END], mrp.flow_number, len - minimal_routing_packet_size - COMMAND_END); //-3 for the mrp flow number and packet number and -command length
          int i;
          int finished_nodes = 0;
          char test[100];
          int offset = 0;
        #if TESTBED == TESTBED_KIEL
          for(i = 0; i < deployment_node_count + 1; i++)
          {
            if(metric_received[i] == 1)
            {
              finished_nodes++;
            }else{
             offset += sprintf(&test[offset], "%i, ", i +1);
            }
          }
        #else
          for(i = 0; i < deployment_node_count; i++)
          {
            if(metric_received[i] == 1)
            {
              finished_nodes++;
            }else{
             offset += sprintf(&test[offset], "%i, ", i +1);
            }
          }
        #endif

          LOG_ERR("Missing metric from %s\n", test);
          if(finished_nodes == deployment_node_count)
          {
            LOG_ERR("ETX-Links finished!");    
            process_start(&serial_line_schedule_input, NULL);
          }
        }else
        {
          LOG_ERR("Deactivate EB\n");
          tsch_eb_active = 0;
          
          masternet_len = len;

          // Response of the Polling request, forward the metric to own time source
          if(current_state == ST_WAIT_FOR_SCHEDULE && tsch_queue_is_empty(tsch_queue_get_time_source()))
          {        
            current_state = ST_SEND_METRIC;
            LOG_ERR("Polling finished but packet arrived. handle convergast!\n");
            handle_convergcast(0);
          }else{
            LOG_ERR("Add packet to time source queue\n");
            prepare_forward_config(tsch_queue_get_time_source()->etx_link, CM_ETX_METRIC);
            NETSTACK_NETWORK.output(&tsch_queue_get_time_source()->addr);
          }
        }
        break;
      case CM_SCHEDULE:
      LOG_ERR("---------------- Packet Nr.%d \n", mrp.data[1]);
        if(last_schedule_packet_number_received == mrp.data[1]- 1)
        {
          sent_packet_configuration.max_tx = 1;
          sent_packet_configuration.command = CM_SCHEDULE;
          masternet_len = len;
          last_schedule_packet_number_received = mrp.data[1];
          //deactive enhanced beacons during metric distribution
          tsch_eb_active = 0;
          current_state = ST_DIST_SCHEDULE;
          //Unpack into the schedule structure
          unpack_packet(len - minimal_routing_packet_size);       
          #if TSCH_TTL_BASED_RETRANSMISSIONS
          set_ttl_retransmissions();
          #endif
          //Mark the packet as received in the bit-vector
          setBit(mrp.packet_number);
          NETSTACK_NETWORK.output(&tsch_broadcast_address);
        }else{
          LOG_ERR("Skip schedule packet %d\n", mrp.data[1]);
        }
        break;
      case CM_SCHEDULE_END:
        //make sure to unpack only the folllowing packets one after another
        LOG_ERR("---------------- Packet Nr.%d Last Packet\n", mrp.data[1]);
        if(last_schedule_packet_number_received == mrp.data[1] - 1)
        {
          sent_packet_configuration.max_tx = 1;
          sent_packet_configuration.command = CM_SCHEDULE_END;
          masternet_len = len;
          last_schedule_packet_number_received = mrp.data[1];
          tsch_eb_active = 0;
          current_state = ST_SCHEDULE_INSTALLED;
          tsch_change_time_source_active = 1;
          setBit(mrp.packet_number);
          unpack_packet(len - minimal_routing_packet_size);
          schedule_complete = 1;
          #if TSCH_TTL_BASED_RETRANSMISSIONS
          set_ttl_retransmissions();
          #endif
          NETSTACK_NETWORK.output(&tsch_broadcast_address);
        }else{
          LOG_ERR("Skip schedule packet %d\n", mrp.data[1]);
        }
        break;
      default:
        break;
      }
    }else{
      if(command != CM_DATA)
      {
        return;
      }
      LOG_ERR("Received %d bytes with command %u\n", len, command);
      uint16_t received_asn = packetbuf_attr(PACKETBUF_ATTR_RECEIVED_ASN);
      struct master_tsch_schedule_t* schedule = get_own_schedule();

      // this node is receiver:
      if (node_id == schedule_config.receiver_of_flow[mrp.flow_number - 1])
      {
        //filter out duplicate packets by remebering the last received packet for a flow
        if (TSCH_SLOTNUM_LT((uint16_t)schedule_config.last_received_relayed_packet_of_flow[mrp.flow_number - 1], mrp.packet_number))
        {                                                                                             // if old known one < new one
          schedule_config.last_received_relayed_packet_of_flow[mrp.flow_number - 1] = mrp.packet_number; // update last received packet number
          LOG_INFO("received %u at ASN %u from flow %u\n", mrp.packet_number, received_asn, mrp.flow_number);
          forward_to_upper_layer = 1;
        }
        else
        {
          LOG_INFO("received %u at ASN %u duplicate from flow %u\n", mrp.packet_number, received_asn, mrp.flow_number);
        }
      }
      else
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
            sent_packet_configuration.max_tx = get_max_transmissions(schedule, mrp.flow_number);
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
    }
    
    if (forward_to_upper_layer && len > minimal_routing_packet_size)
    {
      // TODO: exchange source by flow-source
      // upper layer input callback (&mrp.data, len-minimal_routing_packet_size)
      if(is_configured)
      {
        current_callback((void *)&mrp.data[COMMAND_END], len - minimal_routing_packet_size, schedule_config.sender_of_flow[mrp.flow_number - 1], schedule_config.receiver_of_flow[mrp.flow_number - 1]);
      }else{
        current_callback((void *)&mrp.data[COMMAND_END], len - minimal_routing_packet_size - COMMAND_END, src->u8[NODE_ID_INDEX], dest->u8[NODE_ID_INDEX]);
      }
    }
  }
  // leds_off(LEDS_RED);
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
int get_node_receiver()
{
  return get_own_schedule()->own_receiver;
}
/*---------------------------------------------------------------------------*/
int node_is_sender()
{
  //We are a sender if we have own_receiver != 0
  return get_node_receiver() != 0;
}
/*---------------------------------------------------------------------------*/
int master_routing_configured()
{
  return is_configured;
}
/*---------------------------------------------------------------------------*/
int master_routing_send(const void *data, uint16_t datalen)
{
  //In case we dont have the schedule configured, dont do anything
  if(is_configured == 0)
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
      LOG_TRACE_RETURN("master_routing_send \n");
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

#if TSCH_TTL_BASED_RETRANSMISSIONS
      // packetbuf set TTL
      sent_packet_configuration.ttl_slot_number = mrp.ttl_slot_number;
      sent_packet_configuration.earliest_tx_slot = mrp.earliest_tx_slot;
      // set max_transmissions
      sent_packet_configuration.max_tx = (uint16_t)TSCH_SLOTNUM_DIFF16(mrp.ttl_slot_number, (uint16_t)(tsch_current_asn.ls4b - 1)); //(uint16_t) (0xFFFF + 1 + nullnet_routing_packet.ttl_slot_number - nullnet_routing_packet.earliest_tx_slot); //include earliest slot!
#else
      sent_packet_configuration.max_tx = get_max_transmissions(schedule, sent_packet_configuration.flow_number);
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS */

      LOG_INFO("expected max tx: %u\n", sent_packet_configuration.max_tx);

      masternet_len = minimal_routing_packet_size + COMMAND_END + datalen;
      sent_packet_configuration.command = CM_DATA;
      NETSTACK_NETWORK.output(&destination);
      LOG_ERR("Send %d bytes with command %u\n", masternet_len, mrp.data[0]);
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
}
/*---------------------------------------------------------------------------*/
int master_routing_sendto(const void *data, uint16_t datalen, uint8_t receiver)
{
  LOG_TRACE("master_routing_sendto \n");
  // LOG_INFO("send length %u to %u", datalen, receiver);
  if (receiver == get_node_receiver())
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
#if NETSTACK_CONF_WITH_MASTER_NET
  if (started == 0)
  {
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
    master_schedule_set_schedule_loaded_callback(master_schedule_loaded_callback);

    sf[0] = tsch_schedule_add_slotframe(0, 1);                      // Listen on this every frame where the nodes doesnt send

    //Make slotframes always uneven
    if(deployment_node_count % 2)
    {
      LOG_ERR("Uneven\n");
      sf[1] = tsch_schedule_add_slotframe(1, deployment_node_count);  // send in this frame every "node_count"
    }else{
      sf[1] = tsch_schedule_add_slotframe(1, deployment_node_count + 1);  // send in this frame every "node_count"
      LOG_ERR("Even\n");
    }

    tsch_schedule_add_link(sf[0], LINK_OPTION_RX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, 0, 0);
    tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, node_id - 1, 0);
    /* wait for end of TSCH initialization phase, timed with MASTER_INIT_PERIOD */
    LOG_INFO("Time to run before convergcast = %i", ((TSCH_DEFAULT_TS_TIMESLOT_LENGTH / 1000) * deployment_node_count * TSCH_BEACON_AMOUNT) / 1000);
    ctimer_set(&install_schedule_timer, (CLOCK_SECOND * (TSCH_DEFAULT_TS_TIMESLOT_LENGTH / 1000) * deployment_node_count * TSCH_BEACON_AMOUNT) / 1000, finalize_neighbor_discovery, NULL);
    //ctimer_set(&install_schedule_timer, (CLOCK_SECOND * 40), finalize_neighbor_discovery, NULL);
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
