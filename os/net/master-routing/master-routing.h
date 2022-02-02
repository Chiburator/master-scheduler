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

enum phase
{
  ST_EB,
  ST_BEGIN_GATHER_METRIC,
  ST_POLL_NEIGHBOUR,
  ST_SEND_METRIC,
  ST_WAIT_FOR_SCHEDULE,
  ST_SCHEDULE_DIST,
  ST_SCHEDULE_INSTALLED,
  ST_SCHEDULE_OLD,
  ST_SCHEDULE_RETRANSMITTING,
  ST_END,
};

enum commands
{
  CM_NO_COMMAND,
  CM_GET_ETX_METRIC,
  CM_ETX_METRIC,
  CM_SCHEDULE,
  CM_SCHEDULE_RETRANSMIT,
  CM_SCHEDULE_RETRANSMIT_REQ,
  CM_SCHEDULE_END,
  CM_DATA,
  CM_END,
};

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

/**
 * Function prototype for MasterRouting input callback
*/
typedef void (* master_routing_input_callback)(const void *data, uint16_t len,
  uint8_t src, uint8_t dest);

typedef void (* mac_callback_t)(void *ptr, int status, int transmissions);

/**
 * Set input callback for MasterRouting
 *
 * \param callback The input callback
*/
void master_routing_set_input_callback(master_routing_input_callback callback);

/**
 * Set input callback for MasterRouting
 *
 * \param callback The input callback
*/
void master_routing_set_output_callback(mac_callback_t callback);

int node_is_sender(void);

int get_node_receiver(void);

int master_routing_configured(void);

//choose tx flow based on routing information
int master_routing_send(const void *data, uint16_t datalen);

int master_routing_sendto(const void *data, uint16_t datalen, uint8_t flow);

void init_master_routing(void);

void install_schedule();

#endif /* MASTER_ROUTING_H */

/** @} */
/** @} */
