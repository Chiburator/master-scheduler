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

/* The process to receive a schedule over serial line input */
PROCESS_NAME(serial_line_schedule_input);

enum MessagePart{
  MESSAGE_BEGIN,
  MESSAGE_CONTINUE,
  MESSAGE_END
};

/* structure used by Master (Python) */
typedef struct
{
  uint8_t slotframe_handle;
  uint8_t send_receive;
  uint8_t timeslot;
  uint8_t channel_offset;
} scheduled_link_t;

/** Master's schedule that will be distributed and applied at all nodes*/
typedef struct master_tsch_schedule_t
{
  uint8_t own_transmission_flow;
  uint8_t own_receiver; //is_sender can be checked if own_receiver is available (!= 0)
  uint8_t flow_forwards[MASTER_NUM_FLOWS];
  uint8_t max_transmission[MASTER_NUM_FLOWS];
  uint8_t links_len;
  scheduled_link_t links[TSCH_SCHEDULE_MAX_LINKS];
} master_tsch_schedule_t;

typedef struct master_tsch_schedule_universall_config_t
{
  uint8_t schedule_length;
  uint8_t slot_frames;
  uint8_t sender_of_flow[MASTER_NUM_FLOWS]; 
  uint8_t receiver_of_flow[MASTER_NUM_FLOWS]; 
  uint8_t first_tx_slot_in_flow[MASTER_NUM_FLOWS]; 
  uint8_t last_tx_slot_in_flow[MASTER_NUM_FLOWS]; 
  uint8_t last_received_relayed_packet_of_flow[MASTER_NUM_FLOWS]; 
} master_tsch_schedule_universall_config_t;

/* Variable to know how many bytes we wrote to flash */
extern int bytes_in_flash;

/* The schedule structure to be used */
extern master_tsch_schedule_t schedule;
/* The schedule configuration applied at every node to be used */
extern master_tsch_schedule_universall_config_t schedule_config;

struct master_tsch_schedule_t* get_own_schedule();

int get_node_receiver();

int node_is_sender();

uint8_t get_forward_dest_by_slotframe(master_tsch_schedule_t* schedule, uint8_t link_idx);

uint8_t get_forward_dest(master_tsch_schedule_t* schedule, uint8_t flow);

uint8_t get_max_transmissions(master_tsch_schedule_t* schedule, uint8_t flow);

uint8_t read_flash(uint8_t * buf, int offset, uint8_t len);

uint8_t read_from_flash_by_id(master_tsch_schedule_universall_config_t * config, master_tsch_schedule_t * schedule, int id);

uint8_t write_to_flash_offset(uint8_t * data, int offset, uint8_t len);

void setBit(uint32_t *bit_vector, int k);
void setBit1Byte(uint8_t *bit_vector, int k);

void clearBit(uint32_t *bit_vector, int k);

uint8_t isBitSet(uint32_t *bit_vector, int k);
uint8_t isBitSet1Byte(uint8_t *bit_vector, int k);

void resetBitVector(uint32_t *bit_vector, uint8_t n);

/**
 * Check if every packet for the current schedule is received.
 * Returns -1 if all packets are received. Otherwise the return value is the missing packet.
 * 
 * \param last_packet The last packet for a schedule
*/
int getMissingPacket(uint32_t *bit_vector, int last_packet);

#endif /* MASTER_SCHEDULE_H */