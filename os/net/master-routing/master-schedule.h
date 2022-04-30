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

//TODO:: remove this?
struct master_tsch_schedule_t* get_own_schedule();

/** @brief Convert two ascii charackter representing a byte back to a byte
 * 
 *  @param asciiHex the buffer containing the charackters
 *  @return A one byte unsigned integer
 */
uint8_t asciiHex_to_int(uint8_t *asciiHex);

/** @brief Check if a node is a receiver of packets
 * 
 *  @return 1 if this node is a sender. 0 otherwise
 */
int get_node_receiver();

/** @brief Check if a node is a sender or not
 * 
 *  @return 1 if this node is a sender. 0 otherwise
 */
int node_is_sender();

/** @brief Open a file in a specified mode
 * 
 *  @param mode the mode to open the file with
 *  @return 1 if the file could be opened. 0 otherwise.
 */
int open_file(uint8_t mode);

/** @brief Get the destination by link
 * 
 *  @param schedule the schedule to look at
 *  @param link_idx the to get the destination address for
 *  @return the destination
 */
uint8_t get_forward_dest_by_slotframe(master_tsch_schedule_t* schedule, uint8_t link_idx);

/** @brief Get the destination for a flow packet
 * 
 *  @param schedule the schedule to look at
 *  @param flow the flow
 *  @return the destination for a flow packet
 */
uint8_t get_forward_dest(master_tsch_schedule_t* schedule, uint8_t flow);

/** @brief Get the max transmission for a flow in a schedule
 *
 * 
 *  @param schedule the schedule to look at
 *  @param flow the flow
 *  @return max transmission for this flow in the schedule
 */
uint8_t get_max_transmissions(master_tsch_schedule_t* schedule, uint8_t flow);

/** @brief Read data directly from the flash memory to a buffer
 *
 *  This function is used to prepare transmission of packets. 
 *  We can read directly into a packet unformated content from the flash memory 
 * 
 *  @param buf the schedule config to write at
 *  @param offset the offset to read from
 *  @param len how many bytes to read
 *  @return 1 if data could be read. 0 otherwise.
 */
uint8_t read_flash(uint8_t * buf, int offset, uint8_t len);

/** @brief Read data directly from the flash memory to data structures
 *
 *  @param config the schedule config to write at
 *  @param schedule the schedule to write at
 *  @param id the node id to search the schedule in the flash memory for
 *  @return 1 if a schedule was found. 0 if not or if an error occured.
 */
uint8_t read_from_flash_by_id(master_tsch_schedule_universall_config_t * config, master_tsch_schedule_t * schedule, int id);

/** @brief Write data directly to the flash memory
 *
 *  @param data the data to write
 *  @param offset the offset in the flash memory to start writing to
 *  @param len the length of the data to write
 *  @return 0 if the write operation failed. 1 otherwise.
 */
uint8_t write_to_flash_offset(uint8_t * data, int offset, uint8_t len);

/** @brief Set the bit a position 'k' to 1
 *
 *  @param bit_vector the bit-vector to set the bit at
 *  @param k the bit position
 */
void setBit(uint32_t *bit_vector, int k);

/** @brief Set the bit a position 'k' to 1 (1 byte bit-vector)
 *
 *  @param bit_vector the bit-vector to set the bit at
 *  @param k the bit position
 */
void setBit1Byte(uint8_t *bit_vector, int k);

/** @brief clear a bit at position 'k'
 *
 *  @param bit_vector the bit-vector to check
 *  @param k the bit position
 */
void clearBit(uint32_t *bit_vector, int k);

/** @brief Check if a bit is set at position 'k'
 *
 *  @param bit_vector the bit-vector to check
 *  @param k the bit position
 *  @return 1 if the bit is set. 0 otherwise
 */
uint8_t isBitSet(uint32_t *bit_vector, int k);

/** @brief Check if a bit is set at position 'k' (1 byte bit vector)
 *
 *  @param bit_vector the bit-vector to check
 *  @param k the bit position
 *  @return 1 if the bit is set. 0 otherwise
 */
uint8_t isBitSet1Byte(uint8_t *bit_vector, int k);

/** @brief Set a bit-vector of size 'n' to 0.
 *
 *  @param bit_vector the bit-vector to reset
 *  @param n the size of the vector
 */
void resetBitVector(uint32_t *bit_vector, uint8_t n);

/** @brief Check a bit-vector for a bit = 0.
 *
 *  Check the bit-vector for missing packets.
 *  Go through the vector starting from 1 to 'last_packet'.
 *
 *  @param bit_vector the bit-vector identifieng received packets
 *  @param last_packet the last packet as a search delimiter
 *  @return The missing packet as an integer >= 1. Return -1 if all packets are received.
 */
int getMissingPacket(uint32_t *bit_vector, int last_packet);

#endif /* MASTER_SCHEDULE_H */