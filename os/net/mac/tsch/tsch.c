/*
 * Copyright (c) 2015, SICS Swedish ICT.
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
 *         IEEE 802.15.4 TSCH MAC implementation.
 *         Does not use any RDC layer. Should be used with nordc.
 * \author
 *         Simon Duquennoy <simonduq@sics.se>
 *         Beshr Al Nahas <beshr@sics.se>
 *
 */

/**
 * \addtogroup tsch
 * @{
*/

#include "contiki.h"
#include "dev/radio.h"
#include "dev/leds.h"
#include "net/netstack.h"
#include "net/packetbuf.h"
#include "net/queuebuf.h"
#include "net/nbr-table.h"
#include "net/link-stats.h"
#include "net/mac/framer/framer-802154.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-slot-operation.h"
#include "net/mac/tsch/tsch-queue.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/tsch/tsch-log.h"
#include "net/mac/tsch/tsch-packet.h"
#include "net/mac/tsch/tsch-security.h"
#include "net/mac/mac-sequence.h"
#include "net/queuebuf.h"
#include "lib/random.h"

uint8_t tsch_change_time_source_active = 1;

#if UIP_CONF_IPV6_RPL
#include "net/mac/tsch/tsch-rpl.h"
#endif /* UIP_CONF_IPV6_RPL */

#if TSCH_WITH_SIXTOP
#include "net/mac/tsch/sixtop/sixtop.h"
#endif

#if FRAME802154_VERSION < FRAME802154_IEEE802154_2015
#error TSCH: FRAME802154_VERSION must be at least FRAME802154_IEEE802154_2015
#endif

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "TSCH"
#define LOG_LEVEL LOG_LEVEL_DBG

#if TSCH_DEBUG_PRINT
#include <stdio.h>
#endif /* TSCH_DEBUG_PRINT */

/* Use to collect link statistics even on Keep-Alive, even though they were
 * not sent from an upper layer and don't have a valid packet_sent callback */
#ifndef TSCH_LINK_NEIGHBOR_CALLBACK
#if NETSTACK_CONF_WITH_IPV6
void uip_ds6_link_neighbor_callback(int status, int numtx);
#define TSCH_LINK_NEIGHBOR_CALLBACK(dest, status, num) uip_ds6_link_neighbor_callback(status, num)
#endif /* NETSTACK_CONF_WITH_IPV6 */
#endif /* TSCH_LINK_NEIGHBOR_CALLBACK */

/* Use to perform neighbor discovery based EB packets sent */
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
extern void neighbor_discovery_input(const uint16_t *data, struct tsch_neighbor *nbr);
#endif /* TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY */

//#ifdef COMPETITION_RUN
//extern uint16_t net_processing_get_slotframe(void *dataptr);
//#endif /* COMPETITION_RUN */

// #if TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES
// extern uint8_t net_processing_get_flow_address_and_max_transmissions(linkaddr_t *flow_addr);
// #endif /* TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES */

/* The address of the last node we received an EB from (other than our time source).
 * Used for recovery */
static linkaddr_t last_eb_nbr_addr;
/* The join priority advertised by last_eb_nbr_addr */
static uint8_t last_eb_nbr_jp;

/* Let TSCH select a time source with no help of an upper layer.
 * We do so using statistics from incoming EBs */
#if TSCH_AUTOSELECT_TIME_SOURCE
int best_neighbor_eb_count;
struct eb_stat
{
  int rx_count;
  int jp;
};
NBR_TABLE(struct eb_stat, eb_stats);
#endif /* TSCH_AUTOSELECT_TIME_SOURCE */

uint8_t schedule_received[FRAME802154E_IE_SCHEDULE_BIT_VECTOR] = {0};

/* TSCH channel hopping sequence */
uint8_t tsch_hopping_sequence[TSCH_HOPPING_SEQUENCE_MAX_LEN];
struct tsch_asn_divisor_t tsch_hopping_sequence_length;

/* Default TSCH timeslot timing (in micro-second) */
static const uint16_t tsch_default_timing_us[tsch_ts_elements_count] = {
    TSCH_DEFAULT_TS_CCA_OFFSET,
    TSCH_DEFAULT_TS_CCA,
    TSCH_DEFAULT_TS_TX_OFFSET,
    TSCH_DEFAULT_TS_RX_OFFSET,
    TSCH_DEFAULT_TS_RX_ACK_DELAY,
    TSCH_DEFAULT_TS_TX_ACK_DELAY,
    TSCH_DEFAULT_TS_RX_WAIT,
    TSCH_DEFAULT_TS_ACK_WAIT,
    TSCH_DEFAULT_TS_RX_TX,
    TSCH_DEFAULT_TS_MAX_ACK,
    TSCH_DEFAULT_TS_MAX_TX,
    TSCH_DEFAULT_TS_TIMESLOT_LENGTH,
};
/* TSCH timeslot timing (in rtimer ticks) */
rtimer_clock_t tsch_timing[tsch_ts_elements_count];

#if LINKADDR_SIZE == 8
/* 802.15.4 broadcast MAC address  */
const linkaddr_t tsch_broadcast_address = {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
/* Address used for the EB virtual neighbor queue */
const linkaddr_t tsch_eb_address = {{0, 0, 0, 0, 0, 0, 0, 0}};
#else  /* LINKADDR_SIZE == 8 */
const linkaddr_t tsch_broadcast_address = {{0xff, 0xff}};
const linkaddr_t tsch_eb_address = {{0, 0}};
#endif /* LINKADDR_SIZE == 8 */

/* Is TSCH started? */
int tsch_is_started = 0;
/* Has TSCH initialization failed? */
int tsch_is_initialized = 0;
/* Are we coordinator of the TSCH network? */
int tsch_is_coordinator = 0;
/* Are we associated to a TSCH network? */
int tsch_is_associated = 0;
/* Total number of associations since boot */
int tsch_association_count = 0;
/* Is the PAN running link-layer security? */
int tsch_is_pan_secured = LLSEC802154_ENABLED;
/* The current Absolute Slot Number (ASN) */
struct tsch_asn_t tsch_current_asn;
/* Device rank or join priority:
 * For PAN coordinator: 0 -- lower is better */
uint8_t tsch_join_priority;
/* The current TSCH sequence number, used for unicast data frames only */
static uint8_t tsch_packet_seqno;
/* Current period for EB output */
static clock_time_t tsch_current_eb_period;
/* Current period for keepalive output */
static clock_time_t tsch_current_ka_timeout;

/* timer for sending keepalive messages */
static struct ctimer keepalive_timer;

/* Statistics on the current session */
unsigned long tx_count;
unsigned long rx_count;
unsigned long sync_count;

/* TSCH processes and protothreads */
PT_THREAD(tsch_scan(struct pt *pt));
PROCESS(tsch_process, "main process");
PROCESS(tsch_send_eb_process, "send EB process");
PROCESS(tsch_pending_events_process, "pending events process");

static master_routing_schedule_difference_callback schedule_difference_callback = NULL;
static master_callback_check_received_schedules schedule_received_callback = NULL;

process_event_t tsch_associated_to_network;

/* Other function prototypes */
static void packet_input(void);

/* Getters and setters */
/*---------------------------------------------------------------------------*/
void tsch_set_schedule_difference_callback(master_routing_schedule_difference_callback callback)
{
  schedule_difference_callback = callback;
}

void tsch_set_schedule_received_callback(master_callback_check_received_schedules callback)
{
  schedule_received_callback = callback;
}

void tsch_reset_schedule_received_callback()
{
  schedule_received_callback = NULL;
}
/*---------------------------------------------------------------------------*/
void tsch_set_coordinator(int enable)
{
  if (tsch_is_coordinator != enable)
  {
    tsch_is_associated = 0;
  }
  tsch_is_coordinator = enable;
  tsch_set_eb_period(TSCH_EB_PERIOD);
}
/*---------------------------------------------------------------------------*/
void tsch_set_rank(int rank)
{
  tsch_rank = rank;
}
/*---------------------------------------------------------------------------*/
void tsch_set_pan_secured(int enable)
{
  tsch_is_pan_secured = LLSEC802154_ENABLED && enable;
}
/*---------------------------------------------------------------------------*/
void tsch_set_join_priority(uint8_t jp)
{
  tsch_join_priority = jp;
}
/*---------------------------------------------------------------------------*/
void tsch_set_ka_timeout(uint32_t timeout)
{
  tsch_current_ka_timeout = timeout;
  if (timeout == 0)
  {
    ctimer_stop(&keepalive_timer);
  }
  else
  {
    tsch_schedule_keepalive();
  }
}
/*---------------------------------------------------------------------------*/
void tsch_set_eb_period(uint32_t period)
{
  tsch_current_eb_period = MIN(period, TSCH_MAX_EB_PERIOD);
}
/*---------------------------------------------------------------------------*/
static void
tsch_reset(void)
{
  int i;
  frame802154_set_pan_id(0xffff);
  /* First make sure pending packet callbacks are sent etc */
  process_post_synch(&tsch_pending_events_process, PROCESS_EVENT_POLL, NULL);
  /* Reset neighbor queues */
  tsch_queue_reset();
  /* Remove unused neighbors */
  tsch_queue_free_unused_neighbors();
  tsch_queue_update_time_source(NULL);
  /* Initialize global variables */
  tsch_join_priority = 0xff;
  TSCH_ASN_INIT(tsch_current_asn, 0, 0);
  current_link = NULL;
  /* Reset timeslot timing to defaults */
  for (i = 0; i < tsch_ts_elements_count; i++)
  {
    tsch_timing[i] = US_TO_RTIMERTICKS(tsch_default_timing_us[i]);
  }
#ifdef TSCH_CALLBACK_LEAVING_NETWORK
  TSCH_CALLBACK_LEAVING_NETWORK();
#endif
  linkaddr_copy(&last_eb_nbr_addr, &linkaddr_null);
#if TSCH_AUTOSELECT_TIME_SOURCE
  struct nbr_sync_stat *stat;
  best_neighbor_eb_count = 0;
  /* Remove all nbr stats */
  stat = nbr_table_head(eb_stats);
  while (stat != NULL)
  {
    nbr_table_remove(eb_stats, stat);
    stat = nbr_table_next(eb_stats, stat);
  }
#endif /* TSCH_AUTOSELECT_TIME_SOURCE */
  tsch_set_eb_period(TSCH_EB_PERIOD);
}
/* TSCH keep-alive functions */

/*---------------------------------------------------------------------------*/
/* Tx callback for keepalive messages */
static void
keepalive_packet_sent(void *ptr, int status, int transmissions)
{
  /* Update neighbor link statistics */
  link_stats_packet_sent(packetbuf_addr(PACKETBUF_ADDR_RECEIVER), status, transmissions);
  /* Call RPL callback if RPL is enabled */
#ifdef TSCH_CALLBACK_KA_SENT
  TSCH_CALLBACK_KA_SENT(status, transmissions);
#endif /* TSCH_CALLBACK_KA_SENT */
  LOG_INFO("KA sent to ");
  LOG_INFO_LLADDR(packetbuf_addr(PACKETBUF_ADDR_RECEIVER));
  LOG_INFO_(", st %d-%d\n", status, transmissions);

  /* We got no ack, try to recover by switching to the last neighbor we received an EB from */
  if (status != MAC_TX_OK)
  {
    if (linkaddr_cmp(&last_eb_nbr_addr, &linkaddr_null))
    {
      LOG_WARN("not able to re-synchronize, received no EB from other neighbors\n");
      if (sync_count == 0)
      {
        /* We got no synchronization at all in this session, leave the network */
        printf("Diss associate A\n");
        tsch_disassociate();
      }
    }
    else
    {
      LOG_WARN("re-synchronizing on ");
      LOG_WARN_LLADDR(&last_eb_nbr_addr);
      LOG_WARN_("\n");
#if TSCH_DEBUG_PRINT
      printf("re-synchronizing on %u\n", last_eb_nbr_addr.u8[0]);
#endif /* TSCH_DEBUG_PRINT */
      /* We simply pick the last neighbor we receiver sync information from */
      tsch_queue_update_time_source(&last_eb_nbr_addr);

    #if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
      struct tsch_neighbor * time_source = tsch_queue_get_time_source();
      tsch_rank = time_source->rank + 1;
      LOG_INFO("Increase rank to %d\n", tsch_rank);
    #endif

      tsch_join_priority = last_eb_nbr_jp + 1;
      /* Try to get in sync ASAP */
      tsch_schedule_keepalive_immediately();
      return;
    }
  }

  tsch_schedule_keepalive();
}
/*---------------------------------------------------------------------------*/
/* Prepare and send a keepalive message */
static void
keepalive_send(void *ptr)
{
  if (tsch_is_associated)
  {
    struct tsch_neighbor *n = tsch_queue_get_time_source();
    if (n != NULL)
    {
      /* Simply send an empty packet */
      packetbuf_clear();
      packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &n->addr);
      NETSTACK_MAC.send(keepalive_packet_sent, NULL);
      LOG_INFO("sending KA to ");
      LOG_INFO_LLADDR(&n->addr);
      LOG_INFO_("\n");
    }
    else
    {
      LOG_ERR("no timesource - KA not sent\n");
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Set ctimer to send a keepalive message after expiration of TSCH_KEEPALIVE_TIMEOUT */
void tsch_schedule_keepalive(void)
{
  /* Pick a delay in the range [tsch_current_ka_timeout*0.9, tsch_current_ka_timeout[ */
  if (!tsch_is_coordinator && tsch_is_associated && tsch_current_ka_timeout > 0)
  {
    unsigned long delay = (tsch_current_ka_timeout - tsch_current_ka_timeout / 10) + random_rand() % (tsch_current_ka_timeout / 10);
    ctimer_set(&keepalive_timer, delay, keepalive_send, NULL);
  }
}
/*---------------------------------------------------------------------------*/
/* Set ctimer to send a keepalive message immediately */
void tsch_schedule_keepalive_immediately(void)
{
  /* Pick a delay in the range [tsch_current_ka_timeout*0.9, tsch_current_ka_timeout[ */
  if (!tsch_is_coordinator && tsch_is_associated)
  {
    ctimer_set(&keepalive_timer, 0, keepalive_send, NULL);
  }
}
/*---------------------------------------------------------------------------*/

static void
eb_input(struct input_packet *current_input)
{
  /* LOG_INFO("EB received\n"); */
  frame802154_t frame;
  /* Verify incoming EB (does its ASN match our Rx time?),
   * and update our join priority. */
  struct ieee802154_ies eb_ies;

  if (tsch_packet_parse_eb(current_input->payload, current_input->len,
                           &frame, &eb_ies, NULL, 1))
  {

    /* PAN ID check and authentication done at rx time */

    //TODO: This does not do what is says it does!!! 
    
    /* Got an EB from a different neighbor than our time source, keep enough data
     * to switch to it in case we lose the link to our time source */
    struct tsch_neighbor *ts = tsch_queue_get_time_source();
    if (ts == NULL || !linkaddr_cmp(&last_eb_nbr_addr, &ts->addr))
    {
      linkaddr_copy(&last_eb_nbr_addr, (linkaddr_t *)&frame.src_addr);
      last_eb_nbr_jp = eb_ies.ie_join_priority;
    }

#if TSCH_AUTOSELECT_TIME_SOURCE
    if (!tsch_is_coordinator)
    {
      /* Maintain EB received counter for every neighbor */
      struct eb_stat *stat = (struct eb_stat *)nbr_table_get_from_lladdr(eb_stats, (linkaddr_t *)&frame.src_addr);
      if (stat == NULL)
      {
        stat = (struct eb_stat *)nbr_table_add_lladdr(eb_stats, (linkaddr_t *)&frame.src_addr, NBR_TABLE_REASON_MAC, NULL);
      }
      if (stat != NULL)
      {
        stat->rx_count++;
        stat->jp = eb_ies.ie_join_priority;
        best_neighbor_eb_count = MAX(best_neighbor_eb_count, stat->rx_count);
      }
      /* Select best time source */
      struct eb_stat *best_stat = NULL;
      stat = nbr_table_head(eb_stats);
      while (stat != NULL)
      {
        /* Is neighbor eligible as a time source? */
        if (stat->rx_count > best_neighbor_eb_count / 2)
        {
          if (best_stat == NULL ||
              stat->jp < best_stat->jp)
          {
            best_stat = stat;
          }
        }
        stat = nbr_table_next(eb_stats, stat);
      }
      /* Update time source */
      if (best_stat != NULL)
      {
        tsch_queue_update_time_source(nbr_table_get_lladdr(eb_stats, best_stat));
        tsch_join_priority = best_stat->jp + 1;
      }
    }
#endif /* TSCH_AUTOSELECT_TIME_SOURCE */

#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
    struct tsch_neighbor *n = tsch_queue_add_nbr((linkaddr_t *)&frame.src_addr);
    if(n == NULL)
    {
      LOG_ERR("Neighbor is null!\n");
    }else{
      neighbor_discovery_input(&eb_ies.ie_sequence_number, n);

      if(schedule_received_callback != NULL)
      {
        schedule_received_callback(eb_ies.ie_schedule_received, FRAME802154E_IE_SCHEDULE_BIT_VECTOR);
      }
    }

#endif

                    // TSCH_LOG_ADD(tsch_log_message,
                    //   snprintf(log->message, sizeof(log->message),
                    //   "NBR is %d", frame.src_addr[NODE_ID_INDEX]));
    /* Did the EB come from our time source? */
    if (ts != NULL && linkaddr_cmp((linkaddr_t *)&frame.src_addr, &ts->addr))
    {
      /* Check for ASN drift */
      int32_t asn_diff = TSCH_ASN_DIFF(current_input->rx_asn, eb_ies.ie_asn);
      if (asn_diff != 0)
      {
        /* We disagree with our time source's ASN -- leave the network */
        LOG_WARN("! ASN drifted by %ld, leaving the network\n", asn_diff);
        printf("Diss associate B\n");
        tsch_disassociate();
      }

      if (eb_ies.ie_join_priority >= TSCH_MAX_JOIN_PRIORITY)
      {
        /* Join priority unacceptable. Leave network. */
        LOG_WARN("! EB JP too high %u, leaving the network\n",
                 eb_ies.ie_join_priority);
        printf("Diss associate C\n");
        tsch_disassociate();
      }
      else
      {
#if TSCH_AUTOSELECT_TIME_SOURCE
        /* Update join priority */
        if (tsch_join_priority != eb_ies.ie_join_priority + 1)
        {
          LOG_INFO("update JP from EB %u -> %u\n",
                   tsch_join_priority, eb_ies.ie_join_priority + 1);
          tsch_join_priority = eb_ies.ie_join_priority + 1;
        }
#endif /* TSCH_AUTOSELECT_TIME_SOURCE */
      }
    }else{
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
      if(n != NULL)
      {

        n->rank = eb_ies.ie_rank;
        n->time_source = eb_ies.ie_time_source;
        
        //The coordinator does not need a time source, We requiere an etx-link to decive if we want to change to an neighbor,
        //we are not allowed to choose the distributor as the time source and the neighbor is not the current time source
        if((tsch_is_coordinator == 0) && 
           linkaddr_node_addr.u8[NODE_ID_INDEX] != MASTER_TSCH_DISTRIBUTOR &&
           (n->etx_link != 0) && 
           n->addr.u8[NODE_ID_INDEX] != MASTER_TSCH_DISTRIBUTOR && 
           !linkaddr_cmp(&n->addr, &tsch_queue_get_time_source()->addr))
        {

          //case 1. a beacon from a node with a better rank arrives. Only take this node as a time source if the etx_lin is not horrible
          //This will remove cases where a beacon rarely arrives at a far node
          //The last && makes sure that we dont switch a good link for a bad link just because we are 1 node closer to the cpan.
          uint8_t nbr_closer_to_cpan = (n->rank < tsch_queue_get_time_source()->rank) &&
                                       (n->etx_link < 2000);
          uint8_t nbr_is_better = (n->rank == tsch_rank) &&
                                  (n->etx_link < tsch_queue_get_time_source()->etx_link) &&
                                  (tsch_queue_get_time_source()->etx_link > 2000);

          //Online change during beacon phases allowed. otherwise metric gathering can break.
          if(tsch_change_time_source_active && (nbr_closer_to_cpan || nbr_is_better))
          {
            LOG_ERR("Got rank - conditions 1 or 2 %d %d\n", nbr_closer_to_cpan, nbr_is_better);
            LOG_ERR("Got rank %u (my rank %u) from src %u. link old vs new %d - %d . Switch from %d and set my rank to %u. nbrs = %d\n", 
            eb_ies.ie_rank, tsch_rank, frame.src_addr[NODE_ID_INDEX], tsch_queue_get_time_source()->etx_link, n->etx_link, tsch_queue_get_time_source()->addr.u8[NODE_ID_INDEX], eb_ies.ie_rank + 1, tsch_queue_count_nbr());
            tsch_queue_update_time_source((linkaddr_t *)&frame.src_addr);
            tsch_rank = eb_ies.ie_rank + 1;
          }
        }
        if(eb_ies.ie_schedule_version != schedule_version)
        {
          //printf("nbr %i\n", frame.src_addr[NODE_ID_INDEX]);
          schedule_difference_callback((linkaddr_t *)&frame.src_addr, eb_ies.ie_schedule_version, eb_ies.ie_schedule_packets);
        }
      }
    }
    #endif
  }else{
      LOG_ERR("SOMETHING wrong with eb");
    }
}
/*---------------------------------------------------------------------------*/
extern void neighbor_discovery_input(const uint16_t *data, struct tsch_neighbor *nbr)
{
  if(nbr == NULL)
  {
    LOG_ERR("NULL PTR when getting NBR for discovery. NBR size = %d from %d\n", tsch_queue_count_nbr(), TSCH_QUEUE_MAX_NEIGHBOR_QUEUES);
    return;
  }

  if(*data == nbr->last_eb)
  {
    return;
  }

  nbr->missed_ebs += (*data - (nbr->last_eb + 1));
  nbr->last_eb = *data;
  nbr->count++;

  //In case of a reaaaaallly bad link quality, we might run into an overflow for the uint16_t. just throw away everything worse than an ETX of 30
  int current_etx = (int) (1000 * (1.0 / ((float)nbr->count / nbr->last_eb)));
  if(current_etx > 30000)
  {
    current_etx = 25555;
  }
    
  nbr->etx_link = current_etx;
}
/*---------------------------------------------------------------------------*/
/* Process pending input packet(s) */
static void
tsch_rx_process_pending()
{
  int16_t input_index;
  /* Loop on accessing (without removing) a pending input packet */
  while ((input_index = ringbufindex_peek_get(&input_ringbuf)) != -1)
  {
    struct input_packet *current_input = &input_array[input_index];
    frame802154_t frame;
    uint8_t ret = frame802154_parse(current_input->payload, current_input->len, &frame);
    int is_data = ret && frame.fcf.frame_type == FRAME802154_DATAFRAME;
    int is_eb = ret && frame.fcf.frame_version == FRAME802154_IEEE802154_2015 && frame.fcf.frame_type == FRAME802154_BEACONFRAME;

    //LOG_ERR("hdr size %d, data %d, eb %d\n", ret, is_data, is_eb);
    if (is_data)
    {
      // TSCH_LOG_ADD(tsch_log_message,
      //              snprintf(log->message, sizeof(log->message),
      //                       "[    RX SLOT:    ] {asn-%x.%lx link-%u-%u-%u}",
      //                       current_input->rx_asn.ms1b, (unsigned long)current_input->rx_asn.ls4b,
      //                       current_link->slotframe_handle, current_link->timeslot, current_link->channel_offset));
      //LOG_ERR("[    RX SLOT:    ] {asn-%x.%lx link-%u-%u-%u}\n",
      //  current_input->rx_asn.ms1b, current_input->rx_asn.ls4b,
      //  current_link->slotframe_handle, current_link->timeslot, current_link->channel_offset);
      /* Skip EBs and other control messages */
      /* Copy to packetbuf for processing */

      // If IE's are present, overwritte them before copying the payload into the packetbuffer
      //LOG_ERR("Is ie list present? %d\n", frame.fcf.ie_list_present);
      if(frame.fcf.ie_list_present)
      {
      /* Calculate space needed for the security MIC, if any, before attempting to parse IEs */
          int mic_len = 0;
#if LLSEC802154_ENABLED
          if (!frame_without_mic)
          {
            mic_len = tsch_security_mic_len(&frame);
            if (buf_size < curr_len + mic_len)
            {
              return 0;
            }
          }
#endif /* LLSEC802154_ENABLED */
          /* Parse information elements. We need to substract the MIC length, as the exact payload len is needed while parsing */
          //ret_value should be all IE's in len
          struct ieee802154_ies ies;
          memset(&ies, 0, sizeof(struct ieee802154_ies));
          uint8_t ie_len = 0;
          if ((ie_len = frame802154e_parse_information_elements(current_input->payload + ret, current_input->len - ret - mic_len, &ies)) == -1)
          {
            LOG_ERR("! parse_ies: failed to parse IEs\n");
            ringbufindex_get(&input_ringbuf);
            continue;
          }else{
            //LOG_ERR("Trunacte packet and set payload from %d to %d, hdr = %d\n", current_input->len, current_input->len - ie_len, ret);
            current_input->len -= ie_len;
            memmove(&current_input->payload[ret], &current_input->payload[ret + ie_len], current_input->len - ret);
          }
      }

      packetbuf_copyfrom(current_input->payload, current_input->len);
      packetbuf_set_attr(PACKETBUF_ATTR_RSSI, current_input->rssi);
      packetbuf_set_attr(PACKETBUF_ATTR_CHANNEL, current_input->channel);
#if TSCH_WITH_CENTRAL_SCHEDULING
      packetbuf_set_attr(PACKETBUF_ATTR_RECEIVED_ASN, (uint16_t)current_input->rx_asn.ls4b);
#endif /* TSCH_WITH_CENTRAL_SCHEDULING */

      /* Pass to upper layers */
      packet_input();
    } else if (is_eb)
    {
      eb_input(current_input);
    }

    /* Remove input from ringbuf */
    ringbufindex_get(&input_ringbuf);
  }
  //printf("tsch_rx_process_pending \n");
}
/*---------------------------------------------------------------------------*/
/* Pass sent packets to upper layer */
static void
tsch_tx_process_pending(void)
{
  int16_t dequeued_index;
  /* Loop on accessing (without removing) a pending input packet */
  while ((dequeued_index = ringbufindex_peek_get(&dequeued_ringbuf)) != -1)
  {
    struct tsch_packet *p = dequeued_array[dequeued_index];
    /* Put packet into packetbuf for packet_sent callback */

    queuebuf_to_packetbuf(p->qb);
    // LOG_INFO("packet sent to ");
    // LOG_INFO_LLADDR(packetbuf_addr(PACKETBUF_ADDR_RECEIVER));
    // LOG_INFO_(", seqno %u, status %d, tx %d\n",
    //           packetbuf_attr(PACKETBUF_ATTR_MAC_SEQNO), p->ret, p->transmissions);
    mac_call_sent_callback(p->sent, p->ptr, p->ret, p->transmissions);
    /* Free packet queuebuf */
    tsch_queue_free_packet(p);
    /* Free all unused neighbors */
  #ifndef TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
    tsch_queue_free_unused_neighbors();
  #endif
    /* Remove dequeued packet from ringbuf */
    ringbufindex_get(&dequeued_ringbuf);
  }
}
/*---------------------------------------------------------------------------*/
/* Setup TSCH as a coordinator */
static void
tsch_start_coordinator(void)
{
  frame802154_set_pan_id(IEEE802154_PANID);
  /* Initialize hopping sequence as default */
  memcpy(tsch_hopping_sequence, TSCH_DEFAULT_HOPPING_SEQUENCE, sizeof(TSCH_DEFAULT_HOPPING_SEQUENCE));
  TSCH_ASN_DIVISOR_INIT(tsch_hopping_sequence_length, sizeof(TSCH_DEFAULT_HOPPING_SEQUENCE));
#if TSCH_SCHEDULE_WITH_6TISCH_MINIMAL
  tsch_schedule_create_minimal();
#endif

  tsch_is_associated = 1;
  tsch_join_priority = 0;

  LOG_INFO("starting as coordinator, PAN ID %x, asn-%x.%lx\n",
           frame802154_get_pan_id(), tsch_current_asn.ms1b, tsch_current_asn.ls4b);

  /* Start slot operation */
  tsch_slot_operation_sync(RTIMER_NOW(), &tsch_current_asn);
}
/*---------------------------------------------------------------------------*/
/* Leave the TSCH network */
void tsch_disassociate(void)
{
  if (tsch_is_associated == 1)
  {
    tsch_is_associated = 0;
    process_post(&tsch_process, PROCESS_EVENT_POLL, NULL);
  }
}

void tsch_disassociate_synch(void)
{
  if (tsch_is_associated == 1)
  {
    tsch_is_associated = 0;
    //We require a synchronized process post to make sure that we first disassociate before we start any other operation
    //This is only used by the distributor node
    process_post_synch(&tsch_process, PROCESS_EVENT_POLL, NULL);
  }
}
/*---------------------------------------------------------------------------*/
/* Attempt to associate to a network form an incoming EB */
static int
tsch_associate(const struct input_packet *input_eb, rtimer_clock_t timestamp)
{
  frame802154_t frame;
  struct ieee802154_ies ies;
  uint8_t hdrlen;
  int i;

  if (input_eb == NULL || tsch_packet_parse_eb(input_eb->payload, input_eb->len,
                                               &frame, &ies, &hdrlen, 0) == 0)
  {
    LOG_ERR("! failed to parse EB (len %u)\n", input_eb->len);
    return 0;
  }

#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
  //We dont want to connect to the distributor since he will leave the networks
  if(frame.src_addr[NODE_ID_INDEX] == MASTER_TSCH_DISTRIBUTOR)
  {
    LOG_INFO("Dont associate on beacon from distributor node\n");
    return 0;
  }

  //We are the distributor node. only conect to CPAN
  if(linkaddr_node_addr.u8[NODE_ID_INDEX] == MASTER_TSCH_DISTRIBUTOR)
  {
    if(frame.src_addr[NODE_ID_INDEX] != MASTER_TSCH_COORDINATOR)
    {
      LOG_INFO("As the distributor, i only assoicate with the CPAN");
      return 0;
    }
  }
#endif
  tsch_current_asn = ies.ie_asn;
  tsch_join_priority = ies.ie_join_priority + 1;

#if TSCH_JOIN_SECURED_ONLY
  if (frame.fcf.security_enabled == 0)
  {
    LOG_ERR("! parse_eb: EB is not secured\n");
    return 0;
  }
#endif /* TSCH_JOIN_SECURED_ONLY */
#if LLSEC802154_ENABLED
  if (!tsch_security_parse_frame(input_eb->payload, hdrlen,
                                 input_eb->len - hdrlen - tsch_security_mic_len(&frame),
                                 &frame, (linkaddr_t *)&frame.src_addr, &tsch_current_asn))
  {
    LOG_ERR("! parse_eb: failed to authenticate\n");
    return 0;
  }
#endif /* LLSEC802154_ENABLED */

#if !LLSEC802154_ENABLED
  if (frame.fcf.security_enabled == 1)
  {
    LOG_ERR("! parse_eb: we do not support security, but EB is secured\n");
    return 0;
  }
#endif /* !LLSEC802154_ENABLED */

#if TSCH_JOIN_MY_PANID_ONLY
  /* Check if the EB comes from the PAN ID we expect */
  if (frame.src_pid != IEEE802154_PANID)
  {
    LOG_ERR("! parse_eb: PAN ID %x != %x\n", frame.src_pid, IEEE802154_PANID);
    return 0;
  }
  LOG_INFO("Got this pid %x from src %i", frame.src_pid, frame.src_addr[0]);
#endif /* TSCH_JOIN_MY_PANID_ONLY */

  /* There was no join priority (or 0xff) in the EB, do not join */
  if (ies.ie_join_priority == 0xff)
  {
    LOG_ERR("! parse_eb: no join priority\n");
    return 0;
  }

  /* TSCH timeslot timing */
  for (i = 0; i < tsch_ts_elements_count; i++)
  {
    if (ies.ie_tsch_timeslot_id == 0)
    {
      tsch_timing[i] = US_TO_RTIMERTICKS(tsch_default_timing_us[i]);
    }
    else
    {
      tsch_timing[i] = US_TO_RTIMERTICKS(ies.ie_tsch_timeslot[i]);
    }
  }

  /* TSCH hopping sequence */
  if (ies.ie_channel_hopping_sequence_id == 0)
  {
    memcpy(tsch_hopping_sequence, TSCH_DEFAULT_HOPPING_SEQUENCE, sizeof(TSCH_DEFAULT_HOPPING_SEQUENCE));
    TSCH_ASN_DIVISOR_INIT(tsch_hopping_sequence_length, sizeof(TSCH_DEFAULT_HOPPING_SEQUENCE));
  }
  else
  {
    if (ies.ie_hopping_sequence_len <= sizeof(tsch_hopping_sequence))
    {
      memcpy(tsch_hopping_sequence, ies.ie_hopping_sequence_list, ies.ie_hopping_sequence_len);
      TSCH_ASN_DIVISOR_INIT(tsch_hopping_sequence_length, ies.ie_hopping_sequence_len);
    }
    else
    {
      LOG_ERR("! parse_eb: hopping sequence too long (%u)\n", ies.ie_hopping_sequence_len);
      return 0;
    }
  }

#if TSCH_CHECK_TIME_AT_ASSOCIATION > 0
  /* Divide by 4k and multiply again to avoid integer overflow */
  uint32_t expected_asn = 4096 * TSCH_CLOCK_TO_SLOTS(clock_time() / 4096, tsch_timing_timeslot_length); /* Expected ASN based on our current time*/
  int32_t asn_threshold = TSCH_CHECK_TIME_AT_ASSOCIATION * 60ul * TSCH_CLOCK_TO_SLOTS(CLOCK_SECOND, tsch_timing_timeslot_length);
  int32_t asn_diff = (int32_t)tsch_current_asn.ls4b - expected_asn;
  if (asn_diff > asn_threshold)
  {
    LOG_ERR("! EB ASN rejected %lx %lx %ld\n",
            tsch_current_asn.ls4b, expected_asn, asn_diff);
    return 0;
  }
#endif

#if TSCH_INIT_SCHEDULE_FROM_EB
  /* Create schedule */
  if (ies.ie_tsch_slotframe_and_link.num_slotframes == 0)
  {
#if TSCH_SCHEDULE_WITH_6TISCH_MINIMAL
    LOG_INFO("parse_eb: no schedule, setting up minimal schedule\n");
    tsch_schedule_create_minimal();
#else
    LOG_INFO("parse_eb: no schedule\n");
#endif
  }
  else
  {
    /* First, empty current schedule */
    tsch_schedule_remove_all_slotframes();
    /* We support only 0 or 1 slotframe in this IE */
    int num_links = ies.ie_tsch_slotframe_and_link.num_links;
    if (num_links <= FRAME802154E_IE_MAX_LINKS)
    {
      int i;
      struct tsch_slotframe *sf = tsch_schedule_add_slotframe(
          ies.ie_tsch_slotframe_and_link.slotframe_handle,
          ies.ie_tsch_slotframe_and_link.slotframe_size);
      for (i = 0; i < num_links; i++)
      {
        tsch_schedule_add_link(sf,
                               ies.ie_tsch_slotframe_and_link.links[i].link_options,
                               LINK_TYPE_ADVERTISING, &tsch_broadcast_address,
                               ies.ie_tsch_slotframe_and_link.links[i].timeslot, ies.ie_tsch_slotframe_and_link.links[i].channel_offset);
      }
    }
    else
    {
      LOG_ERR("! parse_eb: too many links in schedule (%u)\n", num_links);
      return 0;
    }
  }
#endif /* TSCH_INIT_SCHEDULE_FROM_EB */

  if (tsch_join_priority < TSCH_MAX_JOIN_PRIORITY)
  {
    struct tsch_neighbor *n;

    /* Add coordinator to list of neighbors, lock the entry */
    n = tsch_queue_add_nbr((linkaddr_t *)&frame.src_addr);

    if (n != NULL)
    {
      tsch_queue_update_time_source((linkaddr_t *)&frame.src_addr);

    #if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
      n->rank = ies.ie_rank;
      tsch_rank = ies.ie_rank + 1;
      LOG_ERR("Got rank %u from src %d and set my rank to %u, nbrs = %d\n", ies.ie_rank, frame.src_addr[NODE_ID_INDEX], tsch_rank, tsch_queue_count_nbr());
      neighbor_discovery_input(&ies.ie_sequence_number, n);
    #endif

      /* Set PANID */
      frame802154_set_pan_id(frame.src_pid);

      /* Synchronize on EB */
      tsch_slot_operation_sync(timestamp - tsch_timing[tsch_ts_tx_offset], &tsch_current_asn);

      /* Update global flags */
      tsch_is_associated = 1;
      tsch_is_pan_secured = frame.fcf.security_enabled;
      tx_count = 0;
      rx_count = 0;
      sync_count = 0;

      /* Start sending keep-alives now that tsch_is_associated is set */
      tsch_schedule_keepalive();

#ifdef TSCH_CALLBACK_JOINING_NETWORK
      TSCH_CALLBACK_JOINING_NETWORK();
#endif

      tsch_association_count++;
      LOG_INFO("association done (%u), sec %u, PAN ID %x, asn-%x.%lx, jp %u, timeslot id %u, hopping id %u, slotframe len %u with %u links, from ",
               tsch_association_count,
               tsch_is_pan_secured,
               frame.src_pid,
               tsch_current_asn.ms1b, tsch_current_asn.ls4b, tsch_join_priority,
               ies.ie_tsch_timeslot_id,
               ies.ie_channel_hopping_sequence_id,
               ies.ie_tsch_slotframe_and_link.slotframe_size,
               ies.ie_tsch_slotframe_and_link.num_links);
      LOG_INFO_LLADDR((const linkaddr_t *)&frame.src_addr);
      LOG_INFO_("\n");
#if TSCH_DEBUG_PRINT
      printf("association done (%u), PAN ID %x, asn-%x.%lx, jp %u, timeslot id %u, hopping id %u, slotframe len %u with %u links, from %u\n",
             tsch_association_count,
             frame.src_pid,
             tsch_current_asn.ms1b, tsch_current_asn.ls4b, tsch_join_priority,
             ies.ie_tsch_timeslot_id,
             ies.ie_channel_hopping_sequence_id,
             ies.ie_tsch_slotframe_and_link.slotframe_size,
             ies.ie_tsch_slotframe_and_link.num_links,
             frame.src_addr[0]);
#endif /* TSCH_DEBUG_PRINT */

      tsch_associated_to_network = process_alloc_event();
      process_post(PROCESS_BROADCAST, tsch_associated_to_network, NULL);
      return 1;
    }
  }
  LOG_ERR("! did not associate.\n");
  return 0;
}
/* Processes and protothreads used by TSCH */

/*---------------------------------------------------------------------------*/
/* Scanning protothread, called by tsch_process:
 * Listen to different channels, and when receiving an EB,
 * attempt to associate.
 */
PT_THREAD(tsch_scan(struct pt *pt))
{
  PT_BEGIN(pt);

  static struct input_packet input_eb;
  static struct etimer scan_timer;
  /* Time when we started scanning on current_channel */
  static clock_time_t current_channel_since;

  TSCH_ASN_INIT(tsch_current_asn, 0, 0);

  etimer_set(&scan_timer, CLOCK_SECOND / TSCH_ASSOCIATION_POLL_FREQUENCY);
  current_channel_since = clock_time();

  while (!tsch_is_associated && !tsch_is_coordinator)
  {
    /* Hop to any channel offset */
    static uint8_t current_channel = 0;

    /* We are not coordinator, try to associate */
    rtimer_clock_t t0;
    int is_packet_pending = 0;
    clock_time_t now_time = clock_time();

    /* Switch to a (new) channel for scanning */
    if (current_channel == 0 || now_time - current_channel_since > TSCH_CHANNEL_SCAN_DURATION)
    {
      /* Pick a channel at random in TSCH_JOIN_HOPPING_SEQUENCE */
      uint8_t scan_channel = TSCH_JOIN_HOPPING_SEQUENCE[random_rand() % sizeof(TSCH_JOIN_HOPPING_SEQUENCE)];
      if (current_channel != scan_channel)
      {
        NETSTACK_RADIO.set_value(RADIO_PARAM_CHANNEL, scan_channel);
        current_channel = scan_channel;
        LOG_INFO("scanning on channel %u\n", scan_channel);
      }
      current_channel_since = now_time;
    }

    /* Turn radio on and wait for EB */
    NETSTACK_RADIO.on();

    is_packet_pending = NETSTACK_RADIO.pending_packet();
    if (!is_packet_pending && NETSTACK_RADIO.receiving_packet())
    {
      /* If we are currently receiving a packet, wait until end of reception */
      t0 = RTIMER_NOW();
      BUSYWAIT_UNTIL_ABS((is_packet_pending = NETSTACK_RADIO.pending_packet()), t0, RTIMER_SECOND / 100);
    }

    if (is_packet_pending)
    {
      /* Read packet */
      input_eb.len = NETSTACK_RADIO.read(input_eb.payload, TSCH_PACKET_MAX_LEN);

      /* Save packet timestamp */
      NETSTACK_RADIO.get_object(RADIO_PARAM_LAST_PACKET_TIMESTAMP, &t0, sizeof(rtimer_clock_t));

      /* Parse EB and attempt to associate */
      LOG_INFO("scan: received packet (%u bytes) on channel %u\n", input_eb.len, current_channel);

      tsch_associate(&input_eb, t0);
    }

    if (tsch_is_associated)
    {
      /* End of association, turn the radio off */
      NETSTACK_RADIO.off();
    }
    else if (!tsch_is_coordinator)
    {
      /* Go back to scanning */
      etimer_reset(&scan_timer);
      PT_WAIT_UNTIL(pt, etimer_expired(&scan_timer));
    }
  }
  PT_END(pt);
}

/*---------------------------------------------------------------------------*/
/* The main TSCH process */
PROCESS_THREAD(tsch_process, ev, data)
{
  static struct pt scan_pt;

  PROCESS_BEGIN();

  while (1)
  {
    while (!tsch_is_associated)
    {
      if (tsch_is_coordinator)
      {
        /* We are coordinator, start operating now */
        tsch_start_coordinator();
      }
      else
      {
        /* Start scanning, will attempt to join when receiving an EB */
        PROCESS_PT_SPAWN(&scan_pt, tsch_scan(&scan_pt));
      }
    }

    /* We are part of a TSCH network, start slot operation */
    tsch_slot_operation_start();

    /* Yield our main process. Slot operation will re-schedule itself
     * as long as we are associated */
    PROCESS_YIELD_UNTIL(!tsch_is_associated);
    
    LOG_WARN("leaving the network, stats: tx %lu, rx %lu, sync %lu\n",
             tx_count, rx_count, sync_count);
#if TSCH_DEBUG_PRINT
    printf("leaving the network, stats: tx %lu, rx %lu, sync %lu\n",
           tx_count, rx_count, sync_count);
#endif /* TSCH_DEBUG_PRINT */

    /* Will need to re-synchronize */
    printf("left netwrok\n");
    tsch_reset();
  }

  PROCESS_END();
}

uint8_t dont_inc_first_beacons = 10;
/*---------------------------------------------------------------------------*/
/* A periodic process to send TSCH Enhanced Beacons (EB) */
PROCESS_THREAD(tsch_send_eb_process, ev, data)
{
  static struct etimer eb_timer;
  PROCESS_BEGIN();

  /* Wait until association */
  etimer_set(&eb_timer, CLOCK_SECOND / 10);
  while (!tsch_is_associated)
  {
    PROCESS_WAIT_UNTIL(etimer_expired(&eb_timer));
    etimer_reset(&eb_timer);
  }

  /* Set an initial delay except for coordinator, which should send an EB asap */
  if (!tsch_is_coordinator)
  {
    etimer_set(&eb_timer, random_rand() % TSCH_EB_PERIOD); // is correct!!
    PROCESS_WAIT_UNTIL(etimer_expired(&eb_timer));
  }

  while (1)
  {
    unsigned long delay;

    if (tsch_is_associated && tsch_current_eb_period > 0)
    {
      /* Enqueue EB only if there isn't already one in queue */
      if (tsch_queue_packet_count(&tsch_eb_address) == 0)
      {
        leds_on(LEDS_RED);

        uint8_t hdr_len = 0;
        uint8_t tsch_sync_ie_offset;
        /* Prepare the EB packet and schedule it to be sent */
        if (tsch_packet_create_eb(&hdr_len, &tsch_sync_ie_offset) > 0)
        {
          struct tsch_packet *p;

          /* Enqueue EB packet, for a single transmission only */
          if (!(p = tsch_queue_add_packet(&tsch_eb_address, 1, NULL, NULL)))
          {
            LOG_ERR("! could not enqueue EB packet\n");
#if TSCH_DEBUG_PRINT
            printf("! could not enqueue EB packet\n");
#endif /* TSCH_DEBUG_PRINT */
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
            sequence_number--;
#endif
          }
          else
          {
#if TSCH_DEBUG_PRINT
            printf("TSCH: enqueue EB packet %u %u\n",
                   packetbuf_totlen(), packetbuf_hdrlen());
            //++num_eb_packets;
#endif /* TSCH_DEBUG_PRINT */
            p->tsch_sync_ie_offset = tsch_sync_ie_offset;
            p->header_len = hdr_len;

            if(dont_inc_first_beacons > 0)
            {
              sequence_number--;
              dont_inc_first_beacons--;
            }
          }
        }
        leds_off(LEDS_RED);
      }
    }

    if (tsch_current_eb_period > 0)
    {
      /* Next EB transmission with a random delay
       * within [tsch_current_eb_period*0.75, tsch_current_eb_period[ */
      delay = (tsch_current_eb_period - tsch_current_eb_period / 4) + random_rand() % (tsch_current_eb_period / 4);
    }
    else
    {
      delay = TSCH_EB_PERIOD;
    }
    etimer_set(&eb_timer, delay);
    PROCESS_WAIT_UNTIL(etimer_expired(&eb_timer));
  }
  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/* A process that is polled from interrupt and calls tx/rx input
 * callbacks, outputs pending logs. */
PROCESS_THREAD(tsch_pending_events_process, ev, data)
{
  PROCESS_BEGIN();
  while (1)
  {
    PROCESS_YIELD_UNTIL(ev == PROCESS_EVENT_POLL);
    tsch_rx_process_pending();
    tsch_tx_process_pending();
    tsch_log_process_pending();
  }
  PROCESS_END();
}

/* Functions from the Contiki MAC layer driver interface */

/*---------------------------------------------------------------------------*/
static void
tsch_init(void)
{
  radio_value_t radio_rx_mode;
  radio_value_t radio_tx_mode;
  rtimer_clock_t t;

  /* Radio Rx mode */
  if (NETSTACK_RADIO.get_value(RADIO_PARAM_RX_MODE, &radio_rx_mode) != RADIO_RESULT_OK)
  {
    LOG_ERR("! radio does not support getting RADIO_PARAM_RX_MODE. Abort init.\n");
    return;
  }
  /* Disable radio in frame filtering */
  radio_rx_mode &= ~RADIO_RX_MODE_ADDRESS_FILTER;
  /* Unset autoack */
  radio_rx_mode &= ~RADIO_RX_MODE_AUTOACK;
  /* Set radio in poll mode */
  radio_rx_mode |= RADIO_RX_MODE_POLL_MODE;
  if (NETSTACK_RADIO.set_value(RADIO_PARAM_RX_MODE, radio_rx_mode) != RADIO_RESULT_OK)
  {
    LOG_ERR("! radio does not support setting required RADIO_PARAM_RX_MODE. Abort init.\n");
    return;
  }

  /* Radio Tx mode */
  if (NETSTACK_RADIO.get_value(RADIO_PARAM_TX_MODE, &radio_tx_mode) != RADIO_RESULT_OK)
  {
    LOG_ERR("! radio does not support getting RADIO_PARAM_TX_MODE. Abort init.\n");
    return;
  }
  /* Unset CCA */
  radio_tx_mode &= ~RADIO_TX_MODE_SEND_ON_CCA;
  if (NETSTACK_RADIO.set_value(RADIO_PARAM_TX_MODE, radio_tx_mode) != RADIO_RESULT_OK)
  {
    LOG_ERR("! radio does not support setting required RADIO_PARAM_TX_MODE. Abort init.\n");
    return;
  }
  /* Test setting channel */
  if (NETSTACK_RADIO.set_value(RADIO_PARAM_CHANNEL, TSCH_DEFAULT_HOPPING_SEQUENCE[0]) != RADIO_RESULT_OK)
  {
    LOG_ERR("! radio does not support setting channel. Abort init.\n");
    return;
  }
  /* Test getting timestamp */
  if (NETSTACK_RADIO.get_object(RADIO_PARAM_LAST_PACKET_TIMESTAMP, &t, sizeof(rtimer_clock_t)) != RADIO_RESULT_OK)
  {
    LOG_ERR("! radio does not support getting last packet timestamp. Abort init.\n");
    return;
  }
  /* Check max hopping sequence length vs default sequence length */
  if (TSCH_HOPPING_SEQUENCE_MAX_LEN < sizeof(TSCH_DEFAULT_HOPPING_SEQUENCE))
  {
    LOG_ERR("! TSCH_HOPPING_SEQUENCE_MAX_LEN < sizeof(TSCH_DEFAULT_HOPPING_SEQUENCE). Abort init.\n");
  }

  /* Init the queuebuf and TSCH sub-modules */
  queuebuf_init();
  tsch_reset();
  tsch_queue_init();
  tsch_schedule_init();
  tsch_log_init();
  ringbufindex_init(&input_ringbuf, TSCH_MAX_INCOMING_PACKETS);
  ringbufindex_init(&dequeued_ringbuf, TSCH_DEQUEUED_ARRAY_SIZE);
#if TSCH_AUTOSELECT_TIME_SOURCE
  nbr_table_register(eb_stats, NULL);
#endif /* TSCH_AUTOSELECT_TIME_SOURCE */

  tsch_packet_seqno = random_rand();
  tsch_is_initialized = 1;

#if TSCH_AUTOSTART
  /* Start TSCH operation.
   * If TSCH_AUTOSTART is not set, one needs to call NETSTACK_MAC.on() to start TSCH. */
  NETSTACK_MAC.on();
#endif /* TSCH_AUTOSTART */

#if TSCH_WITH_SIXTOP
  sixtop_init();
#endif
}
/*---------------------------------------------------------------------------*/
/* Function send for TSCH-MAC, puts the packet in packetbuf in the MAC queue */
static void
send_packet(mac_callback_t sent, void *ptr) // HERE called by nullnet/me
{
  int ret = MAC_TX_DEFERRED;
  int hdr_len = 0;
  //Get the receiver for this packet. Set in master-net - output
  const linkaddr_t *addr = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
  uint8_t max_transmissions = 0;
#if TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES
  linkaddr_t flow_addr = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
#endif /* TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES */

  if (!tsch_is_associated)
  {
    if (!tsch_is_initialized)
    {
      LOG_WARN("! not initialized (see earlier logs), drop outgoing packet\n");
    }
    else
    {
      LOG_WARN("! not associated, drop outgoing packet\n");
    }
    ret = MAC_TX_ERR;
    //In master-net no callback is configured
    mac_call_sent_callback(sent, ptr, ret, 1);
    return;
  }

  /* Ask for ACK if we are sending anything other than broadcast */
  if (!linkaddr_cmp(addr, &linkaddr_null))
  {
    /* PACKETBUF_ATTR_MAC_SEQNO cannot be zero, due to a pecuilarity
           in framer-802154.c. */
    if (++tsch_packet_seqno == 0)
    {
      tsch_packet_seqno++;
    }
    packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, tsch_packet_seqno);
    packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, 1);
  }
  else
  {
    /* Broadcast packets shall be added to broadcast queue
     * The broadcast address in Contiki is linkaddr_null which is equal
     * to tsch_eb_address */
    LOG_ERR("Set to tsch broadcast adress\n");
    addr = &tsch_broadcast_address;
  }

  packetbuf_set_attr(PACKETBUF_ATTR_FRAME_TYPE, FRAME802154_DATAFRAME);

#if LLSEC802154_ENABLED
  if (tsch_is_pan_secured)
  {
    /* Set security level, key id and index */
    packetbuf_set_attr(PACKETBUF_ATTR_SECURITY_LEVEL, TSCH_SECURITY_KEY_SEC_LEVEL_OTHER);
    packetbuf_set_attr(PACKETBUF_ATTR_KEY_ID_MODE, FRAME802154_1_BYTE_KEY_ID_MODE); /* Use 1-byte key index */
    packetbuf_set_attr(PACKETBUF_ATTR_KEY_INDEX, TSCH_SECURITY_KEY_INDEX_OTHER);
  }
#endif /* LLSEC802154_ENABLED */

#if !NETSTACK_CONF_BRIDGE_MODE
  /*
   * In the Contiki stack, the source address of a frame is set at the RDC
   * layer. Since TSCH doesn't use any RDC protocol and bypasses the layer to
   * transmit a frame, it should set the source address by itself.
   */
  packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &linkaddr_node_addr);
#endif

  max_transmissions = packetbuf_attr(PACKETBUF_ATTR_MAX_MAC_TRANSMISSIONS);
  //printf("max_tx = %u\n", max_transmissions); //TODOLIV: remove
#if TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES //TODO TODOLIV: why?
  if (max_transmissions == 0)
  {
    LOG_ERR("smax transmission 0\n");
    return; // skip if no transmissions left
  }
  flow_addr.u8[1] = (uint8_t)packetbuf_attr(PACKETBUF_ATTR_FLOW_NUMBER);
  //printf("send to addr: %u %u\n", flow_addr.u8[0], flow_addr.u8[1]); //TODOLIV: remove
#else
  if (max_transmissions == 0)
  {
    /* If not set by the application, use the default TSCH value */
    //#   ifdef COMPETITION_RUN
    //      packetbuf_set_attr(PACKETBUF_ATTR_TSCH_SLOTFRAME, net_processing_get_slotframe(packetbuf_dataptr()) );
    //#   if TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES
    //      // DONE: get number transmissions here and get flow_number here
    //      max_transmissions = net_processing_get_flow_address_and_max_transmissions(&flow_addr);
    //#   else
    max_transmissions = TSCH_MAC_MAX_FRAME_RETRIES + 1;
    //#   endif /* TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES */
  }
#endif /* TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES */

  //Create the link-layer header from the packetbuff
  // if(packetbuf_addr(PACKETBUF_ADDR_SENDER)->u8[0] == 4)
  // {
  //   char str[256];
  //   int i;
  //   for (i = 0; i < packetbuf_totlen(); i++) {
  //     sprintf(&str[i*2], "%02X", ((uint8_t*)packetbuf_hdrptr())[i]);
  //   }
  //   LOG_INFO("Packet LEN =%u\n", packetbuf_totlen());

  //   LOG_INFO("Packet before frame create %s\n", str);
  // }

#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
  if(packetbuf_attr(PACKETBUF_ATTR_MAC_METADATA))
  {
    if(tsch_packet_create_unicast() == -1)
    {
      printf("error when adding unicast");
    }
  }
#endif

  if ((hdr_len = NETSTACK_FRAMER.create()) < 0)
  {
    LOG_ERR("! can't send packet due to framer error\n");
    printf("! can't send packet due to framer error\n"); //TODOLIV: remove
    ret = MAC_TX_ERR;
  }
  else
  {
  // if(packetbuf_addr(PACKETBUF_ADDR_SENDER)->u8[0] == 4)
  // {
  //   char str[256];
  //   int i;
  //   for (i = 0; i < packetbuf_totlen(); i++) {
  //     sprintf(&str[i*2], "%02X", ((uint8_t*)packetbuf_hdrptr())[i]);
  //   }
  //   LOG_INFO("Packet LEN =%u\n", packetbuf_totlen());

  //   LOG_INFO("Packet after frame create %s\n", str);
  // }
    struct tsch_packet *p;
#if TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES
    uint8_t send_to_nbr = packetbuf_attr(PACKETBUF_ATTR_SEND_NBR); //In case of metric gathering, we need to contact neighbors directly, not flows
    if (addr != &tsch_broadcast_address && send_to_nbr == 0)
    {
      /* Enqueue packet */
      p = tsch_queue_add_packet(&flow_addr, max_transmissions, sent, ptr);
      LOG_ERR("Add to Nbr %d. Total packets in queue %d (this nbr %d)\n",addr->u8[NODE_ID_INDEX], tsch_queue_global_packet_count(), tsch_queue_packet_count(addr));
    }
    else
    {
      /* Enqueue packet */
      p = tsch_queue_add_packet(addr, max_transmissions, sent, ptr);
      //LOG_ERR("Add to Nbr %d. Total packets in queue %d (this nbr %d)\n",addr->u8[NODE_ID_INDEX], tsch_queue_global_packet_count(), tsch_queue_packet_count(addr));
    }
    //struct tsch_neighbor* n = tsch_queue_first_nbr();

    // while(n != NULL)
    // {
    //   if(ringbufindex_elements(&n->tx_ringbuf) > 0)
    //   {
    //     //LOG_ERR("Neighbor %d has %d packets in queue\n", n->addr.u8[NODE_ID_INDEX], ringbufindex_elements(&n->tx_ringbuf));
    //   }
    //   n = tsch_queue_next_nbr(n);
    // }
#else
    //Create a tsch_packet from the packetbuffer and add it to queue
    /* Enqueue packet */
    printf("Adding packet for to %d\n", addr->u8[NODE_ID_INDEX]);
    p = tsch_queue_add_packet(addr, max_transmissions, sent, ptr);
#endif /* TSCH_WITH_CENTRAL_SCHEDULING && TSCH_FLOW_BASED_QUEUES */
    if (p == NULL)
    {
      printf("! can't send packet\n"); //TODOLIV: remove
      LOG_ERR_LLADDR(addr);
      //LOG_ERR_(" with seqno %u, queue %u %u\n",
      //    tsch_packet_seqno, tsch_queue_packet_count(addr), tsch_queue_global_packet_count());
      ret = MAC_TX_ERR;
    }
    else
    {
      p->header_len = hdr_len;
      // LOG_INFO("send packet to ");
      // LOG_INFO_LLADDR(addr);
      //LOG_INFO_(" with seqno %u, queue %u %u, len %u %u\n",
      //       tsch_packet_seqno,
      //       tsch_queue_packet_count(addr), tsch_queue_global_packet_count(),
      //       p->header_len, queuebuf_datalen(p->qb));
    }
  }
  if (ret != MAC_TX_DEFERRED)
  {
    LOG_ERR("Error\n");
    mac_call_sent_callback(sent, ptr, ret, 1);
  }
}
/*---------------------------------------------------------------------------*/
static void
packet_input(void)
{
  int frame_parsed = 1;

  frame_parsed = NETSTACK_FRAMER.parse();

  if (frame_parsed < 0)
  {
    LOG_ERR("! failed to parse %u\n", packetbuf_datalen());
  }
  else
  {
    int duplicate = 0;

    /* Seqno of 0xffff means no seqno */
    if (packetbuf_attr(PACKETBUF_ATTR_MAC_SEQNO) != 0xffff)
    {
      /* Check for duplicates */
      duplicate = mac_sequence_is_duplicate();
      if (duplicate)
      {
        /* Drop the packet. */
        LOG_WARN("! drop dup ll from ");
        LOG_WARN_LLADDR(packetbuf_addr(PACKETBUF_ADDR_SENDER));
        LOG_WARN_(" seqno %u\n", packetbuf_attr(PACKETBUF_ATTR_MAC_SEQNO));
      }
      else
      {
        mac_sequence_register_seqno();
      }
    }

    if (!duplicate)
    {
      // TSCH_LOG_ADD(tsch_log_message,
      //              snprintf(log->message, sizeof(log->message),
      //                       "[    RX SLOT:    ] {asn-%x.%lx link-%u-%u-%u}",
      //                       current_input->rx_asn.ms1b, (unsigned long)current_input->rx_asn.ls4b,
      //                       current_link->slotframe_handle, current_link->timeslot, current_link->channel_offset));
      //LOG_ERR("[    RX SLOT:    ] {asn-%x.%lx link-%u-%u-%u}\n",
      //  current_input->rx_asn.ms1b, current_input->rx_asn.ls4b,
      //  current_link->slotframe_handle, current_link->timeslot, current_link->channel_offset);

      // LOG_INFO("received from ");
      // LOG_INFO_LLADDR(packetbuf_addr(PACKETBUF_ADDR_SENDER));
      // LOG_INFO_(" with seqno %u\n", packetbuf_attr(PACKETBUF_ATTR_MAC_SEQNO));
#if TSCH_WITH_SIXTOP
      sixtop_input();
#endif /* TSCH_WITH_SIXTOP */
      NETSTACK_NETWORK.input();
    }
  }
}
/*---------------------------------------------------------------------------*/
static int
turn_on(void)
{
  if (tsch_is_initialized == 1 && tsch_is_started == 0)
  {
    tsch_is_started = 1;
    /* Process tx/rx callback and log messages whenever polled */
    process_start(&tsch_pending_events_process, NULL);
    /* periodically send TSCH EBs */
    process_start(&tsch_send_eb_process, NULL);
    /* try to associate to a network or start one if setup as coordinator */
    process_start(&tsch_process, NULL);
    return 1;
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
static int
turn_off(void)
{
  NETSTACK_RADIO.off();
  return 1;
}
/*---------------------------------------------------------------------------*/
const struct mac_driver tschmac_driver = {
    "TSCH",
    tsch_init,
    send_packet,
    packet_input,
    turn_on,
    turn_off};
/*---------------------------------------------------------------------------*/
/** @} */
