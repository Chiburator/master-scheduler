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
 *         Header file for the Master Routing module.
 * \author
 *         Oliver Harms <oha@informatik.uni-kiel.de>
 *
 */

#ifndef MASTER_ROUTING_H
#define MASTER_ROUTING_H

#include "contiki.h"
#include "master-conf.h"
#include "tsch/tsch-schedule.h"
#include "net/linkaddr.h"
#include "net/master-net/master-net.h"
#include "master-schedule.h"

enum phase
{
  //States for data gathering
  ST_EB,
  ST_BEGIN_GATHER_METRIC,
  ST_POLL_NEIGHBOUR,
  ST_SEND_METRIC,
  ST_POLL_MISSING_METRIC,
  ST_SEARCH_MISSING_METRIC,
  ST_WAIT_FOR_SCHEDULE,
  //State for CPAN and distribution node to comit the request to receive a schedule
  ST_WAIT_FOR_DIST_COMMIT,
  //States for data dissemination
  ST_SCHEDULE_DIST,
  ST_SCHEDULE_OLD,
  ST_SCHEDULE_RETRANSMITTING,
  ST_SCHEDULE_RECEIVED,
  ST_SCHEDULE_INSTALLED,
  //During installation, ignore commands with this state
  ST_IGNORE_COMMANDS,
  //Marking the end of the enum
  ST_END,
};

enum commands
{
  //Commands for data gathering
  CM_NO_COMMAND,
  CM_ETX_METRIC_GET,
  CM_ETX_METRIC_MISSING,
  CM_ETX_METRIC_SEND,
  //Command for distribution node start
  CM_START_DIST_COMMIT,
  //Commands for data dissemination
  CM_SCHEDULE,
  CM_SCHEDULE_RETRANSMIT,
  CM_SCHEDULE_RETRANSMIT_REQ,
  CM_SCHEDULE_END,
  //Command signaling that application data is send
  CM_DATA,
  //Marking the end of the enum
  CM_END,
};

//The process to start once the association to the network is finished
PROCESS_NAME(distributor_callback);

/** Master's routing packet with "header" */
typedef struct __attribute__((packed)) master_routing_packet_t
{
  uint8_t flow_number; // use as sender in case of neighbor discovery
  uint16_t packet_number;
#if TSCH_TTL_BASED_RETRANSMISSIONS
  uint16_t ttl_slot_number;
  uint16_t earliest_tx_slot;
#endif /* TSCH_TTL_BASED_RETRANSMISSIONS*/
  uint8_t data[MASTER_MSG_LENGTH];
} master_routing_packet_t;

/** @brief The callback to an upper layer once a packet is received
 * 
 *  Only data packets that are not intended for the inner routing mechanism of MASTER
 *  will be forwarded to an upper layer
 * 
 *  @param data the buffer containing the data
 *  @param len the length of the data
 *  @param src the source of the packet
 *  @param dest the destination of the packet
 */
typedef void (* master_routing_input_callback)(const void *data, uint16_t len,
  uint8_t src, uint8_t dest);

/**
 * Set input callback for MasterRouting
 *
 * @param callback the callback to the upper layer
*/
void master_routing_set_input_callback(master_routing_input_callback callback);

/**
 * Callback after a beacon arrives with a bit-vector of nodes that received the schedule so far.
 *
 * @param schedule_received_from_others the bit-vector
 * @param len the length of the bit-vector
*/
void master_schedule_received_callback(uint8_t *schedule_received_from_others, uint8_t len);

/**
 * Callback after a beacon arrives. Check the version and request missing packets if the schedule version is 
 * higher than this nodes.
 *
 * @param nbr the neighbor that send the beacon
 * @param nbr_schedule_version the schedule version
 * @param nbr_schedule_packets the amount of packets the schedule consists of
*/
void master_schedule_difference_callback(linkaddr_t * nbr, uint8_t nbr_schedule_version, uint16_t nbr_schedule_packets);

/** @brief Get the correct transmission slot
 * 
 *  This function will return the correct slot for the initial schedule
 *  for each node when to transmit data. 
 * 
 *  @param id the node id
 *  @return The slot to transmit at as unsigned int
 */
uint8_t get_tx_slot(uint8_t id);

/** @brief Check if all nodes in the network received the schedule
 * 
 *  @param id the node id
 *  @return 1 if all nodes received the schedule. 0 otherwise.
 */
int check_received_schedules_by_nodes();

/** @brief Reset schedule received flags of neighbors.
 * 
 * During data gathering, we will mark neighbors whom we received
 * the schedule. This results in no duplicate packet transmissions
 * if an acknowledgement gets lost. Reset the flags for all neighbors
 * that indicates the reception of this data.
 * 
 *  @param id the node id
 *  @return 1 if all nodes received the schedule. 0 otherwise.
 */
void reset_nbr_metric_received();

/** @brief Get the current beacon slot for initial data gather and schedule distribution
 * 
 *  @return  The timeslot for the beacon
 */
uint8_t get_beacon_slot();

/** @brief Prepare the master_packetbuf_config_t for MASTER-NET 
 * 
 *  @param etx_link the current etx link to the destination
 *  @param command the command in the packet
 *  @param packet_number the packet number
 *  @param overhearing_active the flag if overhearing is active
 */
void setup_packet_configuration(uint16_t etx_link, uint8_t command, uint16_t packet_number, uint8_t overhearing_active);

/** @brief Fill a packet to transmit with a part of the schedule from flash memory.
 * 
 *  @param bytes_in_packet the bytes currently in the packet
 *  @param packet_number the packet number of the schedule
 *  @return 1 if this was the last packet. 0 if there is more data in the flash memory. 2 if an error occured during memory access.
 */
uint8_t fill_schedule_packet_from_flash(int bytes_in_packet, int packet_number);

/** @brief Write the schedule from a received packet to flash memory
 * 
 *  @param packet_len the received packet length
 *  @return 1 if this data was written to flash memory. 0 otherwise.
 */
uint8_t unpack_schedule_to_flash(int packet_len);

/** @brief Called from distributor node once the schedule is loaded and the node joined the network again.
 */
void master_schedule_loaded_callback();

/** @brief Install the new schedule at the node.
 */
void install_schedule();

/** @brief Change the link to a new destination if necessary
 * 
 *  @param link_type the link type
 *  @param dest the destination address
 *  @param timeslot the timeslot to change the link at
 */
void change_link(int link_type, const linkaddr_t* dest, uint16_t timeslot);

/** @brief Send a poll request to a neighbor to receive the etx-metric from this node and his neighbors.
 * 
 *  @param command the command to send
 *  @param dest the destination neighbor
 */
void convergcast_poll_neighbor(enum commands command, struct tsch_neighbor * dest);

/** @brief Forward a packet to the time source.
 */
void convergcast_forward_to_timesource();

/** @brief Depending on the state, send packets to time_source or poll a neighbor for their metric.
 */
void handle_convergcast();

/** @brief Log the received etx-metric.
 * 
 *  This will onyl be called by the CPAN and is required for MASTER to work!
 * 
 *  @param metric the buffer containing the metric
 *  @param metric_owner the node id of the owner of this metric
 *  @param len the length of the buffer
 */
void print_metric(uint8_t *metric, uint8_t metric_owner, uint16_t len);

/** @brief Put the already calculated metrics to an array for transmission.
 */
int calculate_etx_metric();

/** @brief Handle the case when retransmits are requested and the requested packet was transmitted.
 */
void handle_retransmit();

/** @brief Check for missing packets or finish searching for missing packets.
 * 
 *  If all packets are received, finish the schedule gathering. 
 *  Otherwise send next request for a missing packet
 * 
 *  @param destination the nestination address to request the retransmission at.
 */
void request_retransmit_or_finish(const linkaddr_t * destination);

/** @brief Change the state to waiting for a schedule again.
 * 
 *  This will be called after an timeout if the node does not receive a requested packet.
 */
void missing_response();

/** @brief Change the current state to a new state and log the change.
 * 
 *  @param new_state the new state
 */
void handle_state_change(enum phase new_state);

/** @brief Search for the next neighbor.
 * 
 *  Only neighbors will be selected, that have this node as their time source.
 * 
 *  @return 1 if a neighbor was found. 0 otherwise.
 */
int has_next_neighbor();

/** @brief Check if the packet is a schedule distribution packet.
 * 
 * Packets for the first distribution will arrive here and retransmits, as long as
 * the node does not know it has an old schedule.
 * Only packets that are received for the first time will be handled here.
 * 
 *  @param command the command in the packet
 *  @param len the length of the packet
 *  @return 1 if this was a schedule packet and handled. 0 otherwise.
 */
int handle_schedule_distribution_state_changes(enum commands command, uint16_t len);

/** @brief callback function after a schedule packet was sent.
 * 
 * Handle the callback for the initial schedule distribution by the distributor node.
 * The next packet will be prepared and sent. Other nodes do only forward but to not send
 * the next packet by themself.
 */
void callback_input_schedule_send();

/** @brief callback function after fpr the convergcast (data gathering)
 * 
 * Handle the callback during convergcast after a packet was sent. Depending on the next state,
 * send a POLL request, forward a schedule or enter waiting state for the schedule.
 * 
 * @param next_state the next state that the node will transition to
 */
void callback_convergcast(enum phase next_state);

/** @brief Pass the incomming data to the upper layer.
 * 
 * @param received_asn the asn this packet was received at
 * @param len the length of the packet
 */
void command_input_data_received(uint16_t received_asn, uint16_t len);

/** @brief Forward the packet to the next node
 * 
 * @param schedule the current schedule
 * @param received_asn the asn this packet was received at
 * @param len the length of the packet
 */
void command_input_data_forwarding(master_tsch_schedule_t* schedule, uint16_t received_asn, uint16_t len);

/** @brief Handle incomming retransmitted packets
 * 
 * @param len the length of the packet
 * @param src the source address of the packet
 * @param dest the destination address of the packet
 */
void command_input_schedule_retransmitt(uint16_t len, const linkaddr_t *src, const linkaddr_t *dest);

/** @brief Handle incomming retransmission requests for packets
 * 
 * @param src the source address of the requester
 */
void command_input_schedule_retransmitt_request(const linkaddr_t *src);

/** @brief Handle incomming schedule packets.
 * 
 * Unpack the packet to flash memory if this packet is missing.
 * 
 * @param len the length of the packet
 */
void command_input_schedule_new_packet(uint16_t len);

/** @brief Handle a continuing incomming schedule packet.
 * 
 * Unpack the packet to flash memory if this packet is missing.
 * Check if this node has the whole schedule.
 * 
 * @param len the length of the packet
 */
void command_input_schedule_packet(uint16_t len);

/** @brief Handle the last incomming schedule packet.
 * 
 * Unpack the packet to flash memory if this packet is missing.
 * Check if this node has the whole schedule.
 * 
 * @param len the length of the packet
 */
void command_input_schedule_last_packet(uint16_t len);

/** @brief Check if this node is one of the nodes that the etx-metric is missing from.
 *
 * @return 1 if we are one of the nodes. 0 otherwise.
 */
int missing_metric_from_myself();

/** @brief Check if a neighbor of this node is one of the missing nodes that the etx-metric is missing from.
 *
 * @return The neighbor that is missing.
 */
struct tsch_neighbor * has_missing_node_as_nbr();

/** @brief Handle incoming notification that some etx-metrics are missing.
 * 
 * Check if this node is one of the missing ones or if a neighbor is known.
 * Forward the broadcast otherwise.
 * 
 * @param len the length of the packet
 */
void command_input_missing_metric(int len);

/** @brief Prepare the distributor node for the schedule serial input.
 * The distributor node received a request from the CPAN to prepare the serial input for the schedule.
 * First the node needs to send a notification that it is ready.
 */
void command_input_prepare_distributor();

/** @brief Handle the poll from a neighbor node demanding the ETX-metric.
*/
void command_input_get_metric();

/** @brief Handle packet containing the ETX-metric of a node.
 * 
 * In case of a duplicate packet, it will not be handled.
 * 
 * @return 1 if the packet was handled. 0 otherwise.
*/
int command_input_send_metric(uint16_t len, const linkaddr_t *src);

/** @brief Behavior of the CPAN in case of a metric packet.
 * 
 * @param len the length of the packet
 * @param src the src address of the packet
*/
void command_input_send_metric_CPAN(uint16_t len, const linkaddr_t *src);

/** @brief Behavior of every node that is not the CPAN in case of a metric packet
 * 
 * @param len the length of the packet
 * @param src the src address of the packet
*/
void command_input_send_metric_Node(uint16_t len, const linkaddr_t *src);

/** @brief State machine behavior on callbacks.
 * 
 * This is the state machine transition behavior on a successfully sent packet.
 * 
 * @param command the command of the sent packet
 * @param has_packets_to_forward the indicator if there are packets to forward (required only during convergcast)
*/
void transition_to_new_state_after_callback(enum commands command, int has_packets_to_forward);

/** @brief State machine behavior on packet input.
 * 
 * This is the state machine transition behavior on a packets received over radio from other nodes.
 * 
 * @param command the command of the sent packet
 * @param len the length of the packet
 * @param src the source address of the packet
 * @param dest the destination address of the packet
 * @return 1 if the packet was handled during the current state. 0 otherwise.
*/
int transition_to_new_state(enum commands command, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest);

/** @brief Handle the input of a packet from Master-Net.
 * 
 * This is the entry into MASTER called from Master-Net.
 * 
 * @param data the data that is received
 * @param len the length of the packet
 * @param src the source address of the packet
 * @param dest the destination address of the packet
*/
void master_routing_input(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest);

/** @brief Callback for TSCH after a packet was sent.
 * 
 * @param data the data pointer (currently unused)
 * @param ret the return value to identify if the transmission was successful or not
 * @param transmissions the amount of transmission for the packet
*/
void master_routing_output_callback(void *data, int ret, int transmissions);

/** @brief Handle callbacks for the etx-metric gathering after a packet was sent.
 * 
 * @param command the command that was sent
 * @param ret the return value to identify if the transmission was successful or not
 * @param transmissions the amount of transmission for the packet
*/
void handle_callback_commands_divergcast(enum commands command, int ret, int transmissions);

/** @brief Handle callbacks for the schedule distribution after a packet was sent.
 * 
 * @param command the command that was sent
 * @param ret the return value to identify if the transmission was successful or not
 * @param transmissions the amount of transmission for the packet
*/
void handle_callback_commands_convergcast(enum commands command, int ret, int transmissions);

/** @brief Packet transmission called from the upper layer.
 * 
 * @param data the data buffer
 * @param datalen the length of the data buffer
 * @param receiver the receiver of the packet
 * @return 1 if the packet was sent. 0 otherwise.
*/
int master_routing_sendto(const void *data, uint16_t datalen, uint8_t receiver);

/** @brief Packet transmission.
 * 
 * @param data the data buffer
 * @param datalen the length of the data buffer
 * @return 1 if the packet was sent. 0 otherwise.
*/
int master_routing_send(const void *data, uint16_t datalen);

/** @brief Callback for Master-Net.
 * 
 * Master-Net can receive the packet configuration through this callback.
 * 
 * @return The packet configuration structure.
*/
master_packetbuf_config_t master_routing_sent_configuration();

/** @brief Check if the schedule is installed and routing is therefore configured
 * 
 * @return 1 if the schedule is installed. 0 otherwise.
*/
int master_routing_configured(void);

/**
 * @brief Initialize MASTER.
 * 
 */
void init_master_routing(void);

#endif /* MASTER_ROUTING_H */

/** @} */
/** @} */
