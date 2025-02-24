/**
 * \file
 *         Unicast communication
 * \author
 *         Oliver Harms <oha@informatik.uni-kiel.de>
 *
 */

// includes
  #include "contiki.h"
  #include "net/master-routing/master-routing.h"
  #include "node-id.h"
  #include "lib/random.h"
  #include "storage/cfs/cfs.h"
  #include "storage/cfs/cfs-coffee.h"

  #include <string.h>

/* Log configuration */
  #include "sys/log.h"
  #define LOG_MODULE "App"
  #define LOG_LEVEL LOG_LEVEL_INFO


/*---------------------------------------------------------------------------*/
PROCESS(master_unicast_process, "Master unicast example");
AUTOSTART_PROCESSES(&master_unicast_process);

/*---------------------------------------------------------------------------*/
void input_callback(const void *data, uint16_t len, uint8_t src, uint8_t dest)
{
  LOG_INFO("Received %u bytes from node %u\n", len, src);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(master_unicast_process, ev, data)
{
  static struct etimer periodic_timer;
  static uint8_t payload[MASTER_MSG_LENGTH];
  static uint8_t own_receiver;

  PROCESS_BEGIN();
#ifdef TESTBED_KIEL
  //Reserve enough memory for the schedule in the beginning. Increse this latter if not enough
  cfs_coffee_reserve(FILENAME, 10000);
#endif
  /* Initialize Master */
  init_master_routing();
  master_routing_set_input_callback(input_callback);

  etimer_set(&periodic_timer, CLOCK_SECOND);

  do {
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
      etimer_reset(&periodic_timer);
  } while(!master_routing_configured());

  if (node_is_sender()){
    own_receiver = get_node_receiver();
    etimer_set(&periodic_timer, random_rand()%(CLOCK_SECOND));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
#   if CONTIKI_TARGET_SKY
    etimer_set(&periodic_timer, 1.025*CLOCK_SECOND);
#   else
    etimer_set(&periodic_timer, 1.01*CLOCK_SECOND);
#   endif
    while(1){
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

      uint8_t payload_index;
      for (payload_index = 0; payload_index < MASTER_PAYLOAD_LENGTH; ++payload_index){
        payload[payload_index] = (uint8_t)(random_rand() >> 8);
      }

      master_routing_sendto(&payload, MASTER_PAYLOAD_LENGTH, own_receiver);
      etimer_reset(&periodic_timer);
    }
  } else {
    while (1){
      PROCESS_YIELD();
    }
  }
  printf("Process end for master-unicast\n");
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/