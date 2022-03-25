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
#define MAX_PACKETS_PER_SCHEDULE (((NUM_COOJA_NODES + 1) * (4*TSCH_SCHEDULE_MAX_LINKS + 2*MASTER_NUM_FLOWS + 3) + 5*MASTER_NUM_FLOWS + 7) / MASTER_MSG_LENGTH)
#else 
#define MAX_PACKETS_PER_SCHEDULE ((NUM_COOJA_NODES * (4*TSCH_SCHEDULE_MAX_LINKS + 2*MASTER_NUM_FLOWS + 3) + 5*MASTER_NUM_FLOWS + 7) / MASTER_MSG_LENGTH)
#endif


int miss_array[4] = {16, 3, 15 , 7};
uint8_t first_miss = 1;

//The bit-array to mark received packets
#if MAX_PACKETS_PER_SCHEDULE % 32 != 0
  uint32_t received_packets_as_bit_array[(MAX_PACKETS_PER_SCHEDULE / 32) + 1];
#else
  uint32_t received_packets_as_bit_array[MAX_PACKETS_PER_SCHEDULE / 32];
#endif

//Indicator for the end of the universal config as a packet number
uint8_t end_of_universal_config = 0;
//Indicator if a schedule was installed succesfully
uint8_t schedule_installed = 0;

/*Deployment node count*/
#if TESTBED == TESTBED_COOJA
  static const uint8_t deployment_node_count = NUM_COOJA_NODES;

  //The schedule will be transfered between nodes and used to switch to a new schedule
  master_tsch_schedule_t schedules[NUM_COOJA_NODES] = {{0}};

  //Keep track of the received etx-metrics using this bit array
#if MAX_PACKETS_PER_SCHEDULE % 32 != 0
  uint32_t metric_received[(MAX_PACKETS_PER_SCHEDULE / 32) + 1];
#else
  uint32_t metric_received[MAX_PACKETS_PER_SCHEDULE / 32];
#endif
 static const uint8_t last_node_id = NUM_COOJA_NODES;
#elif TESTBED == TESTBED_FLOCKLAB
  static const uint8_t deployment_node_count = 27;
  master_tsch_schedule_t schedules[27] = {0};
  uint32_t metric_received[27] = {0};
#elif TESTBED == TESTBED_KIEL
  static const uint8_t deployment_node_count = 20;
  master_tsch_schedule_t schedules[21] = {{0}};
#if MAX_PACKETS_PER_SCHEDULE % 32 != 0
  uint32_t metric_received[MAX_PACKETS_PER_SCHEDULE / 32]; 
#else
  uint32_t metric_received[(MAX_PACKETS_PER_SCHEDULE / 32) + 1]; 
#endif
  static const uint8_t last_node_id = 21;
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

//The index for the end of the command
static uint8_t COMMAND_END = 1;

/* Keep track of the node id and the last written byte for this node id by a packet number.
 * Without this tables, every time a packet X is requested, the nodes would need to calculate all
 * packets n where 0 <= n < X.
 */
static hash_table_t map_packet_to_schedule_id;
static hash_table_t map_packet_to_last_byte_written;

//Variables used when filling and unpacking the schedule
uint8_t last_schedule_id_started = 0;
uint16_t last_byte_filled = 0;

//This variable is used as an offset to know the beacon slot in case of an installed schedule
uint8_t beacon_offset = 0;

//This variable is used set once a boradcast for a missing metric is receiv
//In order to ignore multiple broadcasts from different nodes, ignore broadcasts for a few seconds usig a timer
uint8_t ignored_missing_metric_requests = 0;
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
uint8_t metric_complete = 0;

//Array used to calculate and save the metric
uint8_t etx_links[(TSCH_QUEUE_MAX_NEIGHBOR_QUEUES - 2) * 2];

//Array used to print the etx-metric for MASTER
#define MAX_CHARS_PER_ETX_LINK 8
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

/*---------------------------------------------------------------------------*/
static void set_destination_link_addr(uint8_t destination_node_id)
{
  destination.u8[NODE_ID_INDEX] = destination_node_id;
}

static void activate_missing_metric_requests()
{
  printf("receive missing metric requests again\n");
  ignored_missing_metric_requests = 0;
}

static double get_percent_of_second(enum tsch_timeslot_timing_elements timing_element)
{
  return (tsch_timing[timing_element] * 1.0) / RTIMER_SECOND;
}

int check_received_schedules_by_nodes()
{
  int i;
  int nodes_to_count = deployment_node_count;
  uint8_t nodes_have_schedule = 0;
#if TESTBED == TESTBED_KIEL
  nodes_to_count++;
#endif
  char missing[100];
  int offset = 0;
  for(i = 0; i < nodes_to_count; i++)
  {
    if(isBitSet1Byte(schedule_received, i))
    {
      nodes_have_schedule++;
      //nodes_have_schedule += isBitSet1Byte(schedule_received, i);
    }else{
      offset += sprintf(&missing[offset], "%i, ", i+1);
    }
    
  }

  //Stop checking for nodes that miss a schedule
  if(nodes_have_schedule == deployment_node_count)
  {
    printf("all nodes have the schedule!\n");
    tsch_reset_schedule_received_callback();

    // sf[0] = tsch_schedule_get_slotframe_by_handle(0);
    // if (sf[0]){
    //   tsch_schedule_remove_slotframe(sf[0]);
    // }
    return 1;
  }else{
    printf("Schedule not installed at: %s\n", missing);
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
  return node_id - 1;
}

/* Prepare the master_packetbuf_config_t for MASTER-NET */
void setup_packet_configuration(uint8_t etx_link, uint8_t command, uint16_t packet_number, uint8_t important_packet)
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

/* Start the schedule installation. The time to start the installation is send in the universal config. 
 * If the node is late, start immediatly.
 */
void start_schedule_installation_timer()
{
  //The time passed since the asn for the start was calculated
  int start_offset = TSCH_ASN_DIFF(schedule_config.start_network_asn, tsch_current_asn);
  //tsch_schedule_print();
  if( start_offset > 0)
  {
    //TODO:: use diferent code to get offset for real devices and cooja
    //LOG_ERR("time when to start the network in asn %d (time %lu ms with ts length = %lu)\n", 
    //start_offset, (CLOCK_SECOND * start_offset * (tsch_timing[tsch_ts_timeslot_length] / 1000)) / 1000, tsch_timing[tsch_ts_timeslot_length]);
    //ctimer_set(&install_schedule_timer, (CLOCK_SECOND * start_offset * (tsch_timing[tsch_ts_timeslot_length] / 1000)) / 1000, install_schedule, NULL);
    LOG_DBG("Time to run before convergcast = %lu ms \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT));
    LOG_DBG("Time to run before convergcast = %lu ticks \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT * CLOCK_SECOND));
    LOG_DBG("time when to start the network in asn %d (time %lu ms with ts length = %lu)\n", 
    start_offset, (uint32_t)(start_offset * CLOCK_SECOND * get_percent_of_second(tsch_ts_timeslot_length)), tsch_timing[tsch_ts_timeslot_length]);
    ctimer_set(&install_schedule_timer, (uint32_t)(start_offset * CLOCK_SECOND * get_percent_of_second(tsch_ts_timeslot_length)), install_schedule, NULL);

  }else{
    LOG_INFO("Already too late, start immediatly\n");
    install_schedule();
  }
}

/* Search for the next node that has a non empty schedule
*/
uint8_t get_next_id(uint8_t last_id)
{
  do
  {
    last_id++;
  }while(schedules[last_id].links_len == 0 && last_id < last_node_id);

  return last_id;
}

/* Setup the asn when the network should switch to the schedule
*/
void setup_config_asns()
{
  schedule_config.start_network_asn = tsch_current_asn;
  uint16_t cycles_for_distribution = 10;
  TSCH_ASN_INC(schedule_config.start_network_asn, cycles_for_distribution*deployment_node_count);
}

/* Write into a buffer as many bytes as possible */
int write_content(void* from, void* to, int start, int length, int end, int packet_number)
{

  int written_bytes = (end - start) >= length ? length : end - start;

  memcpy(to, from, written_bytes);

  return written_bytes;
}

/* Fill the schedule into a packet */
uint8_t fill_schedule_packet(int bytes_in_packet, int packet_number)
{
  int i;
  char string[300] = {0};
  int string_offset = 0;
  //printf("Start writting at: %d\n", bytes_in_packet);
  //Keep track of the schedule index and the last byte that was written in 2 hash maps
  //This is required for retransmission without calculating all packets
  hash_map_insert(&map_packet_to_schedule_id, packet_number, last_schedule_id_started);
  hash_map_insert(&map_packet_to_last_byte_written, packet_number, last_byte_filled);
  printf("Start filling packet %i with schedule id %i and last byte filled %i \n", packet_number, last_schedule_id_started, last_byte_filled);

  if(packet_number <= end_of_universal_config)
  {
    mrp.data[bytes_in_packet] = (last_byte_filled >> 8) & 0xff;    //where writting data was stoped at last iteration in the last schedule id
    mrp.data[bytes_in_packet + 1] = last_byte_filled & 0xff; 
    bytes_in_packet += 2;

    int config_len = sizeof(master_tsch_schedule_universall_config_t) - last_byte_filled;

    //add the length to the schedule to the packet
    mrp.data[bytes_in_packet] = (config_len >> 8) & 0xff; 
    mrp.data[bytes_in_packet + 1] = config_len & 0xff; 
    bytes_in_packet += 2;

    printf("config len %i\n", config_len);

    int written_bytes = write_content(((uint8_t *)&schedule_config) + last_byte_filled, &mrp.data[bytes_in_packet], bytes_in_packet, config_len, MASTER_MSG_LENGTH, packet_number);

    for(i = 0; i < written_bytes; i++)
    {
      string_offset += sprintf(&string[string_offset], "%i ", mrp.data[bytes_in_packet + i]);
    }
    printf("wrote = %s\n", string);
    memset(string, 0, 100);
    string_offset = 0;

    bytes_in_packet += written_bytes;

    if(written_bytes == config_len)
    {
      last_byte_filled = 0;
    }else{
      last_byte_filled += written_bytes;
    }
  }

  //After filling up the universal config, only start filling the schedule into the packet is there is enough space left and
  //-6 because we need at least 5 bytes for information.
  //if(bytes_in_packet < (MASTER_MSG_LENGTH - 6) && (last_schedule_id_started != last_node_id || last_byte_filled != 0))
  if(bytes_in_packet < (MASTER_MSG_LENGTH - 6))
  {
    //The total schedule length
    while(last_schedule_id_started < last_node_id)
    {
      //If we filled a packet up and need to search for the next id, enter the if-case
      // if(last_byte_filled == 0 && schedules[last_schedule_id_started].links_len == 0)
      // {
      //   last_schedule_id_started = get_next_id(last_schedule_id_started);
      // }
      //Always send the current id and the last byte that was written in this id in the beginning of the schedule
      mrp.data[bytes_in_packet] = last_schedule_id_started;              //schedule id where unpacking starts again
      mrp.data[bytes_in_packet + 1] = (last_byte_filled >> 8) & 0xff;    //where writting data was stoped at last iteration in the last schedule id
      mrp.data[bytes_in_packet + 2] = last_byte_filled & 0xff; 
      bytes_in_packet += 3;

      //Get the length for this schedule and remove the bytes that were already sent
      uint16_t schedule_len = 2 + 2 * MASTER_NUM_FLOWS + 1 + schedules[last_schedule_id_started].links_len * sizeof(scheduled_link_t);
      schedule_len -= last_byte_filled;

      //add the length to the schedule to the packet
      mrp.data[bytes_in_packet] = (schedule_len >> 8) & 0xff; 
      mrp.data[bytes_in_packet + 1] = schedule_len & 0xff; 
      bytes_in_packet += 2;

      int written_bytes = write_content(((uint8_t *)&schedules[last_schedule_id_started]) + last_byte_filled, &mrp.data[bytes_in_packet], bytes_in_packet, schedule_len, MASTER_MSG_LENGTH, packet_number);

      for(i = 0; i < written_bytes; i++)
      {
        string_offset += sprintf(&string[string_offset], "%i ", mrp.data[bytes_in_packet + i]);
      }
      printf("node %i with len %i (stoppet at %i) wrote = %s\n", last_schedule_id_started + 1, schedule_len, last_byte_filled, string);
      memset(string, 0, 100);
      string_offset = 0;

      bytes_in_packet += written_bytes;

      //The first case is where a whole schedule for a node fits into the packet. The second case is only a part of the schedule fits in this packet
      if(written_bytes == schedule_len)
      {
        last_schedule_id_started = get_next_id(last_schedule_id_started);
        last_byte_filled = 0; 
      }else{
        last_byte_filled += written_bytes;
        break;
      }

      //If the packet does not even hold 1 byte of the next part of a schedule, finish here
      if((MASTER_MSG_LENGTH - bytes_in_packet) < 6)
      {
        break;
      }
    }
  }

  masternet_len = bytes_in_packet + minimal_routing_packet_size;
  return last_schedule_id_started == last_node_id && last_byte_filled == 0;
}

/* Read as many bytes as possible from a source buffer to a destination buffer */
int read_content(void* from, void* to, int start, int bytes_to_read, int end)
{
  int read_bytes = start + bytes_to_read <= end ? bytes_to_read : end - start;
  memcpy((uint8_t *)to, (uint8_t *)from, read_bytes); 

  return read_bytes;
}

/* Unpack a schedule packet into the schedule struct */
void unpack_schedule_packet(int packet_len)
{
  int i;
  char string[300] = {0};
  int string_offset = 0;
  int current_unpack_index = 2; //0 = command, 1 = packet number, >= 2 data
  uint8_t hash_map_inserted = 0;
  uint8_t id_and_offset_saved = 0;
  uint8_t schedule_index = 0;
  uint16_t last_byte_written = 0;

  if(mrp.data[1] <= end_of_universal_config)
  {
    int last_byte_config_written = (mrp.data[current_unpack_index] << 8) + mrp.data[current_unpack_index + 1];
    current_unpack_index += 2;

    hash_map_insert(&map_packet_to_last_byte_written, mrp.data[1], last_byte_config_written);
    hash_map_inserted = 1;

    printf("last_byte_config_written set %i\n", last_byte_config_written);

    uint16_t config_len = mrp.data[current_unpack_index] << 8;
    config_len += mrp.data[current_unpack_index + 1];
    current_unpack_index += 2; 

    printf("config len %i\n", config_len);

    int read_bytes = read_content(&mrp.data[current_unpack_index], (uint8_t *)&schedule_config + last_byte_config_written, current_unpack_index, config_len, packet_len);

    for(i = 0; i < read_bytes; i++)
    {
      string_offset += sprintf(&string[string_offset], "%i ", (int) mrp.data[current_unpack_index + i]);
    }
    printf("read_config = %s\n", string);
    memset(string, 0, 100);
    string_offset = 0;

    current_unpack_index += read_bytes;
  }

  //If there unpack index is less than the packet size, the packet contains some part of the schedule
  while(current_unpack_index < packet_len)
  {
    schedule_index = mrp.data[current_unpack_index];
    last_byte_written = (mrp.data[current_unpack_index + 1] << 8) + mrp.data[current_unpack_index + 2];
    current_unpack_index += 3;

    //Keep track of the schedule index and offset of the first schedule part of each packet
    //This is required for retransmission without calculating all packets again
    if(!id_and_offset_saved)
    {
      uint8_t result = hash_map_insert(&map_packet_to_schedule_id, mrp.data[1], schedule_index);
      printf("schedule_index set %i result = %i\n", schedule_index, result);
      //If the config was part of this packet, we already inserted the last byte written for the conig into the hash map
      if(!hash_map_inserted)
      {
        hash_map_insert(&map_packet_to_last_byte_written, mrp.data[1], last_byte_written);  
        printf("last_byte_written set %i\n", last_byte_written);
      }
      id_and_offset_saved = 1;
    }

    //Get the len of the current schedule
    uint16_t schedule_len = (mrp.data[current_unpack_index] << 8) + mrp.data[current_unpack_index + 1];
    current_unpack_index += 2;


    int read_bytes = read_content(&mrp.data[current_unpack_index], (uint8_t *)&schedules[schedule_index] + last_byte_written, current_unpack_index, schedule_len, packet_len);

    for(i = 0; i < read_bytes; i++)
    {
      string_offset += sprintf(&string[string_offset], "%i ", mrp.data[current_unpack_index + i]);
    }
    
    printf("node %i with len %i (stoped at %i) read %s\n", schedule_index + 1, schedule_len, last_byte_written, string);
    memset(string, 0, 100);
    string_offset = 0;

    current_unpack_index += read_bytes;
  }
}


/* The callback from master-schedule, once a full schedule is written to flash memory */
void master_schedule_loaded_callback()
{
  LOG_DBG("Schedule loaded\n");
  tsch_eb_active = 0;
  /* Broadcast event */
  //stop = 0;
  //tsch_slot_operation_start();
  //As the CPAN, the schedule is already complete after we arrive at this callback.
  //Initialize distribution as CPAN
  handle_state_change(ST_SCHEDULE_DIST);

  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  mrp.data[0] = CM_SCHEDULE;
  mrp.data[1] = schedule_packet_number;
  setBit(received_packets_as_bit_array, schedule_packet_number);

  //Setup the asn once the schedule distribution starts
  setup_config_asns();

  fill_schedule_packet(2, schedule_packet_number);

  setup_packet_configuration(10, CM_SCHEDULE, mrp.packet_number, 0);

  //Start the timer for the CPAN
  //start_schedule_installation_timer();

  NETSTACK_NETWORK.output(&tsch_broadcast_address);
}
static struct ctimer activate_dist;
void master_schedule_loaded_callback2()
{
  printf("start in 1 sek with dist. do some beacons now\n");
  tsch_eb_active = 1;
  ctimer_set(&activate_dist, CLOCK_SECOND * 10, (void (*) (void*))master_schedule_loaded_callback, NULL);
}
/* Install the schedule for the network */
void install_schedule(){

    //Only remove the slotframe where we listen on every slot, if all nodes have the schedule
  if(!check_received_schedules_by_nodes())
  {
    return;
  }

  //Ignore command that might arrive during installation of the schedule
  handle_state_change(ST_IGNORE_COMMANDS);

  printf("am i a sender node ? %i\n", node_is_sender());

  //master_tsch_schedule_t *scheduletest = get_own_schedule();
  // int string_offset = 0;
  // char string[200] = {0};
  // int j;
  // for(j = 0; j < (2*8 + 3 + scheduletest->links_len*4); j++)
  // {
  //   string_offset += sprintf(&string[string_offset], "%i ", *(((uint8_t *)scheduletest) + j));
  // }
  // printf("read = %s\n", string);
  // memset(string, 0, 100);
  // string_offset = 0;

  if(tsch_queue_get_time_source() != NULL)
  {
    printf("Install schedule at asn %d. My time source is %d\n", (int)tsch_current_asn.ls4b, tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX]);
  }else{
    printf("Install schedule at asn %d.\n", (int)tsch_current_asn.ls4b);
  }


  sf[0] = tsch_schedule_get_slotframe_by_handle(0);
  if (sf[0]){
    tsch_schedule_remove_slotframe(sf[0]);
  }
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

  sf[0] = tsch_schedule_add_slotframe(0, MASTER_EBSF_PERIOD);
  tsch_schedule_add_link(sf[0], LINK_OPTION_TX | LINK_OPTION_RX, LINK_TYPE_ADVERTISING_ONLY, &tsch_broadcast_address, 0, 0);

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

  uint8_t num_of_nodes = 0;

#if TESTBED == TESTBED_KIEL
  num_of_nodes = 20;
#else
  num_of_nodes = NUM_COOJA_NODES;
#endif

  for(i = 0; i < num_of_nodes; i++)
  {
    schedule = &schedules[i];
    int j ;
    for(j = 0; j < schedule->links_len; j++)
    {
      if(schedule->links[j].timeslot > beacon_offset)
      {
        beacon_offset = schedule->links[j].timeslot ;
      }
    }
  }

  beacon_offset++;
  LOG_INFO("Beacon offset (+1) %d\n", beacon_offset);
  // for(i = 0; i < deployment_node_count; i++)
  // {
  //   if(i == node_id - 1)
  //   {
  //     LOG_ERR("Send EB at %d\n", beacon_offset + i);
  //     tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING_ONLY, &tsch_broadcast_address, beacon_offset + i, 0); 
  //   }else{
  //     tsch_schedule_add_link(sf[1], LINK_OPTION_RX, LINK_TYPE_ADVERTISING_ONLY, &tsch_broadcast_address, beacon_offset + i, 0); 
  //   }
  // }
  //tsch_schedule_add_link(sf[1], LINK_OPTION_TX | LINK_OPTION_RX, LINK_TYPE_ADVERTISING_ONLY, &tsch_broadcast_address, schedule_config.schedule_length - 1, 0); 

  tsch_schedule_print();

  reset_nbr_metric_received();

  tsch_set_eb_period(TSCH_EB_PERIOD);

  handle_state_change(ST_SCHEDULE_INSTALLED);

  //TODO:: remove this
  // print_eb_received = 1;
  // print_eb_sent = 1;
  // log_received_paaaaaackets = 1;
  // log_send_packets = 1;
  // use_unicast_ies = 0;
  LOG_INFO("SCHEDULE INSTALLED!!\n");
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

  LOG_INFO("Sending POLL request to %u with size %d\n", dest->addr.u8[NODE_ID_INDEX], masternet_len);
  NETSTACK_NETWORK.output(&dest->addr);
}

/* Forward the nodes own and other received etx-metrics to the time source */
void convergcast_forward_to_timesource()
{
  setup_packet_configuration(tsch_queue_get_time_source()->etx_link, CM_ETX_METRIC_SEND, mrp.packet_number, 0);

  change_link(LINK_TYPE_ADVERTISING, &tsch_queue_get_time_source()->addr, get_beacon_slot());

  LOG_INFO("Sending ETX-Links to %u with size %d, flow_number %i, packet_num %i and retransmits = %i\n", tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX], 
                                                                                          masternet_len, mrp.flow_number, mrp.packet_number, sent_packet_configuration.max_tx);
  NETSTACK_NETWORK.output(&tsch_queue_get_time_source()->addr);
}

/* Depending on the state, send packets to time_source or poll a neighbor for their metric */
void handle_convergcast()
{
  log_asn = 0;
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

  printf("ETX-Links - FROM %i; %s\n", metric_owner, str);
}

/* Calculate the etx-metric for all neighbors */
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

    etx_links[pos] = (uint8_t)nbr->addr.u8[NODE_ID_INDEX];
    etx_links[pos + 1] = (uint8_t)nbr->etx_link;

    nbr->etx_link = etx_links[pos + 1];

    pos += 2;
    nbr = tsch_queue_next_nbr(nbr);
  } 

  return pos; 
}

/*---------------------------------------------------------------------------*/
/* Activate metric gathering for the Network. This has to be started by the CPAN*/
static void master_start_metric_gathering(void *ptr)
{
  LOG_DBG("Starting metric gathering\n");
  //tsch_set_eb_period(CLOCK_SECOND);
  handle_state_change(ST_POLL_NEIGHBOUR);
  LOG_INFO("Time to run before convergcast = %lu ticks \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT * CLOCK_SECOND));
  LOG_INFO("Time to run before convergcast = %lu ms \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT));
  LOG_INFO("Time to run for deactivation of nbr switching = %lu ms \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * 50));
  LOG_INFO("Generate beacon every %lu ms \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count));
  LOG_INFO("Generate beacon every %lu ms \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * CLOCK_SECOND));
  int len = calculate_etx_metric();
  print_metric(etx_links, node_id, len);
  setBit(metric_received, node_id - 1);

  has_next_neighbor();
  
  handle_convergcast();
}

/*Before starting metric gathering, leave enough time for the network to propagate all time source changes through beacons */
static void finalize_neighbor_discovery(void *ptr)
{
  LOG_DBG("Deactivate neighbor switching. nbrs = %i\n", tsch_queue_count_nbr());
  tsch_change_time_source_active = 0;
  char testttt[100];
  int offset = 0;
  int i;
  for(i = 0; i < 20; i++)
  {
    offset += sprintf(&testttt[offset], "%i, ", schedule_received[0]);
  }

  printf("Begging array = %s\n", testttt);
  //TODO:: test setup for precision. remove later
  // tsch_eb_active = 0;
  // struct tsch_neighbor * n = tsch_queue_first_nbr();
  // printf("current eb = %i\n", sequence_number);
  // while(n != NULL)
  // {
  //   printf("ETX:%i = %i, last %i, counted %i\n", n->addr.u8[NODE_ID_INDEX], n->etx_link, n->last_eb, n->count);
  //   n = tsch_queue_next_nbr(n);
  // }

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
/*---------------------------------------------------------------------------*/
/* Handle the case when retransmits are requested after a schedule was installed and the requested packet was transmitted */
// void handle_retransmit_schedule_installed()
// {
//   //Check if there are other neighbors containing packets for retransmitting
//   struct tsch_neighbor* nbr = tsch_queue_first_nbr();

//   while(nbr != NULL && tsch_queue_packet_count(&nbr->addr) == 0)
//   {
//     nbr = tsch_queue_next_nbr(nbr);
//   }

//   if(nbr != NULL && tsch_queue_packet_count(&nbr->addr) != 0)
//   {
//     LOG_INFO("Found another nbr that waits for a retransmit %d\n", nbr->addr.u8[NODE_ID_INDEX]);
//     change_link(LINK_TYPE_ADVERTISING, &nbr->addr, beacon_offset + get_beacon_slot());
//   }else{
//     handle_state_change(ST_SCHEDULE_INSTALLED);
//     tsch_schedule_remove_link_by_timeslot(sf[1], beacon_offset + get_beacon_slot());
//   }
// }

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
      LOG_INFO("Removed packet from this nbr\n");
    }

    handle_state_change(ST_SCHEDULE_RECEIVED);
    schedule_version++;
    LOG_INFO("Received all packets. Version %d with %d packets\n", schedule_version, schedule_packets);
    printf("Set byte at %i\n", node_id);
    setBit1Byte(schedule_received, node_id - 1);

    change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());

    ctimer_stop(&missing_response_to_request_timer);
    
    //start_schedule_installation_timer();

  }else{

    //Dont add multiple requests when receiving important packets while trying to send this packet
    if(tsch_queue_is_packet_in_nbr_queue(tsch_queue_get_nbr(destination), missing_packet))
    {
      LOG_INFO("Already requesting this packet\n");
      return;
    }

    //Prepare the packets for output
    mrp.flow_number = node_id;
    mrp.packet_number = ++own_packet_number;
    mrp.data[0] = CM_SCHEDULE_RETRANSMIT_REQ;
    mrp.data[1] = missing_packet;

    setup_packet_configuration(tsch_queue_get_nbr(destination)->etx_link, CM_SCHEDULE_RETRANSMIT_REQ, mrp.data[1], 0);

    masternet_len = minimal_routing_packet_size + 2;

    LOG_INFO("Requesting retransmit from %d for packet %d with size %d\n", destination->u8[NODE_ID_INDEX], missing_packet, masternet_len);
    NETSTACK_NETWORK.output(destination);
    ctimer_restart(&missing_response_to_request_timer);
  }
}

/*---------------------------------------------------------------------------*/

void missing_response()
{
  handle_state_change(ST_WAIT_FOR_SCHEDULE);
}

/* The callback for TSCH eb_received. Check if the network received the schedule on all nodes */
void master_schedule_received_callback(uint8_t *schedule_received_from_others, uint8_t len)
{
  //To save time, dont even try to compare the received schedules if this is not event the current phase
  if (current_state >= ST_SCHEDULE_DIST && current_state <= ST_SCHEDULE_INSTALLED)
  {
    int i;
    //Check the current state ofthe network. if someone knows about another node having all parts of the schedule, mark this node too
    for(i=0; i< last_node_id;i++)
    {
      if(!isBitSet1Byte(schedule_received, i) && isBitSet1Byte(schedule_received_from_others, i))
      {
        printf("Node %i complete!\n", i+1);
        setBit1Byte(schedule_received, i);
      }
    }

    //Check if the sf[0] can be removed once all nodes have the schedule
    if(check_received_schedules_by_nodes())
    {
      printf("All nodes have a schedule. install\n");
      install_schedule();
    }
  }
}

void master_schedule_difference_callback(linkaddr_t * nbr, uint8_t nbr_schedule_version, uint16_t nbr_schedule_packets)
{
  //Dont get triggered by every beacon to start packet requesting
  if(current_state == ST_SCHEDULE_OLD)
  {
    LOG_INFO("Already searching for packets\n");
    return;
  }

  if(nbr_schedule_version > schedule_version)
  {
    //Remember how many packets the new version of the schedule has.
    schedule_packets = nbr_schedule_packets;
    int missing_packet = getMissingPacket(received_packets_as_bit_array, schedule_packets);

    //The schedule was complete, just not received in order.
    if(missing_packet == -1)
    {
      LOG_INFO("Schedule complete");
      handle_state_change(ST_SCHEDULE_RECEIVED);
      schedule_version++;
      printf("Set byte at %i\n", node_id);
      setBit1Byte(schedule_received, node_id - 1);
      //start_schedule_installation_timer();
      LOG_INFO("Received all packets. Version %d with %d packets\n", schedule_version, schedule_packets);
      return;
    }
    LOG_INFO("Higher schedule from a nbr detected (%d %d)\n", nbr_schedule_version, schedule_version);

    handle_state_change(ST_SCHEDULE_OLD);

    mrp.flow_number = node_id;
    mrp.packet_number = ++own_packet_number;
    mrp.data[0] = CM_SCHEDULE_RETRANSMIT_REQ;
    mrp.data[1] = missing_packet;
    setup_packet_configuration(tsch_queue_get_nbr(nbr)->etx_link, CM_SCHEDULE_RETRANSMIT_REQ, mrp.data[1], 0);
    change_link(LINK_TYPE_ADVERTISING, nbr, get_beacon_slot());

    masternet_len = minimal_routing_packet_size + 2;
    LOG_INFO("Requesting retransmit from %d for packet %d with size %d\n", nbr->u8[NODE_ID_INDEX], missing_packet, masternet_len);
    NETSTACK_NETWORK.output(nbr);

    //Set up a timer for missing responses. We might get a request throught hat will be remoed by installing the schedule
    //In this case, we will simple time out and start listening for beacons and send a request again.
    ctimer_set(&missing_response_to_request_timer, CLOCK_SECOND * 10, missing_response, NULL);
  }
}

static void missing_metric_timeout(void *ptr)
{
  LOG_INFO("Missing metric timeout! \n");
  tsch_eb_active = 0;
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

  LOG_INFO("Sending missing metric request to other nodes as bnroadcast with len %d\n", masternet_len);
  masternet_len = minimal_routing_packet_size + missing_nodes + 2;
  NETSTACK_NETWORK.output(&tsch_broadcast_address);

  ctimer_restart(&missing_metric_timer);
}

/*---------------------------------------------------------------------------*/
/* Depending on the state, set other flags for TSCH and MASTER */
void handle_state_change(enum phase new_state)
{
  if(new_state == ST_SEND_METRIC || new_state == ST_POLL_NEIGHBOUR || 
     new_state == ST_SCHEDULE_DIST || new_state == ST_POLL_MISSING_METRIC ||
     new_state == ST_SCHEDULE_INSTALLED_RETRANSMITTING || new_state == ST_SCHEDULE_RETRANSMITTING ||
     new_state == ST_IGNORE_COMMANDS || new_state == ST_SCHEDULE_OLD)
  {
    printf("EB OFF!\n");
    tsch_eb_active = 0;
  }

  if(new_state == ST_WAIT_FOR_SCHEDULE || new_state == ST_SCHEDULE_RECEIVED || new_state == ST_SCHEDULE_INSTALLED)
  {
    //In case no queued packets and no more neighbors, mark ourself as finished and activate beacons
    //TODO:: remove this
    //log_received_paaaaaackets = 1;
    printf("EB ON!\n");
    tsch_eb_active = 1;
  }

  if(new_state == ST_SCHEDULE_RECEIVED && schedule_installed)
  {
    new_state = ST_SCHEDULE_INSTALLED;
  }

  if(new_state == ST_SCHEDULE_INSTALLED)
  {
    schedule_installed = 1;
  }

  switch (new_state)
  {
  case ST_BEGIN_GATHER_METRIC:
      LOG_INFO("changed state ST_BEGIN_GATHER_METRIC\n");
    break;
  case ST_POLL_NEIGHBOUR:
      LOG_INFO("changed state ST_POLL_NEIGHBOUR\n");
    break;
  case ST_SEND_METRIC:
      LOG_INFO("changed state ST_SEND_METRIC\n");
    break;
  case ST_POLL_MISSING_METRIC:
      LOG_INFO("changed state ST_POLL_MISSING_METRIC\n");
    break;
  case ST_WAIT_FOR_SCHEDULE:
      LOG_INFO("changed state ST_WAIT_FOR_SCHEDULE\n");
      break;
  case ST_SCHEDULE_DIST:
      LOG_INFO("changed state ST_SCHEDULE_DIST\n");
      break;
  case ST_SCHEDULE_OLD:
      LOG_INFO("changed state ST_SCHEDULE_OLD\n");
      break;
  case ST_SCHEDULE_RETRANSMITTING:
      LOG_INFO("changed state ST_SCHEDULE_RETRANSMITTING\n");
      break;
  case ST_SCHEDULE_RECEIVED:
      LOG_INFO("changed state ST_SCHEDULE_RECEIVED\n");
      break;
  case ST_SCHEDULE_INSTALLED_RETRANSMITTING:
      LOG_INFO("changed state ST_SCHEDULE_INSTALLED\n");
      break;
  case ST_SCHEDULE_INSTALLED:
      LOG_INFO("changed state ST_SCHEDULE_INSTALLED\n");
      break;
  case ST_IGNORE_COMMANDS:
      LOG_INFO("changed state ST_IGNORE_COMMANDS\n");
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
    LOG_INFO("No next neighbor\n");
    return 0;
  }

  if(linkaddr_cmp(&next_dest->addr, &tsch_broadcast_address))
  {
    LOG_INFO("Try getting first nbr\n");
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
  LOG_INFO("This is my nbr with id %d, source %d etx-link %d\n", next_dest->addr.u8[NODE_ID_INDEX], next_dest->time_source, next_dest->etx_link);
  return 1;
}

/* Handle the cases where the packets are distributed the first time in the network.
 * We do not handle retransmits here already since there is a speciall callback for this case.
 */
int handle_schedule_distribution_state_changes(enum commands command, uint16_t len)
{
  int result = -1;

  if((command == CM_SCHEDULE) && (isBitSet(received_packets_as_bit_array, mrp.data[1]) == 0))
  {
    handle_state_change(ST_SCHEDULE_DIST);
    command_input_schedule_packet(command, len);
    result = 1;
  }

  if((command == CM_SCHEDULE_END) && (isBitSet(received_packets_as_bit_array, mrp.data[1]) == 0))
  {
    handle_state_change(ST_SCHEDULE_DIST);
    command_input_schedule_last_packet(command, len);
    result = 1;
  }

  //Treat retransmits as normal packets until we realise that we have an old schedule
  if((command == CM_SCHEDULE_RETRANSMIT) && (isBitSet(received_packets_as_bit_array, mrp.data[1]) == 0))
  {
    handle_state_change(ST_SCHEDULE_DIST);

    //If we miss this packet, add the packet to the schedule
    if(isBitSet(received_packets_as_bit_array, mrp.data[1]) == 0)
    {
      LOG_INFO("---------------- Packet Retransmit Nr.%d \n", mrp.data[1]);
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
  if(!tsch_is_coordinator)
  {
    LOG_INFO("Nothing to do after resending a schedule as a normal node\n");
    return;
  }

  schedule_packet_number++;
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  mrp.data[0] = CM_SCHEDULE;
  mrp.data[1] = schedule_packet_number;
  setBit(received_packets_as_bit_array, schedule_packet_number);

  if(fill_schedule_packet(2, schedule_packet_number))
  {
    LOG_INFO("INC SCHED from last packet sent as cpan. Schedule finished\n");
    schedule_version++;
    schedule_packets = schedule_packet_number;

    //Let other nodes know by beacon, of how many nodes we know, that they finished receiving the schedule
    printf("Set byte at %i\n", node_id);
    setBit1Byte(schedule_received, node_id - 1);

    mrp.data[0] = CM_SCHEDULE_END;
    setup_packet_configuration(10, CM_SCHEDULE_END, mrp.packet_number, 0);
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
    LOG_INFO("change link to time src %i\n", tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX]);
    break;
  case ST_POLL_NEIGHBOUR:
    LOG_INFO("Queue empty but polling neighbors are there\n");
    handle_convergcast();
    LOG_INFO("handle convergcast\n");
    break;
  case ST_WAIT_FOR_SCHEDULE:
    //As the coordinator, set a timer when polling is finished. In case metric do not arrive, start searching for missing packets
    if(tsch_is_coordinator)
    {
      LOG_INFO("Starting timer for 20 sec\n");
      ctimer_set(&missing_metric_timer, CLOCK_SECOND * 20, missing_metric_timeout, NULL);
    }
    change_link(LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot());
    LOG_INFO("Finished polling neighbors\n");
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
        printf("relay packet %u. received at ASN %u (current asn %lu) from flow %u\n", mrp.packet_number, received_asn, tsch_current_asn.ls4b , mrp.flow_number);
      }
      else
      {
        printf("packet not enqueueing: next ASN %u, received ASN %u, TTL ASN %u\n", (uint16_t)tsch_current_asn.ls4b, received_asn, mrp.ttl_slot_number);
      }
    #else
      sent_packet_configuration.max_tx = get_max_transmissions(schedule, mrp.flow_number);
      masternet_len = len; // send same length as received
      NETSTACK_NETWORK.output(&destination);
      printf("relay %u at ASN %lu (received at %u) from flow %u\n", mrp.packet_number, tsch_current_asn.ls4b, received_asn, mrp.flow_number);
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
    LOG_INFO("---------------- Packet Retransmit Nr.%d \n", retransmitted_packet);
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
    LOG_INFO("Skip request for next packet: packet was not for me!\n");
  }
}

// void command_input_schedule_retransmitt_request_installed_schedule(const linkaddr_t *src)
// {
//   //While parsing and retransmitting, the src addr becomes null. Save the address localy
//   const linkaddr_t source_addr = *src;

//   handle_state_change(ST_SCHEDULE_INSTALLED_RETRANSMITTING);

//   //We received a request to retransmit a packet. enter the retransmit state and prepare the packet.
//   mrp.flow_number = node_id;
//   mrp.packet_number = ++own_packet_number;
//   mrp.data[0] = CM_SCHEDULE_RETRANSMIT;
//   int missing_packet = mrp.data[1];

//   //Get the schedule index and the offset for the requestet packet to avoid calculating from the start
//   last_schedule_id_started = hash_map_lookup(&map_packet_to_schedule_id, missing_packet);
//   last_byte_filled = hash_map_lookup(&map_packet_to_last_byte_written, missing_packet);
//   printf("after reading %i with schedule id %i and last byte filled %i \n", missing_packet, last_schedule_id_started, last_byte_filled);
//   fill_schedule_packet(2, missing_packet);

//   setup_packet_configuration(tsch_queue_get_nbr(&source_addr)->etx_link, CM_SCHEDULE_RETRANSMIT, mrp.data[1], 1);

//   NETSTACK_NETWORK.output(&source_addr);

//   if(tsch_schedule_get_link_by_timeslot(sf[1], beacon_offset + get_beacon_slot()) == NULL)
//   {
//     tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &source_addr, beacon_offset + get_beacon_slot(), 0);
//   }else{
//     LOG_INFO("Added a packet for %d to queue\n", source_addr.u8[NODE_ID_INDEX]);
//   }
// }

/* Handle behaviour in case of a request for a retransmit from another node */
void command_input_schedule_retransmitt_request(const linkaddr_t *src)
{
  //While parsing and retransmitting, the src addr becomes null. Save the address localy
  const linkaddr_t source_addr = *src;

  LOG_INFO("Received request to retransmit packet %d from %d\n", mrp.data[1], source_addr.u8[NODE_ID_INDEX]);

  /* Small optimization: Due to missing ACK's, a node that sends requests to us for retransmit might send the same 
   * request multiple times. Since only 1 request by each node will be send at a time, check if the queue for the requester contains the
   * packet that was requestet.
  */
  if(tsch_queue_is_packet_in_nbr_queue(tsch_queue_get_nbr(&source_addr), mrp.data[1]))
  {
    LOG_DBG("Packet already in Queue\n");
    return;
  }

  handle_state_change(ST_SCHEDULE_RETRANSMITTING);

  //We received a request to retransmit a packet. enter the retransmit state and prepare the packet.
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  mrp.data[0] = CM_SCHEDULE_RETRANSMIT;
  int missing_packet = mrp.data[1];

  //Get the schedule index and the offset for the requestet packet to avoid calculating from the start
  last_schedule_id_started = hash_map_lookup(&map_packet_to_schedule_id, missing_packet);
  last_byte_filled = hash_map_lookup(&map_packet_to_last_byte_written, missing_packet);
  printf("after reading %i with schedule id %i and last byte filled %i \n", missing_packet, last_schedule_id_started, last_byte_filled);
  fill_schedule_packet(2, missing_packet);

  setup_packet_configuration(tsch_queue_get_nbr(&source_addr)->etx_link, CM_SCHEDULE_RETRANSMIT, mrp.data[1], 1);

  NETSTACK_NETWORK.output(&source_addr);

  //If we receive requests to retransmit parts of the schedule, we add them to the queue of the neighbor
  //When changing the link to a requester for a retransmit, check if we are looking at the broadcast address:
  //If broadcast address -> this is the first request for a retransmit. Change to requester and the packet will be sent
  //If not broadcast address -> we already send a retransmit to another nbr. Link change will be perfmored later in the callback
  if(linkaddr_cmp(&tsch_schedule_get_link_by_timeslot(sf[1], get_beacon_slot())->addr, &tsch_broadcast_address) != 0)
  {
    LOG_INFO("This is the first request, change to requester\n");
    tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &source_addr, get_beacon_slot(), 0);
  }else{
    LOG_INFO("This is a difference in addresses %d vs %d\n", tsch_schedule_get_link_by_timeslot(sf[1], get_beacon_slot())->addr.u8[NODE_ID_INDEX], tsch_broadcast_address.u8[NODE_ID_INDEX]);
  }
}

/* Unpack a schedule packet and mark it as received */
void command_input_schedule_new_packet(uint16_t len)
{
  //Check the packets in the bit vector to later find missing packets
  setBit(received_packets_as_bit_array, mrp.data[1]);

  //Unpack into the schedule structure
  unpack_schedule_packet(len - minimal_routing_packet_size);
}

/* Handle a packet containing the last part of the TSCH schedule */
void command_input_schedule_last_packet(enum commands command, uint16_t len)
{
  LOG_INFO("---------------- Packet Nr.%d \n", mrp.data[1]);

  command_input_schedule_new_packet(len);

  //Check if a packet is missing
  schedule_packets = mrp.data[1];
  int missing_packet = getMissingPacket(received_packets_as_bit_array, schedule_packets);

  //If all packets are received, set the schedule version and the state. Active EB's again to signal the new schedule version
  if(missing_packet == -1)
  { 
    handle_state_change(ST_SCHEDULE_RECEIVED);
    schedule_version++;
    printf("Set byte at for %i\n", node_id);
    setBit1Byte(schedule_received, node_id - 1);

    //start_schedule_installation_timer();
    LOG_INFO("Received all packets. Version %d with %d packets\n", schedule_version, schedule_packets);
    LOG_INFO("INC SCHED since all packets were received\n");
  }else{
    LOG_INFO("Missing some packets. wait for beacons\n");
  }
  tsch_change_time_source_active = 1;

  setup_packet_configuration(10, command, mrp.packet_number, 0);

  masternet_len = len;
  NETSTACK_NETWORK.output(&tsch_broadcast_address);

}

/* Handle a packet containing a part of the TSCH schedule */
void command_input_schedule_packet(enum commands command, uint16_t len)
{
  LOG_INFO("---------------- Packet Nr.%d \n", mrp.data[1]);

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
    if(node_id == missing_metric_from_node[i])
    {
      LOG_INFO("Missing metric from me!\n");
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
      //LOG_INFO("looking at node %d and missing node %d\n", n->addr.u8[NODE_ID_INDEX], missing_metric_from_node[i]);
      //If this neighbor is one of the missing nodes, we need to poll him
      if(n->addr.u8[NODE_ID_INDEX] == missing_metric_from_node[i])
      {
        if(current_choice == NULL)
        {
          current_choice = n;
          break;
        }else{
          if(n->rank < current_choice->rank)
          {
            LOG_INFO("Neighbor %d (rank %d) has a smaller rank than %d (rank %d). Switch\n", n->addr.u8[NODE_ID_INDEX], current_choice->addr.u8[NODE_ID_INDEX], n->rank, current_choice->rank);
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

void command_input_missing_metric(int len)
{
  struct tsch_neighbor * to_poll = has_missing_node_as_nbr();
  if(to_poll != NULL)
  {
    LOG_INFO("Send missing metric request to %i\n", to_poll->addr.u8[NODE_ID_INDEX]);
    handle_state_change(ST_POLL_MISSING_METRIC);
    convergcast_poll_neighbor(CM_ETX_METRIC_MISSING, to_poll);
  }else{
    LOG_INFO("Forward broadcast\n");
    //TODO:: only forward if less than x broadcasts received?
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
  LOG_INFO("Starting prepare metric by command\n");
  mrp.flow_number = node_id;
  mrp.packet_number = ++own_packet_number;
  int command = CM_ETX_METRIC_SEND;

  int len = calculate_etx_metric();

  memcpy(&(mrp.data), &command, sizeof(uint8_t));
  memcpy(&(mrp.data[1]), etx_links, len);
  masternet_len = minimal_routing_packet_size + sizeof(uint8_t) + len;

  handle_convergcast();
}

/* Behaviour for the CPAN */
void command_input_send_metric_CPAN(uint16_t len, const linkaddr_t *src)
{
  if(metric_complete)
  {
    LOG_INFO("ignore metric. all packets received\n");
    return;
  }

  //While in neighbor discovery mode, flow number = node_id
  setBit(metric_received, mrp.flow_number - 1);
  int i;
  char missing_metrics[100];
  int offset = 0;
  int nodes_to_count = deployment_node_count;
  int finished_nodes = 0;
#if TESTBED == TESTBED_KIEL
  nodes_to_count++;
#endif
  for(i = 0; i < nodes_to_count; i++)
  {
    if(isBitSet(metric_received, i))
    {
      finished_nodes++;
    }else{
      offset += sprintf(&missing_metrics[offset], "%i, ", i+1);
    }
  }

  //Once all metrics are received, stop the timer to handle missing metrics and start listening for the schedule transmission
  print_metric(&mrp.data[COMMAND_END], mrp.flow_number, len - minimal_routing_packet_size - COMMAND_END); //-3 for the mrp flow number and packet number and -command length
  if(finished_nodes == deployment_node_count)
  {
    metric_complete = 1;
    test_stop();
    printf("ETX-Links finished!\n");   

    ctimer_stop(&missing_metric_timer);
    process_start(&serial_line_schedule_input, NULL);

  }else{
    printf("Missing metric from %s\n", missing_metrics);
    if(current_state == ST_WAIT_FOR_SCHEDULE)
    {
      ctimer_restart(&missing_metric_timer);
    }
  }
}

/* Behaviour for a node that is not the CPAN */
void command_input_send_metric_Node(uint16_t len, const linkaddr_t *src)
{
  masternet_len = len;

  // Response of the Polling request, forward the metric to own time source
  if(current_state == ST_WAIT_FOR_SCHEDULE && tsch_queue_is_empty(tsch_queue_get_time_source()))
  {        
    LOG_INFO("Polling finished but packet arrived. handle convergast!\n");
    handle_convergcast();
  }else{
    LOG_INFO("Add packet to time source queue %i\n", tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX]);
    setup_packet_configuration(tsch_queue_get_time_source()->etx_link, CM_ETX_METRIC_SEND, mrp.packet_number, 0);
    NETSTACK_NETWORK.output(&tsch_queue_get_time_source()->addr);
  }
}

/* Handle packet containing the ETX-metric of a node */
int command_input_send_metric(uint16_t len, const linkaddr_t *src)
{
  /* Small optimization: only works for direct neighbors */
  //In case we received the metric from our neighbor and he did not receive our ACK, he will resend the metric again -> Drop the packet here
  //The first check is to see if this packet is a metric from the sender or a forward from a node further away. 
  //In the second case, we might not have this neighbor and do not check if the packet should be dropet
  struct tsch_neighbor * nbr = tsch_queue_get_nbr(src);
  if(mrp.flow_number == src->u8[NODE_ID_INDEX] && nbr != NULL && nbr->etx_metric_received)
  {
    LOG_INFO("Received already metric from %d = %d", mrp.flow_number, src->u8[NODE_ID_INDEX]);
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
void transition_to_new_state_after_callback(packet_data_t * packet_data, int has_packets_to_forward)
{
  //We only end up in this function after a succesfull transmission and acknowledgment of a packet
  enum phase next_state = 0;
  printf("Callback arrived with command %i\n", packet_data->command);

  //Handle different packets that we succesfully send
  switch (packet_data->command)
  {
  case CM_ETX_METRIC_SEND:
    LOG_INFO("state send metric\n");
    //If there are packets left, keep sending the packets until the queue is empty. Otherwise poll neighbors
    //Only poll neighbors if there are any neighbors left to poll. Otherwise we are done.
    next_state = has_packets_to_forward ? ST_SEND_METRIC : has_next_neighbor() ? ST_POLL_NEIGHBOUR : ST_WAIT_FOR_SCHEDULE;

    handle_state_change(next_state);
    callback_convergcast(next_state);
    break;

  case CM_ETX_METRIC_GET:
    LOG_INFO("state poll metric\n");
    //If there are packets left, keep sending the packets until the queue is empty. Otherwise poll neighbors
    //Only poll neighbors if there are any neighbors left to poll. Otherwise we are done.
    next_state = has_packets_to_forward ? ST_SEND_METRIC : has_next_neighbor() ? ST_POLL_NEIGHBOUR : ST_WAIT_FOR_SCHEDULE;

    handle_state_change(next_state);
    callback_convergcast(next_state);
    break;

  case CM_ETX_METRIC_MISSING:
    next_state = has_packets_to_forward ? ST_SEND_METRIC : ST_WAIT_FOR_SCHEDULE;
    LOG_INFO("finished CM_ETX_METRIC_MISSING\n");
    handle_state_change(next_state);
    callback_convergcast(next_state);
    break;

  case CM_SCHEDULE:
    LOG_INFO("State CM Schedule\n");
    callback_input_schedule_send();
    break;

  case CM_SCHEDULE_END:   
    if(getMissingPacket(received_packets_as_bit_array, schedule_packets) == -1)
    {
      //Once the schedule is broadcastet completly, start sending EB's with new version and packet number
      handle_state_change(ST_SCHEDULE_RECEIVED);
    }else{
      LOG_INFO("Schedule not finished\n");
    }
    break;

  case CM_SCHEDULE_RETRANSMIT_REQ:
    if(current_state == ST_SCHEDULE_OLD){
      //If we received the packet already, search the next missing packet. otherwise wait for response.
      if(isBitSet(received_packets_as_bit_array, packet_data->packet_nbr))
      {
        //When sending a packet, the packetbuffer is reset in Master-NET. Save the address
        const linkaddr_t dst = *packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
        request_retransmit_or_finish(&dst); 
      }else{
        LOG_INFO("Restart timeout after an ack\n");
        ctimer_restart(&missing_response_to_request_timer);
      }
    }
    break;

  case CM_SCHEDULE_RETRANSMIT:
    if(current_state == ST_SCHEDULE_RETRANSMITTING)
    {
      handle_retransmit();
    }
    // if(current_state == ST_SCHEDULE_INSTALLED_RETRANSMITTING)
    // {
    //   handle_retransmit_schedule_installed();
    // }

    break;

  default:
    LOG_INFO("Dont react to callback for command %d\n", packet_data->command);
    break;
  }
}

/* handle the transition between states after a packet input */
int transition_to_new_state(enum commands command, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest, enum phase next_state)
{
  int result = 1;
  LOG_DBG("Received a packet from %d with len %d and command %d\n", src->u8[NODE_ID_INDEX], len, command);
  switch (current_state)
  {
  //Only allow request for metric
  case ST_EB:
    if(command == CM_ETX_METRIC_GET)
    {
      //TODO remove this later
      // if(first_miss)
      // {
      //   int i;
      //   for(i=0; i < 4; i++)
      //   {
      //     if(miss_array[i] == node_id)
      //     {
      //       printf("miss on first try\n");
      //       first_miss = 0;
      //       return 1; 
      //     }
      //   }
      // }

      //tsch_set_eb_period(CLOCK_SECOND);
      handle_state_change(ST_SEND_METRIC);
      command_input_get_metric();
    }
    
    //We only want to react to missing metric requests from nodes that are not our time source
    //If they are our time source and the did not poll us, they are not receiving our packets.
    if(command == CM_ETX_METRIC_MISSING && linkaddr_cmp(src, &tsch_queue_get_time_source()->addr) == 0){
      //tsch_set_eb_period(CLOCK_SECOND);
      tsch_queue_update_time_source(src);
      handle_state_change(ST_SEND_METRIC);
      command_input_get_metric();
    }else {
      result = -1;
    }
    break;

  //Switch to forwarding once a forward packet arrives
  case ST_POLL_NEIGHBOUR:
    if(command == CM_ETX_METRIC_SEND)
    {
      LOG_DBG("Polling nbrs right now \n");
      command_input_send_metric(len, src);
    }else{
      result = -1;
    }
    break;

    
  //Keep forwarding when packets arrive for a forward
  case ST_SEND_METRIC:
    if(command == CM_ETX_METRIC_SEND)
    {
      LOG_DBG("Sending data right now \n");
      command_input_send_metric(len, src);
    }else{
      result = -1;
    }
    break;

  case ST_POLL_MISSING_METRIC:
    if(command == CM_ETX_METRIC_SEND)
    {
      LOG_DBG("Forward packet\n");
      command_input_send_metric(len, src);
    }else{
      result = -1;
    }
    break;

  //In this state we wait for the schedule distribution. Packets containing the etx-metric forward can still arrive
  case ST_WAIT_FOR_SCHEDULE:
    if(command == CM_ETX_METRIC_SEND)
    {
      int change_state = command_input_send_metric(len, src);
      if(!tsch_is_coordinator && change_state)
      {
        handle_state_change(ST_SEND_METRIC);
      }
    }else if(command == CM_ETX_METRIC_MISSING && ignored_missing_metric_requests == 0){
      //As the coordinator we ignore the broadcasts. This is only a repeat of our initial message
      if(tsch_is_coordinator)
      {
        return 0;
      }

      //Stop receiving more packets asking for the missing schedule
      ignored_missing_metric_requests = 1;
      ctimer_set(&ignore_metric_timer, CLOCK_CONF_SECOND * 5, activate_missing_metric_requests, NULL);

      //If we are one of the nodes that is missing, send the metric
      if(missing_metric_from_myself())
      {
        tsch_queue_update_time_source(src);
        command_input_get_metric();
      }else{
        LOG_DBG("lookup nbrs for missing metrix\n");
        command_input_missing_metric(len);
      }

    }else if(command == CM_ETX_METRIC_MISSING && ignored_missing_metric_requests == 1){
      LOG_DBG("Ignore missing metric requests for now\n");
    }else{
      result = handle_schedule_distribution_state_changes(command, len);
    }
    break;

  case ST_SCHEDULE_DIST:
    log_asn = 1;
    result = handle_schedule_distribution_state_changes(command, len);
    break;

  case ST_SCHEDULE_RECEIVED:
    if(command == CM_SCHEDULE_RETRANSMIT_REQ)
    {
      command_input_schedule_retransmitt_request(src);
    }
    break;

  case ST_SCHEDULE_RETRANSMITTING:
    if(command == CM_SCHEDULE_RETRANSMIT_REQ)
    {
      command_input_schedule_retransmitt_request(src);
    }
    break;

  case ST_SCHEDULE_OLD:
    if(command == CM_SCHEDULE_RETRANSMIT)
    {
      command_input_schedule_retransmitt(len, src, dest);
    }
    break;

  case ST_SCHEDULE_INSTALLED:
    if(command == CM_DATA)
    {

      uint16_t received_asn = packetbuf_attr(PACKETBUF_ATTR_RECEIVED_ASN); //The rx slot when the packet was received in slot-operation
      struct master_tsch_schedule_t* schedule = get_own_schedule();
      // struct tsch_slotframe *sf;
      // uint16_t sf_size;
      // uint16_t current_sf_slot;
      // sf = tsch_schedule_get_slotframe_by_handle(mrp.flow_number);
      // current_sf_slot = TSCH_ASN_MOD(tsch_current_asn, sf->size);
      // sf_size = ((uint16_t)((sf->size).val));

      // struct tsch_link * link = tsch_schedule_get_link_by_timeslot(sf, tsch_current_asn.ls4b % (int)sf_size);
      // struct tsch_link * received_link = tsch_schedule_get_link_by_timeslot(sf, (int)received_asn % (int)sf_size);  
      
      // printf("received ASN %i, current asn %lu, current slot %i for packet %i\n", received_asn, tsch_current_asn.ls4b, current_sf_slot, mrp.packet_number);

      // if(received_link != NULL)
      // {
      //   printf("received at sf %i, option %i, channel_offset %i, timeslot %i, type  %i\n", 
      //   sf->handle, received_link->link_options, received_link->channel_offset, received_link->timeslot, received_link->link_type);        
      // }else{
      //   printf("received_link = 0\n");
      // }

      // if(link != NULL)
      // {
      //   printf("forward at current asn %lu with sf_size = %i\n", tsch_current_asn.ls4b, sf_size);
      //   printf("next forward slot possible at sf %i, option %i, channel_offset %i, timeslot %i, type  %i\n", 
      //   sf->handle, link->link_options, link->channel_offset, link->timeslot, link->link_type);
      // }else{
      //   printf("link = 0\n");
      // }

      // This node is the receiver of the flow
      if (node_id == schedule_config.receiver_of_flow[mrp.flow_number - 1])
      {
        command_input_data_received(received_asn, len);
      }else{
        //Forward the packet through the flow
        command_input_data_forwarding(schedule, received_asn, len);
      }
    }

    // if(command == CM_SCHEDULE_RETRANSMIT_REQ)
    // {
    //   LOG_DBG("Installed schedule retransmit req\n");
    //   command_input_schedule_retransmitt_request_installed_schedule(src);
    // }
    break;

  default:
    LOG_ERR("Something went wrong!\n");
    result = -1;
    break;
  }

  return result;
}

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
  enum phase next_state = 0;
  next_state = transition_to_new_state(command, len, src, dest, next_state);
  if(next_state == -1)
  {
    LOG_ERR("Invalid command %d during state %d\n", command, current_state);
    return;
  }
}

/*---------------------------Callback after sending packets and their main functions--------------------------------*/ 

void handle_callback_commands_divergcast(packet_data_t *packet_data, int ret, int transmissions)
{
  //This will only happen in unicast -> retransmit/retransmit_request
  if (ret != MAC_TX_OK)
  {
    int ie_offset = 0;
    if(packetbuf_attr(PACKETBUF_ATTR_MAC_METADATA))
    {
      LOG_DBG("Callback for a packet with IE fields\n");
      ie_offset = 9;
    }
    memset(&mrp, 0, sizeof(mrp));
    memcpy(&mrp, packetbuf_dataptr() + packet_data->hdr_len + ie_offset, packetbuf_datalen() - packet_data->hdr_len - ie_offset);
    LOG_DBG("MRP command %s, packet %d, trans %d; size %d (hdr %d vs packet %d) and ie_offset %d\n", mrp.data[0] == CM_SCHEDULE_RETRANSMIT ? "Retransmit" : "Request", 
    mrp.data[1], transmissions, packetbuf_datalen(), packetbuf_hdrlen(), packet_data->hdr_len, ie_offset);
    linkaddr_t dest_resend = *packetbuf_addr(PACKETBUF_ADDR_RECEIVER);

    //Small optimization: in case we did not receive an ACK but the nbr send us already the requestet packet, drop the request
    if(mrp.data[0] == CM_SCHEDULE_RETRANSMIT_REQ)
    {
      if(isBitSet(received_packets_as_bit_array, mrp.data[1]))
      {
        LOG_DBG("We already have this %d packet. Dropp request for it\n", mrp.data[1]);
        request_retransmit_or_finish(&dest_resend); 
        return;
      }else{
        LOG_DBG("Bad link quality. drop search and wait for new beacon from other neighbor\n");
        handle_state_change(ST_SCHEDULE_DIST);
        ctimer_stop(&missing_response_to_request_timer);
        return;
      }
    }
    if(mrp.data[0] == CM_SCHEDULE_RETRANSMIT)
    {
      setup_packet_configuration(tsch_queue_get_nbr(&dest_resend)->etx_link, CM_SCHEDULE_RETRANSMIT, mrp.data[1], 1);

      LOG_ERR("Retransmit packet %d to %d \n", mrp.data[1], dest_resend.u8[NODE_ID_INDEX]);
      masternet_len = packetbuf_datalen() - packet_data->hdr_len - ie_offset;
      NETSTACK_NETWORK.output(&dest_resend);
      return;
    }
  }

  transition_to_new_state_after_callback(packet_data, 0);
}

void handle_callback_commands_convergcast(packet_data_t *packet_data, int ret, int transmissions)
{
  //In case of an error, retransmit the packet again
  if (ret != MAC_TX_OK )
  {
    LOG_DBG("Transmission error for command %d after %i transmits with code %d\n", packet_data->command, transmissions, ret);
    struct tsch_neighbor * nbr = tsch_queue_get_nbr(packetbuf_addr(PACKETBUF_ADDR_RECEIVER));
    if(nbr == NULL)
    {
      return;
    }
    /* Small optimizaton: If we poll a neighbor and dont receive ACKS, but already received this neighbors etx-metric, stop sending requests*/
    if(current_state == ST_POLL_NEIGHBOUR && nbr->etx_metric_received)
    {
      LOG_DBG("Packet received while polling neighbor %d, dont repeat request!\n", nbr->addr.u8[NODE_ID_INDEX]);
    }else if(current_state == ST_POLL_MISSING_METRIC && packet_data->command == CM_ETX_METRIC_MISSING){
      LOG_DBG("Handle poll again while missing metric\n");
      convergcast_poll_neighbor(CM_ETX_METRIC_MISSING, nbr);
      return;
    }else{
      memset(&mrp, 0, sizeof(mrp));
      memcpy(&mrp, packetbuf_dataptr() + packet_data->hdr_len, packetbuf_datalen() - packet_data->hdr_len);
      handle_convergcast();
      return;
    }
  }

  if(tsch_is_coordinator)
  {
    transition_to_new_state_after_callback(packet_data, 0);
  }else{
    transition_to_new_state_after_callback(packet_data, tsch_queue_is_empty(tsch_queue_get_time_source()) == 0);
  }
}

// Callback for sent packets over TSCH.
void master_routing_output_callback(void *data, int ret, int transmissions)
{
  packet_data_t *packet_data = (packet_data_t *)data;
  LOG_DBG("Current state: %d with command %d and return value %d for receiver %i\n", current_state, packet_data->command, ret, packetbuf_addr(PACKETBUF_ADDR_RECEIVER)->u8[NODE_ID_INDEX]);

  if(packet_data->command == CM_DATA || packet_data->command == CM_NO_COMMAND || packet_data->command == CM_END)
  {
    return;
  }

  //Handle the metric gathering callback once a packet is sent or an error on sent occured
  if(packet_data->command >= CM_ETX_METRIC_GET && packet_data->command <= CM_ETX_METRIC_SEND)
  {
    handle_callback_commands_convergcast(packet_data, ret, transmissions);
  }

  //Handle the metric distribution callback once a packet is sent or an error on sent occured
  if(packet_data->command >= CM_SCHEDULE && packet_data->command <= CM_SCHEDULE_END)
  {
    handle_callback_commands_divergcast(packet_data, ret, transmissions);
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
    
    //struct tsch_link * link = tsch_schedule_get_link_by_timeslot(sf, mrp.earliest_tx_slot % sf_size);

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

#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
      sent_packet_configuration.important_packet = 0;
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

      LOG_DBG("expected max tx: %u\n", sent_packet_configuration.max_tx);

      masternet_len = minimal_routing_packet_size + COMMAND_END + datalen;
      sent_packet_configuration.command = CM_DATA;
      sent_packet_configuration.packet_nbr = mrp.packet_number;
      NETSTACK_NETWORK.output(&destination);
      LOG_INFO("Send %d bytes with command %u\n", masternet_len, mrp.data[0]);

      // print sent data
#if TSCH_TTL_BASED_RETRANSMISSIONS
      LOG_INFO("sent %u at ASN %u till ASN %u  (current asn %lu) for flow %u\n", mrp.packet_number, mrp.earliest_tx_slot, mrp.ttl_slot_number, tsch_current_asn.ls4b, mrp.flow_number);
      // if(link != NULL)
      // {

      //   // printf("Send at earliest tx %i and sf_size = %i\n", mrp.earliest_tx_slot, sf_size);
      //   // printf("Send at sf %i, option %i, channel_offset %i, timeslot %i, type  %i, (current sf slot %i, sending at %i)\n", 
      //   // sf->handle, link->link_options, link->channel_offset, link->timeslot, link->link_type, current_sf_slot, (int)mrp.earliest_tx_slot % (int)sf_size);
      // }else{
      //   printf("link = 0\n");
      // }

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
  LOG_TRACE("init_master_routing \n");
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
  LOG_INFO("Generate beacon every %lu ms \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * CLOCK_SECOND));
  //LOG_INFO("Generate beacon every %lu ms \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * CLOCK_SECOND * deployment_node_count));
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

  //Calculate the end of the universal config. Each packet contains MASTER_MSG_LENGTH bytes (- 1 byte for command and packet number) of the config
  end_of_universal_config = sizeof(master_tsch_schedule_universall_config_t) / (MASTER_MSG_LENGTH - 6);
  if(sizeof(master_tsch_schedule_universall_config_t) % (MASTER_MSG_LENGTH - 6) != 0)
  {
    end_of_universal_config++;
  }
  LOG_ERR("Packets with config end at %d", end_of_universal_config);

  master_schedule_set_schedule_loaded_callback(master_schedule_loaded_callback2);

  sf[0] = tsch_schedule_add_slotframe(0, 1);                      // Listen on this every frame where the nodes doesnt send

  //Make slotframes always uneven
  if(deployment_node_count % 2)
  {
    LOG_ERR("Uneven\n");
    sf[1] = tsch_schedule_add_slotframe(1, deployment_node_count);  // send in this frame every "node_count"
    //sf[2] = tsch_schedule_add_slotframe(2, deployment_node_count);  // send in this frame every "node_count"
  }else{
    sf[1] = tsch_schedule_add_slotframe(1, deployment_node_count + 1);  // send in this frame every "node_count"
    //sf[2] = tsch_schedule_add_slotframe(2, deployment_node_count + 1);  // send in this frame every "node_count"
    LOG_ERR("Even\n");
  }


  tsch_schedule_add_link(sf[0], LINK_OPTION_RX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, 0, 0);
  tsch_schedule_add_link(sf[1], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot(), 0);
  //tsch_schedule_add_link(sf[2], LINK_OPTION_TX, LINK_TYPE_ADVERTISING, &tsch_broadcast_address, get_beacon_slot(), 0);
  /* wait for end of TSCH initialization phase, timed with MASTER_INIT_PERIOD */
  
  //LOG_INFO("Time to run before convergcast = %i", ((TSCH_DEFAULT_TS_TIMESLOT_LENGTH / 1000) * deployment_node_count * TSCH_BEACON_AMOUNT) / 1000);
  //ctimer_set(&install_schedule_timer, (CLOCK_SECOND * (TSCH_DEFAULT_TS_TIMESLOT_LENGTH / 1000) * deployment_node_count * TSCH_BEACON_AMOUNT) / 1000, finalize_neighbor_discovery, NULL);
  
#if TESTBED == TESTBED_KIEL
  LOG_INFO("Time to run before convergcast = %lu ms \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT));
  LOG_INFO("Time to run before convergcast = %lu ticks \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT * CLOCK_SECOND));
  ctimer_set(&install_schedule_timer, (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT * CLOCK_SECOND), finalize_neighbor_discovery, NULL);
#else
  LOG_INFO("Time to run before convergcast = %lu ms \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT * 1000));
  LOG_INFO("Time to run before convergcast = %lu ticks \n", (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT * CLOCK_SECOND));
  ctimer_set(&install_schedule_timer, (uint32_t)(get_percent_of_second(tsch_ts_timeslot_length) * deployment_node_count * TSCH_BEACON_AMOUNT * CLOCK_SECOND), finalize_neighbor_discovery, NULL);
  //ctimer_set(&install_schedule_timer, 200 * CLOCK_SECOND, finalize_neighbor_discovery, NULL);
#endif
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
  LOG_TRACE_RETURN("init_master_routing \n");
}
/*---------------------------------------------------------------------------*/
/** @} */
