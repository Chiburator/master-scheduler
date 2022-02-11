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

//Calculate the worst case package count = (Nodes * Schedule_size_per_node + universall config) / payload_size  
//In case of Kiel, we miss node 11 and have node 21 instead -> expand array by 1 node     
#if TESTBED == TESTBED_KIEL
#define MAX_PACKETS_PER_SCHEDULE (((NUM_COOJA_NODES + 1) * (4*TSCH_SCHEDULE_MAX_LINKS + 2*MASTER_NUM_FLOWS + 3) + 5*MASTER_NUM_FLOWS + 2) / MASTER_MSG_LENGTH)
#else
#define MAX_PACKETS_PER_SCHEDULE ((NUM_COOJA_NODES * (4*TSCH_SCHEDULE_MAX_LINKS + 2*MASTER_NUM_FLOWS + 3) + 5*MASTER_NUM_FLOWS + 2) / MASTER_MSG_LENGTH)
#endif

#if MAX_PACKETS_PER_SCHEDULE % 32 != 0
  uint32_t received_packets_as_bit_array[(MAX_PACKETS_PER_SCHEDULE / 32) + 1];
#else
  uint32_t received_packets_as_bit_array[MAX_PACKETS_PER_SCHEDULE / 32];
#endif

/*Deployment node count*/
#if TESTBED == TESTBED_COOJA
  static const uint8_t deployment_node_count = NUM_COOJA_NODES;
  master_tsch_schedule_t schedules[NUM_COOJA_NODES] = {0};
#if MAX_PACKETS_PER_SCHEDULE % 32 != 0
  uint32_t metric_received[(MAX_PACKETS_PER_SCHEDULE / 32) + 1];
#else
  uint32_t metric_received[MAX_PACKETS_PER_SCHEDULE / 32];
#endif
  //uint8_t metric_received[NUM_COOJA_NODES] = {0};
#elif TESTBED == TESTBED_FLOCKLAB
  static const uint8_t deployment_node_count = 27;
  master_tsch_schedule_t schedules[27] = {0};
  uint32_t metric_received[27] = {0};
#elif TESTBED == TESTBED_KIEL
  static const uint8_t deployment_node_count = 20;
  master_tsch_schedule_t schedules[20] = {0};
#if MAX_PACKETS_PER_SCHEDULE % 32 != 0
  uint32_t metric_received[MAX_PACKETS_PER_SCHEDULE / 32]; 
#else
  uint32_t metric_received[(MAX_PACKETS_PER_SCHEDULE / 32) + 1]; 
#endif
  //uint8_t metric_received[21] = {0}; //node 11 is missing and instead node 21 exists
#elif TESTBED == TESTBED_DESK
  static const uint8_t deployment_node_count = 5;
  master_tsch_schedule_t schedules[5] = {0};
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

static uint8_t COMMAND_END = 1;

static hash_table_t map_packet_to_schedule_id;
static hash_table_t map_packet_to_last_byte_written;

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
//static uint8_t schedule_complete = 0;

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
char test_out[200];
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

void start_schedule_installation_timer()
{
  //The time passed since the asn for the start was calculated
  // struct tsch_asn_t temp = schedule_config.created_as_asn;
  // int time_passed = TSCH_ASN_DIFF(tsch_current_asn, schedule_config.created_as_asn);
  // temp.ls4b = time_passed;
  int start_offset = TSCH_ASN_DIFF(schedule_config.offset_as_asn, tsch_current_asn);
  LOG_ERR("time passed since the schedule was calculated %d  and offset in asn when to start %d (time %lu ms)\n", 
  (int) schedule_config.created_as_asn.ls4b, start_offset, (CLOCK_SECOND * start_offset * (tsch_timing[tsch_ts_timeslot_length / 1000])) / 1000);
  ctimer_set(&install_schedule_timer, (CLOCK_SECOND * start_offset * (tsch_timing[tsch_ts_timeslot_length] / 1000)) / 1000, install_schedule, NULL);
}

void setup_config_asns()
{
  schedule_config.created_as_asn = tsch_current_asn;
  schedule_config.offset_as_asn = tsch_current_asn;
  //100 cycles * node amount
  TSCH_ASN_INC(schedule_config.offset_as_asn, 500*5);
}

int add_start_asn(int offset)
{
  mrp.data[offset] = schedule_config.created_as_asn.ms1b;
  offset++;
  WRITE32(&mrp.data[offset], schedule_config.created_as_asn.ls4b);
  offset += 4;

  mrp.data[offset] = schedule_config.offset_as_asn.ms1b;
  offset++;
  WRITE32(&mrp.data[offset], schedule_config.offset_as_asn.ls4b);
  offset += 4;

  LOG_ERR("written asn %d, %d\n", (int) schedule_config.created_as_asn.ls4b, (int) schedule_config.offset_as_asn.ls4b);

  return offset;
}

uint8_t fill_universal_config(int offset)
{
  //TODO:: what if universal config is bigger than 1 packet?
  mrp.data[offset] = schedule_config.schedule_length;
  mrp.data[offset + 1] = schedule_config.slot_frames;
  offset += 2;
  memcpy(&(mrp.data[4]), schedule_config.sender_of_flow, MASTER_NUM_FLOWS);
  offset +=8;
  memcpy(&(mrp.data[4 + MASTER_NUM_FLOWS]), schedule_config.receiver_of_flow, MASTER_NUM_FLOWS);
  offset +=8;
  memcpy(&(mrp.data[4 + 2*MASTER_NUM_FLOWS]), schedule_config.first_tx_slot_in_flow, MASTER_NUM_FLOWS);
  offset +=8;
  memcpy(&(mrp.data[4 + 3*MASTER_NUM_FLOWS]), schedule_config.last_tx_slot_in_flow, MASTER_NUM_FLOWS);
  offset +=8;

  offset = add_start_asn(offset);

  return offset;
}

uint8_t fill_packet(int bytes_in_packet, int packet_number)
{
  printf("Start writting at: %d\n", bytes_in_packet);
  //Keep track of the schedule index and the last byte that was written in 2 hash maps
  //This is required for retransmission without calculating all packets
  hash_map_insert(&map_packet_to_schedule_id, packet_number, last_schedule_id_started);
  hash_map_insert(&map_packet_to_last_byte_written, packet_number, last_byte_filled);

  if(last_schedule_id_started != deployment_node_count || last_byte_filled != 0)
  {
    //Always send the current id and the last byte that was written in this id in the beginning of the schedule
    mrp.data[bytes_in_packet] = last_schedule_id_started;             //schedule id where unpacking starts again
    mrp.data[bytes_in_packet + 1] = (last_byte_filled >> 8) & 0xff;    //where writting data was stoped at last iteration in the last schedule id
    mrp.data[bytes_in_packet + 2] = last_byte_filled & 0xff; 
    bytes_in_packet += 3;
    int offset = 0;
    //The total schedule length
    uint16_t schedule_len = 2 + 2 * MASTER_NUM_FLOWS + 1 + schedules[last_schedule_id_started].links_len * sizeof(scheduled_link_t);
    while(last_schedule_id_started < deployment_node_count)
    {
      //If the packet does not even hold 1 byte of the next part of a schedule, finish here
      if((MASTER_MSG_LENGTH - bytes_in_packet) < 2)
      {
        break;
      }

      //Remove the bytes that were already send in an earliert packet
      schedule_len -= last_byte_filled;

      //add the length to the schedule to the packet
      mrp.data[bytes_in_packet] = (schedule_len >> 8) & 0xff; 
      mrp.data[bytes_in_packet + 1] = schedule_len & 0xff; 
      bytes_in_packet += 2;

      //The first cas is where a whole schedule for a node fits into the packet. The second case is only a part of the schedule fits in this packet
      if((MASTER_MSG_LENGTH - bytes_in_packet) >= schedule_len)
      {

        memcpy(&mrp.data[bytes_in_packet], ((uint8_t *)&schedules[last_schedule_id_started]) + last_byte_filled, schedule_len);
        int i;
        for(i = 0; i < schedule_len; i++)
        {
          offset += sprintf(&test_out[offset], "%i", ((uint8_t *)&schedules[last_schedule_id_started] + last_byte_filled)[i]);
        }
        printf("written %d; id %d; %s\n", mrp.data[1], last_schedule_id_started, test_out);
        memset(test_out, 0, 200);
        offset = 0;
        printf("Packet contains whole schedule for id:%d. Wrote bytes from %d to %d (total %d)\n", last_schedule_id_started, last_byte_filled, last_byte_filled + schedule_len, schedule_len);
        printf("Packet contains %d links \n", schedules[last_schedule_id_started].links_len);
        bytes_in_packet += schedule_len;
        last_schedule_id_started++;
        last_byte_filled = 0;
        schedule_len = 2 + 2 * MASTER_NUM_FLOWS + 1 + schedules[last_schedule_id_started].links_len * sizeof(scheduled_link_t);

      }else{
        //if the length of the packet does not fit, leave the rest of the packet empty
        memcpy(&(mrp.data[bytes_in_packet]), (uint8_t *)&schedules[last_schedule_id_started] + last_byte_filled, (MASTER_MSG_LENGTH - bytes_in_packet));
        int i;
        for(i = 0; i < MASTER_MSG_LENGTH - bytes_in_packet; i++)
        {
          offset += sprintf(&test_out[offset], "%i", ((uint8_t *)&schedules[last_schedule_id_started] + last_byte_filled)[i]);
        }
        
        printf("written %d; id %d; %s\n", mrp.data[1], last_schedule_id_started, test_out);
        memset(test_out, 0, 200);
        offset = 0;
        printf("Packet contains part of schedule for id:%d. Wrote bytes from %d to %d (total %d)\n", last_schedule_id_started, last_byte_filled, last_byte_filled + (MASTER_MSG_LENGTH - bytes_in_packet), (MASTER_MSG_LENGTH - bytes_in_packet));
        printf("Packet contains %d links \n", schedules[last_schedule_id_started].links_len);
        last_byte_filled = (MASTER_MSG_LENGTH - bytes_in_packet);
        bytes_in_packet += last_byte_filled;
        break;
      }
    }
    printf("Packet contains out\n");
  }

  masternet_len = bytes_in_packet + minimal_routing_packet_size;

  printf("Packet bytes sending %d\n", masternet_len - minimal_routing_packet_size);
  #if TSCH_TTL_BASED_RETRANSMISSIONS
  set_ttl_retransmissions();
  #endif

  return last_schedule_id_started == deployment_node_count && last_byte_filled == 0;
}

int unpack_asn(int offset)
{
  LOG_ERR("Packet contains asn\n");
  schedule_config.created_as_asn.ms1b = mrp.data[offset];
  offset++;
  READ32(&mrp.data[offset], schedule_config.created_as_asn.ls4b);
  offset += 4;

  schedule_config.offset_as_asn.ms1b = mrp.data[offset];
  offset++;
  READ32(&mrp.data[offset], schedule_config.offset_as_asn.ls4b);
  offset += 4;
  LOG_ERR("packet asn %d, current asn %d and to start %d\n", (int) schedule_config.created_as_asn.ls4b, (int) tsch_current_asn.ls4b, (int) schedule_config.offset_as_asn.ls4b);

  return offset;
}

void unpack_packet(int packet_len)
{
  int offset = 0;
  memset(test_out, 0, 200);

  int unpack_at_index = 2; //0 = command, 1 = packet number, >= 2 data
  //printf("Packet bytes receiving %d\n", packet_len);
  if(mrp.data[1] == 1)
  {
    schedule_config.schedule_length = mrp.data[2];
    schedule_config.slot_frames = mrp.data[3];
    memcpy(schedule_config.sender_of_flow, &(mrp.data[4]), MASTER_NUM_FLOWS);
    memcpy(schedule_config.receiver_of_flow, &(mrp.data[4 + MASTER_NUM_FLOWS]), MASTER_NUM_FLOWS);
    memcpy(schedule_config.first_tx_slot_in_flow, &(mrp.data[4 + 2*MASTER_NUM_FLOWS]), MASTER_NUM_FLOWS);
    memcpy(schedule_config.last_tx_slot_in_flow, &(mrp.data[4 + 3*MASTER_NUM_FLOWS]), MASTER_NUM_FLOWS);
    unpack_at_index += 2 + 4*MASTER_NUM_FLOWS;

    unpack_at_index = unpack_asn(unpack_at_index);

    //printf("Packet contained universal config. unpack %d bytes \n", unpack_at_index);
  }

  uint8_t schedule_index = mrp.data[unpack_at_index];
  unpack_at_index++;
  uint16_t last_byte_written = mrp.data[unpack_at_index] << 8;
  last_byte_written += mrp.data[unpack_at_index + 1];
  unpack_at_index += 2;
  //printf("unpacked bytes %d\n", unpack_at_index);

  //Keep track of the schedule index and the last byte that was written in 2 has maps
  //This is required for retransmission without calculating all packets
  hash_map_insert(&map_packet_to_schedule_id, mrp.data[1], schedule_index);
  hash_map_insert(&map_packet_to_last_byte_written, mrp.data[1], last_byte_written);

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
      int i;
      for(i = 0; i < schedule_len; i++)
      {
        offset += sprintf(&test_out[offset], "%i", ((uint8_t *)&schedules[schedule_index] + last_byte_written)[i]);
      }
      //printf("packet %d; id %d; %s\n", mrp.data[1], schedule_index, test_out);
      memset(test_out, 0, 200);
      offset = 0;
      //printf("Packet contains full schedule to unpack for schedule %d. start from %d to %d (totall %d)\n", schedule_index, unpack_at_index, unpack_at_index + schedule_len, schedule_len);
      schedule_index++;
      unpack_at_index += schedule_len;
      last_byte_written = 0;

    }else{
      //in case this is a part of a packet, keep track how many bytes to read for the next packet.
      //remaining_len_last_packet = (unpack_at_index + schedule_len) - packet_len;
      //Unpack the remaining bytes into the schedule.
      memcpy((uint8_t *)&schedules[schedule_index] + last_byte_written, &(mrp.data[unpack_at_index]), packet_len - unpack_at_index);
      int i;
      for(i = 0; i < packet_len - unpack_at_index; i++)
      {
        offset += sprintf(&test_out[offset], "%i", ((uint8_t *)&schedules[schedule_index + last_byte_written])[i]);
      }
      //printf("packet %d; id %d; %s\n", mrp.data[1], schedule_index, test_out);
      memset(test_out, 0, 200);
      offset = 0;
      //printf("Packet contains part of the schedule to unpack for schedule %d. start from %d to %d (totall %d)\n", schedule_index, unpack_at_index, unpack_at_index + packet_len - unpack_at_index, packet_len - unpack_at_index);
      unpack_at_index += packet_len - unpack_at_index;
    }
  }
}

void master_schedule_loaded_callback()
{
  LOG_ERR("Schedule loaded");
  //As the Coordinator, the schedule is already complete after we arrive at this callback
  //schedule_complete = 1;
  //deactivate EB since EB packets are a higher priority and will block the distribution of the schedule
  tsch_eb_active = 0;
  //Enter state for schedule distribution
  current_state = ST_SCHEDULE_DIST;
  //TODO:: fix this later to max_tx = worst etx of all nbrs
  sent_packet_configuration.max_tx = 1;
  //Signal for receiver, that this is not the last packet
  sent_packet_configuration.command = CM_SCHEDULE;

  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  mrp.data[0] = CM_SCHEDULE;
  mrp.data[1] = schedule_packet_number;
  setBit(received_packets_as_bit_array, schedule_packet_number);

  //Setup the asn once the schedule distribution starts
  setup_config_asns();

  //Fill the first packet with the universal config
  fill_packet(fill_universal_config(2), schedule_packet_number);

  //Start the timer for the CPAN
  start_schedule_installation_timer();
  
  NETSTACK_NETWORK.output(&tsch_broadcast_address);
}

void install_schedule(){
  LOG_INFO("Install schedule at asn %d\n", (int)tsch_current_asn.ls4b);
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
void prepare_forward_config(uint8_t etx_link, uint8_t command, uint16_t packet_number, uint8_t important_packet)
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
  sent_packet_configuration.packet_nbr = packet_number;
  sent_packet_configuration.important_packet = important_packet;
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

//Depending on the state, send packets to time_source or poll a neighbor for their metric
void handle_convergcast(int repeat)
{
  if (current_state == ST_POLL_NEIGHBOUR)
  {
    // Prepare packet for get metric command
    mrp.flow_number = node_id;
    mrp.packet_number = ++own_packet_number;
    mrp.data[0] = CM_GET_ETX_METRIC;
    masternet_len = minimal_routing_packet_size + sizeof(uint8_t);

    prepare_forward_config(next_dest->etx_link, CM_GET_ETX_METRIC, mrp.packet_number, 0);

    prepare_link_for_metric_distribution(&next_dest->addr, node_id - 1);

    LOG_ERR("Sending POLL request to %u with size %d\n", next_dest->addr.u8[NODE_ID_INDEX], masternet_len);
    NETSTACK_NETWORK.output(&next_dest->addr);
  }else
  {
    prepare_forward_config(tsch_queue_get_time_source()->etx_link, CM_ETX_METRIC, mrp.packet_number, 0);

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
    LOG_ERR("This is my nbr with id %d, source %d etx-link %d\n", next_dest->addr.u8[NODE_ID_INDEX], next_dest->time_source, next_dest->etx_link);
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
    if(nbr->missed_ebs - nbr->last_eb == 0)
    {
      nbr = tsch_queue_next_nbr(nbr);
      continue;
    }

    int etx_link = 100 * (1.0 / (1.0 - ((float)nbr->missed_ebs / nbr->last_eb)));

    uint8_t round = 0;
    //LOG_INFO("current etx_link =%d, mb %d; lb %d; fb %d\n", current_etx_link, n->missed_ebs, n->last_eb, n->first_eb);
    if(etx_link % 10 > 0)
    {
      round = 1;
    }

    if((etx_link / 10) > 255)
    {
      etx_link = 255;
    }else{
      etx_link = (etx_link / 10) + round;
    }

    etx_links[pos] = (uint8_t)nbr->addr.u8[NODE_ID_INDEX];
    etx_links[pos + 1] = (uint8_t)etx_link;

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
    setBit(metric_received, node_id - 1);
    current_state = ST_POLL_NEIGHBOUR;
    LOG_INFO("Have %d nbrs\n", tsch_queue_count_nbr());
    next_dest = tsch_queue_first_nbr();
    while (next_dest != NULL && next_dest->time_source != node_id)
    {
      next_dest = tsch_queue_next_nbr(next_dest);
    }
    
    handle_convergcast(0);
  }else{
    LOG_ERR("My time src is %d, my rank %d, with etx %d and nbr size is %d with %d flows\n", tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX], tsch_rank, tsch_queue_get_time_source()->etx_link, TSCH_QUEUE_MAX_NEIGHBOR_QUEUES, MASTER_NUM_FLOWS);
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

void handle_retransmit()
{
  //Check if there are other neighbors containing packets for retransmitting
  struct tsch_neighbor* nbr = tsch_queue_first_nbr();
  LOG_ERR("Check for more nbrs\n");

  while(nbr != NULL && tsch_queue_packet_count(&nbr->addr) == 0)
  {
    nbr = tsch_queue_next_nbr(nbr);
  }

  if(nbr != NULL && tsch_queue_packet_count(&nbr->addr) != 0)
  {
    current_state = ST_SCHEDULE_RETRANSMITTING;
    LOG_ERR("Found another nbr that waits for a retransmit %d\n", nbr->addr.u8[NODE_ID_INDEX]);
    if(linkaddr_cmp(&tsch_schedule_get_link_by_timeslot(sf[1], node_id - 1)->addr, &nbr->addr) == 0)
    {
      tsch_schedule_remove_link_by_timeslot(sf[1], node_id - 1);
      tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &nbr->addr, node_id - 1, 0);
    }
  }else{
    LOG_ERR("Found no nbr for retransmits. Start sending beacons again\n");
    current_state = ST_SCHEDULE_INSTALLED;
    tsch_eb_active = 1;
    tsch_schedule_remove_link_by_timeslot(sf[1], node_id - 1);
    tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, node_id - 1, 0);
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

  //In a retransmit scenario, we already found out how many packets we need to have
  int missing_packet = getMissingPacket(received_packets_as_bit_array, schedule_packets);

  if(missing_packet == -1)
  { 
    //Finished, activate EB again
    tsch_eb_active = 1;
    //We are finished. increment the version and go into finished state
    current_state = ST_SCHEDULE_INSTALLED;
    schedule_version++;
    LOG_ERR("Received all packets. Version %d with %d packets\n", schedule_version, schedule_packets);

    tsch_schedule_remove_link_by_timeslot(sf[1], node_id - 1);
    tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, node_id - 1, 0);
  }else{
    //TODO:: check if the current retransmit packet is already in the queue
    //Prepare the packets for output
    mrp.flow_number = node_id;
    mrp.packet_number = ++own_packet_number;
    mrp.data[0] = CM_SCHEDULE_RETRANSMIT_REQ;
    mrp.data[1] = missing_packet;

    prepare_forward_config(tsch_queue_get_nbr(destination)->etx_link, CM_SCHEDULE_RETRANSMIT_REQ, mrp.data[1], 0);

    masternet_len = minimal_routing_packet_size + 2;

    LOG_ERR("Requesting retransmit from %d for packet %d with size %d\n", destination->u8[NODE_ID_INDEX], missing_packet, masternet_len);

    NETSTACK_NETWORK.output(destination);
  }
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

void handle_divergcast_callback(packet_data_t *packet_data, int ret,int transmissions)
{
  //This will only happen in unicast -> retransmit/retransmit_request
  if (ret != MAC_TX_OK)
  {
    int ie_offset = 0;
    if(packetbuf_attr(PACKETBUF_ATTR_MAC_METADATA))
    {
      printf("Callback for a packet with IE fields\n");
      ie_offset = 9;
    }
    memset(&mrp, 0, sizeof(mrp));
    memcpy(&mrp, packetbuf_dataptr() + packet_data->hdr_len + ie_offset, packetbuf_datalen() - packet_data->hdr_len - ie_offset);
    LOG_ERR("MRP command %s, packet %d, trans %d; size %d (hdr %d vs packet %d) and ie_offset %d\n", mrp.data[0] == CM_SCHEDULE_RETRANSMIT ? "Retransmit" : "Request", 
    mrp.data[1], transmissions, packetbuf_datalen(), packetbuf_hdrlen(), packet_data->hdr_len, ie_offset);
    linkaddr_t dest_resend = *packetbuf_addr(PACKETBUF_ADDR_RECEIVER);

    //Small optimization: in case we did not receive an ACK but the nbr send us already the requestet packet, drop the request
    if(mrp.data[0] == CM_SCHEDULE_RETRANSMIT_REQ && isBitSet(received_packets_as_bit_array, mrp.data[1]))
    {
      LOG_ERR("We already have this %d packet. Dropp request for it\n", mrp.data[1]);
      request_retransmit_or_finish(&dest_resend); 
      return;
    }
    else //if(mrp.data[0] != CM_SCHEDULE_RETRANSMIT_REQ || isBitSet(received_packets_as_bit_array, mrp.data[1]) == 0)
    {
      uint8_t important_packet = mrp.data[0] == CM_SCHEDULE_RETRANSMIT ? 1 : 0;
      prepare_forward_config(tsch_queue_get_nbr(&dest_resend)->etx_link, mrp.data[0], mrp.data[1], important_packet);
      masternet_len = packetbuf_datalen() - packet_data->hdr_len - ie_offset;
      NETSTACK_NETWORK.output(&dest_resend);
      return;
    }
  }

  if(packet_data->command == CM_SCHEDULE)
  {
    if(tsch_is_coordinator)
    {
      LOG_INFO("Callback SCHEDULE\n");
      schedule_packet_number++;
      mrp.flow_number = node_id;
      mrp.packet_number = ++own_packet_number;
      mrp.data[0] = CM_SCHEDULE;
      mrp.data[1] = schedule_packet_number;
      setBit(received_packets_as_bit_array, schedule_packet_number);

      if(fill_packet(2, schedule_packet_number))
      {
        //Once the schedule is broadcastet completly, start sending EB's with new version and packet number
        LOG_ERR("INC SCHED from last packet sent as cpan. Schedule finished\n");
        current_state = ST_SCHEDULE_INSTALLED;
        schedule_version++;
        schedule_packets = schedule_packet_number;

        mrp.data[0] = CM_SCHEDULE_END;
        sent_packet_configuration.command = CM_SCHEDULE_END;
      }

      NETSTACK_NETWORK.output(&tsch_broadcast_address);
    }else{
      LOG_ERR("Nothing to do after resending a schedule as a normal node\n");
    }
    return;
  }

  //last packet for the schedule was sent. now we only need to react to retransmits
  if(packet_data->command == CM_SCHEDULE_END)
  {
    if(getMissingPacket(received_packets_as_bit_array, schedule_packets) == -1)
    {
      tsch_eb_active = 1;
    }else{
      LOG_ERR("Schedule not finished\n");
    }
  }  

  if(packet_data->command == CM_SCHEDULE_RETRANSMIT_REQ)
  {
    //In case we received already a request since we have all the packets
    if(current_state == ST_SCHEDULE_RETRANSMITTING)
    {
      handle_retransmit();
    }else if(current_state == ST_SCHEDULE_OLD){
      //If we received the packet already, search the next missing packet. otherwise wait for response.
      if(isBitSet(received_packets_as_bit_array, packet_data->packet_nbr))
      {
        request_retransmit_or_finish(packetbuf_addr(PACKETBUF_ADDR_RECEIVER)); 
      }
    }
  }

  if(packet_data->command == CM_SCHEDULE_RETRANSMIT)
  {
    handle_retransmit();
  }
}

/*---------------------------------------------------------------------------*/
void master_schedule_difference_callback(linkaddr_t * nbr, uint8_t nbr_schedule_version, uint16_t nbr_schedule_packets)
{
  //Dont get triggered by every beacon to start packet requesting
  if(current_state == ST_SCHEDULE_OLD)
  {
    LOG_ERR("Already searching for packets\n");
    return;
  }

  if(nbr_schedule_version < schedule_version)
  {
    LOG_ERR("Lower schedule from a nbr detected (%d %d)\n", nbr_schedule_version, schedule_version);
  }else{
    LOG_ERR("Higher schedule from a nbr detected (%d %d)\n", nbr_schedule_version, schedule_version);
    current_state = ST_SCHEDULE_OLD;
    tsch_eb_active = 0;

    //Remember how many packets the new version of the schedule has.
    schedule_packets = nbr_schedule_packets;
    int missing_packet = getMissingPacket(received_packets_as_bit_array, schedule_packets);

    if(missing_packet == -1)
    {
      //TODO:: in this case we got all packets. we just did not know `?
      LOG_ERR("why????\n");
    }
    LOG_ERR("NBR packets = %d\n", nbr_schedule_packets);
    mrp.flow_number = node_id;
    mrp.packet_number = ++own_packet_number;
    mrp.data[0] = CM_SCHEDULE_RETRANSMIT_REQ;
    mrp.data[1] = missing_packet;
    prepare_forward_config(tsch_queue_get_nbr(nbr)->etx_link, CM_SCHEDULE_RETRANSMIT_REQ, mrp.data[1], 0);
    prepare_link_for_metric_distribution(nbr, node_id - 1);

    masternet_len = minimal_routing_packet_size + 2;
    LOG_ERR("Requesting retransmit from %d for packet %d with size %d\n", nbr->u8[NODE_ID_INDEX], missing_packet, masternet_len);
    NETSTACK_NETWORK.output(nbr);
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

  //Handle the metric gathering callback once a packet is sent or an error on sent occured
  if(packet_data->command >= CM_GET_ETX_METRIC && packet_data->command <= CM_ETX_METRIC)
  {
    handle_covergcast_callback(packet_data, ret, transmissions);
  }

  //Handle the metric distribution callback once a packet is sent or an error on sent occured
  if(packet_data->command >= CM_SCHEDULE && packet_data->command <= CM_SCHEDULE_END)
  {
    handle_divergcast_callback(packet_data, ret, transmissions);
  }
}
/*---------------------------------------------------------------------------*/
void master_routing_input(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
  LOG_ERR("Received len = %d (min %d, max %d)\n", len, minimal_routing_packet_size, maximal_routing_packet_size);
  if (len >= minimal_routing_packet_size && len <= maximal_routing_packet_size)
  {
    uint8_t forward_to_upper_layer = 0;
    memcpy(&mrp, data, len);
    int command = mrp.data[0];

    if(is_configured == 0)
    {

      if(command == CM_GET_ETX_METRIC)
      {
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
      }

      if(command == CM_ETX_METRIC)
      {
        //In case we received the metric from our neighbor and he did not receive our ACK, he will resend. Drop the packet here
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
          setBit(metric_received, mrp.flow_number - 1);
          //metric_received[ mrp.flow_number - 1] = 1;
          print_metric(&mrp.data[COMMAND_END], mrp.flow_number, len - minimal_routing_packet_size - COMMAND_END); //-3 for the mrp flow number and packet number and -command length
          int i;
          int finished_nodes = 0;
          char test[100];
          int offset = 0;
        #if TESTBED == TESTBED_KIEL
          for(i = 0; i < deployment_node_count + 1; i++)
          {
            if(isBitSet(metric_received, i))
            {
              finished_nodes++;
            }else{
             offset += sprintf(&test[offset], "%i, ", i + 1);
            }
          }
        #else
          for(i = 0; i < deployment_node_count; i++)
          {
            if(isBitSet(metric_received, i))
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
            prepare_forward_config(tsch_queue_get_time_source()->etx_link, CM_ETX_METRIC, mrp.packet_number, 0);
            NETSTACK_NETWORK.output(&tsch_queue_get_time_source()->addr);
          }
        }
      }

      //During the first transmit from the CPAN, this commands are send.
      if(command == CM_SCHEDULE || command == CM_SCHEDULE_END)
      { 
        if(isBitSet(received_packets_as_bit_array, mrp.data[1]) == 0)
        {
          LOG_ERR("---------------- Packet Nr.%d \n", mrp.data[1]);

          //We start forwarding the schedule and enter the state for it
          current_state = ST_SCHEDULE_DIST;

          //deactive enhanced beacons during metric distribution
          tsch_eb_active = 0;

          //Check the packets in the bit vector to later find missing packets
          setBit(received_packets_as_bit_array, mrp.data[1]);

          //Unpack into the schedule structure
          unpack_packet(len - minimal_routing_packet_size);

          if(schedule_packets < mrp.data[1])
          {
            schedule_packets = mrp.data[1];
          }

          //The first packet contains the univeral config and therefore we can start the timer when the switch to the new schedule happens
          if(mrp.data[1] == 1)
          {
            start_schedule_installation_timer();
          }

          //Receiving the last packet triggers a check if this node has a complete schedule
          if(command == CM_SCHEDULE_END)
          {
            //Check if a packet is missing
            int missing_packet = getMissingPacket(received_packets_as_bit_array, schedule_packets);

            //If all packets are received, set the schedule version and the state. Active EB's again to signal the new schedule version
            if(missing_packet == -1)
            { 
              //We are finished. increment the version and go into finished state
              LOG_ERR("INC SCHED since all packets were received\n");
              current_state = ST_SCHEDULE_INSTALLED;
              schedule_version++;
              LOG_ERR("Received all packets. Version %d with %d packets\n", schedule_version, schedule_packets);
              //Finished, activate EB again
              tsch_eb_active = 1;
            }else{
              LOG_ERR("Missing some packets. wait for beacons\n");
            }
            tsch_change_time_source_active = 1;
          }

          #if TSCH_TTL_BASED_RETRANSMISSIONS
          set_ttl_retransmissions();
          #endif

          //Prepare the packets for output
          sent_packet_configuration.max_tx = 1;
          sent_packet_configuration.command = command;

          masternet_len = len;
          NETSTACK_NETWORK.output(&tsch_broadcast_address);
        }else{
          LOG_ERR("Skip schedule packet %d\n", mrp.data[1]);
        }
      }

      //Retransmit received. only apply packet if this node misses the packet
      if(command == CM_SCHEDULE_RETRANSMIT)
      {
        //Get the schedule packet number
        int retransmitted_packet = mrp.data[1];

        //Only look at packets if we know that we miss some packets from the final schedule
        if(current_state == ST_SCHEDULE_OLD)
        {
          //While parsing and retransmitting, the src addr becomes null. Save the address localy
          const linkaddr_t source_addr = *src;

          //If we miss this packet, add the packet to the schedule
          if(isBitSet(received_packets_as_bit_array, retransmitted_packet) == 0)
          {
            LOG_ERR("---------------- Packet Retransmit Nr.%d \n", retransmitted_packet);
            //Mark the packet in the bit vector as received
            setBit(received_packets_as_bit_array, retransmitted_packet);

            //Unpack into the schedule structure
            unpack_packet(len - minimal_routing_packet_size);

            //The first packet contains the univeral config and therefore we can start the timer when the switch to the new schedule happens
            if(mrp.data[1] == 1)
            {
              start_schedule_installation_timer();
            }
          }

          //If this packet was send to us, check if other packets are missing and start the next request
          if(linkaddr_cmp(&linkaddr_node_addr, dest))
          {
            request_retransmit_or_finish(&source_addr); 
          }else{
            //In case this packet was received but was for another node, dont request the next missing packet
            LOG_ERR("Skip request for next packet: packet was not for me!\n");
          }
        }
      }

      //If we received a request to retransmit a packet, start generating the missing packet and send it
      if(command == CM_SCHEDULE_RETRANSMIT_REQ)
      {
        //While parsing and retransmitting, the src addr becomes null. Save the address localy
        const linkaddr_t source_addr = *src;

        LOG_ERR("Received request to retransmit packet %d from %d\n", mrp.data[1], source_addr.u8[NODE_ID_INDEX]);

        /* Small optimization: Due to missing ACK's, a node that sends requests to us for retransmit might send the same 
         * request multiple times. Since only 1 request by each node will be send at a time, check if the queue for the requester contains the
         * packet that was requestet.
        */
        if(tsch_queue_is_packet_in_nbr_queue(tsch_queue_get_nbr(&source_addr), mrp.data[1]))
        {
          LOG_ERR("Packet already in Queue\n");
          return;
        }

        //We received a request to retransmit a packet. enter the retransmit state and prepare the packet.
        mrp.flow_number = node_id;
        mrp.packet_number = ++own_packet_number;
        mrp.data[0] = CM_SCHEDULE_RETRANSMIT;
        current_state = ST_SCHEDULE_RETRANSMITTING;
        int missing_packet = mrp.data[1];
        tsch_eb_active = 0;

        //Get the schedule index and the offset for the requestet packet to avoid calculating from the start
        last_schedule_id_started = hash_map_lookup(&map_packet_to_schedule_id, missing_packet);
        last_byte_filled = hash_map_lookup(&map_packet_to_last_byte_written, missing_packet);

        //In case of the first packet missing, send the universal config with it.
        if(missing_packet == 1)
        {
          fill_packet(fill_universal_config(2), missing_packet);
        }else{
          fill_packet(2, missing_packet);
        }

        prepare_forward_config(tsch_queue_get_nbr(&source_addr)->etx_link, CM_SCHEDULE_RETRANSMIT, mrp.data[1], 1);

        LOG_ERR("Add a packet for %d. total packets %d\n", source_addr.u8[NODE_ID_INDEX], tsch_queue_global_packet_count());

        NETSTACK_NETWORK.output(&source_addr);
        
        //If we receive requests to retransmit parts of the schedule, we add them to the queue of the neighbor
        //When changing the link to a requester for a retransmit, check if we are looking at the broadcast address:
        //If broadcast address -> this is the first request for a retransmit. Change to requester and the packet will be sent
        //If not broadcast address -> we already send a retransmit to another nbr. Link change will be perfmored later in the callback
        if(linkaddr_cmp(&tsch_schedule_get_link_by_timeslot(sf[1], node_id - 1)->addr, &tsch_broadcast_address) != 0)
        {
          LOG_ERR("This is the first request, change to requester\n");
          tsch_schedule_remove_link_by_timeslot(sf[1], node_id - 1);
          tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &source_addr, node_id - 1, 0);
        }else{
          LOG_ERR("This is a difference in addresses %d vs %d\n", tsch_schedule_get_link_by_timeslot(sf[1], node_id - 1)->addr.u8[NODE_ID_INDEX], tsch_broadcast_address.u8[NODE_ID_INDEX]);
        }
      }

    }else{
      if(command != CM_DATA)
      {
        return;
      }
      //LOG_ERR("Received %d bytes with command %u\n", len, command);
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
    LOG_INFO("Generate beacon every %i ms\n", ((CLOCK_SECOND * ((TSCH_DEFAULT_TS_TIMESLOT_LENGTH / 1000) * deployment_node_count)) / 1000));
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
    tsch_set_schedule_difference_callback(master_schedule_difference_callback);

    tsch_schedule_remove_all_slotframes();
  #if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
    uint32_t array_size;
    if(MAX_PACKETS_PER_SCHEDULE % 32 != 0)
    {
      array_size = (MAX_PACKETS_PER_SCHEDULE / 32) + 1;
    }else{
      array_size = MAX_PACKETS_PER_SCHEDULE / 32;
    }

    memset(received_packets_as_bit_array, 0, array_size * sizeof(uint32_t));
    memset(metric_received, 0, array_size * sizeof(uint32_t));

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
