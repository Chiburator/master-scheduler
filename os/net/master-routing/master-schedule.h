/** 
 * \file
 *         Header file for the Master schedule module.
 * \author
 *         Viktor Paskal 
 *
 */

#ifndef MASTER_SCHEDULE_H
#define MASTER_SCHEDULE_H

#include "contiki.h"
#include "tsch/tsch-schedule.h"

PROCESS_NAME(serial_line_schedule_input);

enum MessagePart{
  MESSAGE_BEGIN,
  MESSAGE_CONTINUE,
  MESSAGE_END
};

/**
 * Function prototype for master schedule callback once the schedule is loaded
*/
typedef void (* masternet_schedule_loaded)(void);

/**
 * Set callback for Master schedule after the schedule is loaded
 *
 * \param callback The callback
*/
void master_schedule_set_schedule_loaded_callback(masternet_schedule_loaded callback);

/* structure used by Master (Python) */
typedef struct __attribute__((packed))
{
  uint8_t slotframe_handle;
  uint8_t send_receive;
  uint8_t timeslot;
  uint8_t channel_offset;
} scheduled_link_t;

/** Master's schedule that will be distributed and applied at all nodes*/
typedef struct __attribute__((packed)) master_tsch_schedule_t
{
  uint8_t own_transmission_flow;
  uint8_t own_receiver; //is_sender can be checked if own_receiver is available (!= 0)
  uint8_t flow_forwards[MASTER_NUM_FLOWS];
  uint8_t max_transmission[MASTER_NUM_FLOWS];
  uint8_t links_len;
  scheduled_link_t links[TSCH_SCHEDULE_MAX_LINKS];
} master_tsch_schedule_t;

extern master_tsch_schedule_t schedules[];

typedef struct __attribute__((packed)) master_tsch_schedule_universall_config_t
{
  uint8_t schedule_length;
  uint8_t slot_frames; //TODO:: this will be used to init the slotframes starting at 1 
  uint8_t sender_of_flow[MASTER_NUM_FLOWS]; 
  uint8_t receiver_of_flow[MASTER_NUM_FLOWS]; 
  uint8_t first_tx_slot_in_flow[MASTER_NUM_FLOWS]; 
  uint8_t last_tx_slot_in_flow[MASTER_NUM_FLOWS]; 
  uint8_t last_received_relayed_packet_of_flow[MASTER_NUM_FLOWS]; 
} master_tsch_schedule_universall_config_t;

extern master_tsch_schedule_universall_config_t schedule_config;

#endif /* MASTER_SCHEDULE_H */