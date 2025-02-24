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
 *         Per-neighbor packet queues for TSCH MAC.
 *         The list of neighbors uses the TSCH lock, but per-neighbor packet array are lock-free.
 *				 Read-only operation on neighbor and packets are allowed from interrupts and outside of them.
 *				 *Other operations are allowed outside of interrupt only.*
 * \author
 *         Simon Duquennoy <simonduq@sics.se>
 *         Beshr Al Nahas <beshr@sics.se>
 *         Domenico De Guglielmo <d.deguglielmo@iet.unipi.it >
 */

/**
 * \addtogroup tsch
 * @{
 */

#include "contiki.h"
#include "dev/leds.h"
#include "lib/list.h"
#include "lib/memb.h"
#include "lib/random.h"
#include "net/queuebuf.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/tsch/tsch-queue.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/mac/tsch/tsch-slot-operation.h"
#include "net/mac/tsch/tsch-log.h"
//#include "../Scheduler/flocklab-gpio.h"
#include <string.h>

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "TSCH Queue"
#define LOG_LEVEL LOG_LEVEL_ERR

/* Check if TSCH_QUEUE_NUM_PER_NEIGHBOR is power of two */ // TODOLIV: allow 0 ? maybe already
#if (TSCH_QUEUE_NUM_PER_NEIGHBOR & (TSCH_QUEUE_NUM_PER_NEIGHBOR - 1)) != 0
#error TSCH_QUEUE_NUM_PER_NEIGHBOR must be power of two
#endif

/* Use to perform retransmissions for not fixed strategies */
#if TSCH_EXTENDED_RETRANSMISSIONS
extern uint8_t perform_retransmission(struct tsch_link *link);
#endif /* TSCH_EXTENDED_RETRANSMISSIONS */

/* We have as many packets are there are queuebuf in the system */
MEMB(packet_memb, struct tsch_packet, QUEUEBUF_NUM); // 8 packets
MEMB(neighbor_memb, struct tsch_neighbor, TSCH_QUEUE_MAX_NEIGHBOR_QUEUES);
LIST(neighbor_list);

//16 neighbors can each hold 8 packets. but a max of 8 packets can be allocated

/* Broadcast and EB virtual neighbors */
struct tsch_neighbor *n_broadcast;
struct tsch_neighbor *n_eb;

uint8_t num_eb_packets = 0;
// uint8_t packet_memb_count = 0;

/*---------------------------------------------------------------------------*/
/* Add a TSCH neighbor */
struct tsch_neighbor *
tsch_queue_add_nbr(const linkaddr_t *addr)
{
  struct tsch_neighbor *n = NULL;
  /* If we have an entry for this neighbor already, we simply update it */
  n = tsch_queue_get_nbr(addr);
  // printf("neighbor_exists? %u\n", n != NULL);
  if (n == NULL)
  {
    if (tsch_get_lock())
    {
      /* Allocate a neighbor */
      n = memb_alloc(&neighbor_memb);
      if (n != NULL)
      {
        /* Initialize neighbor entry */
        memset(n, 0, sizeof(struct tsch_neighbor));
        ringbufindex_init(&n->tx_ringbuf, TSCH_QUEUE_NUM_PER_NEIGHBOR);
        linkaddr_copy(&n->addr, addr);
        n->is_broadcast = linkaddr_cmp(addr, &tsch_eb_address) || linkaddr_cmp(addr, &tsch_broadcast_address);
        tsch_queue_backoff_reset(n);
        /* Add neighbor to the list */
        list_add(neighbor_list, n);
        //LOG_ERR("Added nbr %d \n", n->addr.u8[NODE_ID_INDEX]);
      }
      tsch_release_lock();
    }
  }
  return n;
}
/*---------------------------------------------------------------------------*/
/* Get a TSCH neighbor */
struct tsch_neighbor *
tsch_queue_get_nbr(const linkaddr_t *addr)
{
  if (!tsch_is_locked())
  {
    struct tsch_neighbor *n = list_head(neighbor_list);
    while (n != NULL)
    {
      if (linkaddr_cmp(&n->addr, addr))
      {
        return n;
      }
      n = list_item_next(n);
    }
  }
  return NULL;
}

struct tsch_neighbor *
tsch_queue_get_nbr_by_node_id(const uint8_t node_id)
{
  if (!tsch_is_locked())
  {
    struct tsch_neighbor *n = list_head(neighbor_list);
    while (n != NULL)
    {
      if (n->addr.u8[NODE_ID_INDEX] == node_id)
      {
        return n;
      }
      n = list_item_next(n);
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* Get the first TSCH neighbor */
struct tsch_neighbor *tsch_queue_first_nbr()
{
  //
  if (!tsch_is_locked())
  {
    struct tsch_neighbor *n = list_head(neighbor_list);
    while (n != NULL)
    {
      if (linkaddr_cmp(&n->addr, &tsch_broadcast_address) ||
          linkaddr_cmp(&n->addr, &tsch_eb_address))
      {
        n = list_item_next(n);
      }
      else
      {
        //flow have at index 1 != 0 and at index 0 == 0. We want to send to NBRs and not flows
        if(n->addr.u8[1] != 0 && n->addr.u8[NODE_ID_INDEX] == 0)
        {
          n = list_item_next(n);
        }else{
          return n;
        }
      }
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* Get the following TSCH neighbor */
struct tsch_neighbor *tsch_queue_next_nbr(struct tsch_neighbor *neighbour)
{
  if (!tsch_is_locked())
  {
    struct tsch_neighbor *n = list_item_next(neighbour);
    while (n != NULL)
    {
      if (linkaddr_cmp(&n->addr, &tsch_broadcast_address) ||
          linkaddr_cmp(&n->addr, &tsch_eb_address))
      {
        n = list_item_next(n);
      }
      else
      {
        //flow have at index 1 != 0 and at index 0 == 0. We want to send to NBRs and not flows
        if(n->addr.u8[1] != 0 && n->addr.u8[NODE_ID_INDEX] == 0)
        {
          n = list_item_next(n);
        }else{
          return n;
        }
      }   
    }
  }
  return NULL;
}

int tsch_queue_count_nbr()
{
  int count = 0;
  if (!tsch_is_locked())
  {
    //TODO:: check this if it makes nodes crash?
    struct tsch_neighbor *n = list_head(neighbor_list);
    while (n != NULL)
    {
      if (linkaddr_cmp(&n->addr, &tsch_broadcast_address) ||
          linkaddr_cmp(&n->addr, &tsch_eb_address) ||
          (n->addr.u8[1] != 0 && n->addr.u8[NODE_ID_INDEX] == 0))
      {
        n = list_item_next(n);
      }
      else
      {
        //flow have at index 1 != 0 and at index 0 == 0. We want to send to NBRs and not flows
        count++;
        n = list_item_next(n);
      }   
    }
  }
  return count;
}
/*---------------------------------------------------------------------------*/
/* Get a TSCH time source (we currently assume there is only one) */
struct tsch_neighbor *
tsch_queue_get_time_source(void)
{
  if (!tsch_is_locked())
  {
    struct tsch_neighbor *curr_nbr = list_head(neighbor_list);
    while (curr_nbr != NULL)
    {
      if (curr_nbr->is_time_source)
      {
        return curr_nbr;
      }
      curr_nbr = list_item_next(curr_nbr);
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* Update TSCH time source */
int tsch_queue_update_time_source(const linkaddr_t *new_addr)
{
  printf("tsch_queue_update_time_source\n");
  if (!tsch_is_locked())
  {
    printf("!tsch_is_locked()\n");
    if (!tsch_is_coordinator)
    {
      struct tsch_neighbor *old_time_src = tsch_queue_get_time_source();
      struct tsch_neighbor *new_time_src = NULL;

      if (new_addr != NULL)
      {
        /* Get/add neighbor, return 0 in case of failure */
        new_time_src = tsch_queue_add_nbr(new_addr);
        if (new_time_src == NULL)
        {
          return 0;
        }
      }

      if (new_time_src != old_time_src)
      {
        LOG_INFO("update time source: ");
        LOG_INFO_LLADDR(old_time_src ? &old_time_src->addr : NULL);
        LOG_INFO_(" -> ");
        LOG_INFO_LLADDR(new_time_src ? &new_time_src->addr : NULL);
        LOG_INFO_("\n");

        /* Update time source */
        if (new_time_src != NULL)
        {
          new_time_src->is_time_source = 1;
          /* (Re)set keep-alive timeout */
          tsch_set_ka_timeout(TSCH_KEEPALIVE_TIMEOUT);
        }
        else
        {
          /* Stop sending keepalives */
          tsch_set_ka_timeout(0);
        }

        if (old_time_src != NULL)
        {
          old_time_src->is_time_source = 0;
        }

#ifdef TSCH_CALLBACK_NEW_TIME_SOURCE
        TSCH_CALLBACK_NEW_TIME_SOURCE(old_time_src, new_time_src);
#endif
      }

      return 1;
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
#if TSCH_PACKET_EB_WITH_NEIGHBOR_DISCOVERY
void tsch_queue_update_time_source_rank(const uint8_t time_source_rank)
{
  struct tsch_neighbor *time_src = tsch_queue_get_time_source();
  time_src->rank = time_source_rank;
}

void tsch_queue_update_neighbour_rank_and_time_source(const linkaddr_t *neighbour_addr, const uint8_t rank, const uint8_t time_source)
{
  if (neighbour_addr == NULL)
    return;

  struct tsch_neighbor *nbr = tsch_queue_get_nbr(neighbour_addr);
  if (nbr == NULL)
    return;

  nbr->rank = rank;
  nbr->time_source = time_source;
}

// uint8_t tsch_queue_has_packet_type(struct tsch_neighbor *nbr, int type)
// {
//   LOG_ERR("Looking for packet %d\n", type);
//   if (!tsch_is_locked())
//   {
//     if(nbr != NULL)
//     {
//       int16_t index = ringbufindex_peek_get(&nbr->tx_ringbuf);
//       if(index != -1)
//       {
//         if(queuebuf_attr(nbr->tx_array[index]->qb, PACKETBUF_ATTR_PACKET_NUMBER) == packet_number)
//         {
//           return 1;
//         }
//         else{
//           return 0;
//         }
//       }
//     }
//   }
//   return 0;
// }

uint8_t tsch_queue_is_packet_in_nbr_queue(struct tsch_neighbor *nbr, uint8_t packet_number)
{

  LOG_DBG("Looking for packet %d\n", packet_number);
  if (!tsch_is_locked())
  {
    if(nbr != NULL)
    {
      int16_t index = ringbufindex_peek_get(&nbr->tx_ringbuf);
      if(index != -1)
      {
        struct queuebuf * quebuf = nbr->tx_array[index]->qb;
        if(quebuf != NULL && queuebuf_attr(nbr->tx_array[index]->qb, PACKETBUF_ATTR_PACKET_NUMBER) == packet_number)
        {
          return 1;
        }
      }
    }
  }
  return 0;
}

#endif
/*---------------------------------------------------------------------------*/
/* Flush a neighbor queue */
static void
tsch_queue_flush_nbr_queue(struct tsch_neighbor *n)
{
  while (!tsch_queue_is_empty(n))
  {
    struct tsch_packet *p = tsch_queue_remove_packet_from_queue(n);
    if (p != NULL)
    {
      /* Set return status for packet_sent callback */
      p->ret = MAC_TX_ERR;
      LOG_WARN("! flushing packet\n");
      /* Call packet_sent callback */
      mac_call_sent_callback(p->sent, p->ptr, p->ret, p->transmissions);
      /* Free packet queuebuf */
      tsch_queue_free_packet(p);
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Remove TSCH neighbor queue */
static void
tsch_queue_remove_nbr(struct tsch_neighbor *n)
{
  if (n != NULL)
  {
    if (tsch_get_lock())
    {

      /* Remove neighbor from list */
      list_remove(neighbor_list, n);

      tsch_release_lock();

      /* Flush queue */
      tsch_queue_flush_nbr_queue(n);

      /* Free neighbor */
      memb_free(&neighbor_memb, n);
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Add packet to neighbor queue. Use same lockfree implementation as ringbuf.c (put is atomic) */
struct tsch_packet *
tsch_queue_add_packet(const linkaddr_t *addr, uint8_t max_transmissions,
                      mac_callback_t sent, void *ptr)
{
  // leds_on(LEDS_YELLOW);
  struct tsch_neighbor *n = NULL;
  int16_t put_index = -1;
  struct tsch_packet *p = NULL;
  if (!tsch_is_locked())
  {
    n = tsch_queue_add_nbr(addr);
    if (n != NULL)
    {
      put_index = ringbufindex_peek_put(&n->tx_ringbuf);
      //LOG_ERR("Put index: %d, nbr packets: %d for nbr %d  %d, total elements: %d\n",  put_index, tsch_queue_packet_count(addr), addr->u8[1], addr->u8[NODE_ID_INDEX], tsch_queue_global_packet_count());
      if (put_index != -1)
      {
        p = memb_alloc(&packet_memb);
        //++packet_memb_count;
        // printf("p == NULL? %u\n", p == NULL);
        if (p != NULL)
        {
          /* Enqueue packet */
#ifdef TSCH_CALLBACK_PACKET_READY
          TSCH_CALLBACK_PACKET_READY();
#endif
          p->qb = queuebuf_new_from_packetbuf();
          // printf("p->qb != NULL? %u\n", p->qb != NULL);
          if (p->qb != NULL)
          {
            // leds_on(LEDS_YELLOW);
            // leds_on(LEDS_GREEN);
            p->sent = sent;
            p->ptr = ptr;
            p->ret = MAC_TX_DEFERRED;
            p->transmissions = 0;
            p->max_transmissions = max_transmissions;
            /* Add to ringbuf (actual add committed through atomic operation) */
            n->tx_array[put_index] = p;
            ringbufindex_put(&n->tx_ringbuf);
            // LOG_DBG("packet is added put_index %u, packet %p\n",
            //         put_index, p);
            // printf("TSCH: added packet to queue with %u transmissions, put_index %u, packet %p, neighbor %p\n", max_transmissions, put_index, p, n);
            // leds_off(LEDS_GREEN);
            //++num_packets;
            if (n == n_eb)
            {
              ++num_eb_packets;
            }
            // leds_off(LEDS_YELLOW);
            return p;
          }
          else
          {
            memb_free(&packet_memb, p);
            //--packet_memb_count;
          }
        }else{
          LOG_ERR("! add packet failed, p is NULL \n");
        }
      }
    }
  }
  LOG_ERR("! add packet failed: %u %p %d %p %p\n", tsch_is_locked(), n, put_index, p, p ? p->qb : NULL);
  // leds_off(LEDS_YELLOW);
  return 0;
}
/*---------------------------------------------------------------------------*/
/* Returns the number of packets currently in any TSCH queue */
int tsch_queue_global_packet_count(void)
{
  return QUEUEBUF_NUM - memb_numfree(&packet_memb);
}
/*---------------------------------------------------------------------------*/
/* Returns the number of packets currently in the queue */
int tsch_queue_packet_count(const linkaddr_t *addr)
{
  struct tsch_neighbor *n = NULL;
  if (!tsch_is_locked())
  {
    n = tsch_queue_add_nbr(addr);
    if (n != NULL)
    {
      return ringbufindex_elements(&n->tx_ringbuf);
    }
  }
  return -1;
}
/*---------------------------------------------------------------------------*/
/* Remove first packet from a neighbor queue */
struct tsch_packet *
tsch_queue_remove_packet_from_queue(struct tsch_neighbor *n)
{
  if (!tsch_is_locked())
  {
    if (n != NULL)
    {
      /* Get and remove packet from ringbuf (remove committed through an atomic operation */
      int16_t get_index = ringbufindex_get(&n->tx_ringbuf);
      if (get_index != -1)
      {
        if (n == n_eb)
        {
          --num_eb_packets;
          //--num_packets;
          // printf("num_packets = %d\n", tsch_queue_global_packet_count());
          //} else {
          //--num_packets;
        } //*/
        //LOG_ERR("Removed Packet at index %d for nbr %d\n", get_index, n->addr.u8[NODE_ID_INDEX]);
        return n->tx_array[get_index];
      }
      else
      {
        return NULL;
      }
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* Free a packet */
void tsch_queue_free_packet(struct tsch_packet *p)
{
  if (p != NULL)
  {
    queuebuf_free(p->qb);
    memb_free(&packet_memb, p);
    //--packet_memb_count;
  }
}
/*---------------------------------------------------------------------------*/
/* Updates neighbor queue state after a transmission */
int tsch_queue_packet_sent(struct tsch_neighbor *n, struct tsch_packet *p,
                           struct tsch_link *link, uint8_t mac_tx_status)
{
  int in_queue = 1;
  int is_shared_link = link->link_options & LINK_OPTION_SHARED;
  int is_unicast = !n->is_broadcast;

  if (mac_tx_status == MAC_TX_OK)
  {
    /* Successful transmission */
    // SET_PIN_ADC0;
    tsch_queue_remove_packet_from_queue(n);
    // UNSET_PIN_ADC0;
    in_queue = 0;

    /* Update CSMA state in the unicast case */
    if (is_unicast)
    {
      if (is_shared_link || tsch_queue_is_empty(n))
      {
        /* If this is a shared link, reset backoff on success.
         * Otherwise, do so only is the queue is empty */
        tsch_queue_backoff_reset(n);
      }
    }
  }
  else
  {
    /* Failed transmission */
#if TSCH_WITH_CENTRAL_SCHEDULING && TSCH_TTL_BASED_RETRANSMISSIONS
    uint16_t timeslot_diff = 0;
    // TODOLIV: base on next active slot if possible v
    //  tsch_link * next_active_link_tmp;
    //  next_active_link_tmp = tsch_schedule_get_next_active_link(&tsch_current_asn, &timeslot_diff, NULL);
    //  if (next_active_link_tmp == NULL){
    //    timeslot_diff = 1;
    //  }
    // TODOLIV: base on next active slot if possible ^
    timeslot_diff = 1;
    uint16_t next_asn_ls4b = (uint16_t)tsch_current_asn.ls4b + timeslot_diff;
    if (TSCH_SLOTNUM_LT((next_asn_ls4b - 1), queuebuf_attr(p->qb, PACKETBUF_ATTR_TRANSMISSION_TTL)))
    { // re-send only if time left for sending
      //                                                                                        (ensure next tx if before expiration)
      p->max_transmissions = (uint8_t)(queuebuf_attr(p->qb, PACKETBUF_ATTR_TRANSMISSION_TTL) - (next_asn_ls4b - 1)) + p->transmissions; // DONELIV update max_transmissions based on current time slot (max_transmissions = ttl_slot - current_slot + p->transmissions)
    }
    else
    {
      p->max_transmissions = p->transmissions; // => dequeue
    }
#endif /* TSCH_WITH_CENTRAL_SCHEDULING && TSCH_TTL_BASED_RETRANSMISSIONS */

#if TSCH_EXTENDED_RETRANSMISSIONS // needed!!
    if (!perform_retransmission(link) || (p->transmissions >= p->max_transmissions))
    {  // ask upper layer whether to retransmit
#else  /* TSCH_EXTENDED_RETRANSMISSIONS */
    if (p->transmissions >= p->max_transmissions)
    {
#endif /* TSCH_EXTENDED_RETRANSMISSIONS */
      /* Drop packet */
      // SET_PIN_ADC0;
      tsch_queue_remove_packet_from_queue(n);
      // UNSET_PIN_ADC0;
      // SET_PIN_ADC0;
      // UNSET_PIN_ADC0;
      in_queue = 0;
    }
    /* Update CSMA state in the unicast case */
    if (is_unicast)
    {
      /* Failures on dedicated (== non-shared) leave the backoff
       * window nor exponent unchanged */
      if (is_shared_link)
      {
        /* Shared link: increment backoff exponent, pick a new window */
        tsch_queue_backoff_inc(n);
      }
    }
  }

  return in_queue;
}
/*---------------------------------------------------------------------------*/
/* Flush all neighbor queues */
void tsch_queue_reset(void)
{
  /* Deallocate unneeded neighbors */
  if (!tsch_is_locked())
  {
    struct tsch_neighbor *n = list_head(neighbor_list);
    while (n != NULL)
    {
      struct tsch_neighbor *next_n = list_item_next(n);
      /* Flush queue */
      tsch_queue_flush_nbr_queue(n);
      /* Reset backoff exponent */
      tsch_queue_backoff_reset(n);
      n = next_n;
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Deallocate neighbors with empty queue */
void tsch_queue_free_unused_neighbors(void)
{
  /* Deallocate unneeded neighbors */
  if (!tsch_is_locked())
  {
    struct tsch_neighbor *n = list_head(neighbor_list);
    while (n != NULL)
    {
      struct tsch_neighbor *next_n = list_item_next(n);
      /* Queue is empty, no tx link to this neighbor: deallocate.
       * Always keep time source and virtual broadcast neighbors. */
      if (!n->is_broadcast && !n->is_time_source && !n->tx_links_count && tsch_queue_is_empty(n))
      {
        tsch_queue_remove_nbr(n);
      }
      n = next_n;
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Is the neighbor queue empty? */
int tsch_queue_is_empty(const struct tsch_neighbor *n)
{
  return !tsch_is_locked() && n != NULL && ringbufindex_empty(&n->tx_ringbuf);
}
/*---------------------------------------------------------------------------*/
/* Returns the first packet from a neighbor queue */
struct tsch_packet *
tsch_queue_get_packet_for_nbr(const struct tsch_neighbor *n, struct tsch_link *link)
{
  if (!tsch_is_locked())
  {
    int is_shared_link = link != NULL && link->link_options & LINK_OPTION_SHARED;
    if (n != NULL)
    {
      int16_t get_index = ringbufindex_peek_get(&n->tx_ringbuf);
      // if (link->addr.u8[0] == 6 && link->link_options == LINK_OPTION_TX){
      //   printf("get_index = %i\n", get_index);
      // }
      if (get_index != -1 &&
          !(is_shared_link && !tsch_queue_backoff_expired(n)))
      { /* If this is a shared link,
        make sure the backoff has expired */
#if TSCH_WITH_LINK_SELECTOR_SCHEDULER
        uint16_t packet_attr_slotframe = queuebuf_attr(n->tx_array[get_index]->qb, PACKETBUF_ATTR_TSCH_SLOTFRAME);
        if (packet_attr_slotframe != 0xffff && packet_attr_slotframe != link->slotframe_handle)
        {
          return NULL;
        }
#endif
#if TSCH_WITH_LINK_SELECTOR
        int packet_attr_slotframe = queuebuf_attr(n->tx_array[get_index]->qb, PACKETBUF_ATTR_TSCH_SLOTFRAME);
        int packet_attr_timeslot = queuebuf_attr(n->tx_array[get_index]->qb, PACKETBUF_ATTR_TSCH_TIMESLOT);
        if (packet_attr_slotframe != 0xffff && packet_attr_slotframe != link->slotframe_handle)
        {
          return NULL;
        }
        if (packet_attr_timeslot != 0xffff && packet_attr_timeslot != link->timeslot)
        {
          return NULL;
        }
#endif
        return n->tx_array[get_index];
      }
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* Returns the head packet from a neighbor queue (from neighbor address) */
struct tsch_packet *
tsch_queue_get_packet_for_dest_addr(const linkaddr_t *addr, struct tsch_link *link)
{
  if (!tsch_is_locked())
  {
    return tsch_queue_get_packet_for_nbr(tsch_queue_get_nbr(addr), link);
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* Returns the head packet of any neighbor queue with zero backoff counter.
 * Writes pointer to the neighbor in *n */
struct tsch_packet *
tsch_queue_get_unicast_packet_for_any(struct tsch_neighbor **n, struct tsch_link *link)
{
  if (!tsch_is_locked())
  {
    struct tsch_neighbor *curr_nbr = list_head(neighbor_list);
    struct tsch_packet *p = NULL;
    while (curr_nbr != NULL)
    {
      if (!curr_nbr->is_broadcast && curr_nbr->tx_links_count == 0)
      {
        /* Only look up for non-broadcast neighbors we do not have a tx link to */
        p = tsch_queue_get_packet_for_nbr(curr_nbr, link);
        if (p != NULL)
        {
          if (n != NULL)
          {
            *n = curr_nbr;
          }
          return p;
        }
      }
      curr_nbr = list_item_next(curr_nbr);
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* May the neighbor transmit over a shared link? */
int tsch_queue_backoff_expired(const struct tsch_neighbor *n)
{
  return n->backoff_window == 0;
}
/*---------------------------------------------------------------------------*/
/* Reset neighbor backoff */
void tsch_queue_backoff_reset(struct tsch_neighbor *n)
{
  n->backoff_window = 0;
  n->backoff_exponent = TSCH_MAC_MIN_BE;
}
/*---------------------------------------------------------------------------*/
/* Increment backoff exponent, pick a new window */
void tsch_queue_backoff_inc(struct tsch_neighbor *n)
{
  /* Increment exponent */
  n->backoff_exponent = MIN(n->backoff_exponent + 1, TSCH_MAC_MAX_BE);
  /* Pick a window (number of shared slots to skip). Ignore least significant
   * few bits, which, on some embedded implementations of rand (e.g. msp430-libc),
   * are known to have poor pseudo-random properties. */
  n->backoff_window = (random_rand() >> 6) % (1 << n->backoff_exponent);
  /* Add one to the window as we will decrement it at the end of the current slot
   * through tsch_queue_update_all_backoff_windows */
  n->backoff_window++;
}
/*---------------------------------------------------------------------------*/
/* Decrement backoff window for all queues directed at dest_addr */
void tsch_queue_update_all_backoff_windows(const linkaddr_t *dest_addr)
{
  if (!tsch_is_locked())
  {
    int is_broadcast = linkaddr_cmp(dest_addr, &tsch_broadcast_address);
    struct tsch_neighbor *n = list_head(neighbor_list);
    while (n != NULL)
    {
      if (n->backoff_window != 0 /* Is the queue in backoff state? */
          && ((n->tx_links_count == 0 && is_broadcast) || (n->tx_links_count > 0 && linkaddr_cmp(dest_addr, &n->addr))))
      {
        n->backoff_window--;
      }
      n = list_item_next(n);
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Initialize TSCH queue module */
void tsch_queue_init(void)
{
  list_init(neighbor_list);
  memb_init(&neighbor_memb);
  memb_init(&packet_memb);
  /* Add virtual EB and the broadcast neighbors */
  n_eb = tsch_queue_add_nbr(&tsch_eb_address);
  n_broadcast = tsch_queue_add_nbr(&tsch_broadcast_address);
}
/*---------------------------------------------------------------------------*/
/** @} */
