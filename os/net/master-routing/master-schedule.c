/** 
 * \file
 *         Source file for the Master schedule module.
 * \author
 *         Viktor Paskal 
 *
 */
#include "contiki.h"
#include "master-schedule.h"
#include "tsch-slot-operation.h"
#include "dev/serial-line.h"
#include "os/storage/cfs/cfs.h"
#include "os/storage/cfs/cfs-coffee.h"

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "MASTER-S"
#define LOG_LEVEL LOG_LEVEL_DBG

//Serial input process
PROCESS(serial_line_schedule_input, "Master scheduler serial line input");

//pt for the protothread
static struct pt start_slot_operation_pt;

//Bool if the schedule is loaded or not
uint8_t schedule_loading = 1;

//file descriptor
int fd;

//bytes in flash. used to identify when we are finished
int bytes_in_flash;

//Used to indicate if the file to read from is open
uint8_t open = 0;

/* Convert 2 bytes represented as ASCII characters back to one byte */
uint8_t asciiHex_to_int(uint8_t *asciiHex)
{
  if(*(asciiHex) == 0)
  {
    return 0;
  }

  uint8_t HigherNibble = *(asciiHex);
  uint8_t lowerNibble = *(asciiHex + 1);

  if(lowerNibble >= (uint8_t) 'A')
  {
    lowerNibble = lowerNibble - (uint8_t)'7';
  }else{
    lowerNibble = lowerNibble - (uint8_t)'0';
  }


  if(HigherNibble >= (uint8_t) 'A')
  {
    HigherNibble = HigherNibble - (uint8_t)'7';
  }else{
    HigherNibble = HigherNibble - (uint8_t)'0';
  }

  uint8_t result = (HigherNibble << 4) + lowerNibble;

  return result;
}

/* Open a file in flash memory to save the schedule */
int open_file(uint8_t mode)
{
  if(!open)
  {
    open = 1;
    fd = cfs_open(FILENAME, mode);
    if(fd < 0) {
      LOG_ERR("failed to open %s\n", FILENAME);
      open = 0;
    }
  }
  return open;
}

/* Only called from the Distributor when receiving data over serial input.
  Write the serial input to flash memory. */
void write_to_flash(uint8_t * data)
{
  int r = 0;
  int len = 0;

  while(*(data + len) != 0)
  {
    *(data + (len/2)) = asciiHex_to_int(data + len);
    len += 2;
  }

  r = cfs_write(fd, data, len/2);
  if(r != len/2) {
    LOG_ERR("Failed to write %d bytes to %s\n", len/2, FILENAME);
    cfs_close(fd);
    open = 0;
    return;
  }

  bytes_in_flash += r;
  LOG_DBG("Total bytes writen so far: %d \n", bytes_in_flash);
}

/* Used by all nodes, except distributor node, when receiving the schedule over radio */
uint8_t write_to_flash_offset(uint8_t * data, int offset, uint8_t len)
{
  if(!open_file(CFS_WRITE + CFS_READ))
  {
    return 0;
  }

  if(cfs_seek(fd, offset, CFS_SEEK_SET) != offset) {
    LOG_ERR("seek failed! %i\n", offset);
    cfs_close(fd);
    open = 0;
    return 0;
  }

  LOG_DBG("write %i to memory offset %i\n", len, offset);

  int wrote_bytes = cfs_write(fd, data, len);
  if(wrote_bytes != len) {
    LOG_ERR("Failed to write %d bytes. Wrote %d\n", len, wrote_bytes);
    cfs_close(fd);
    open = 0;
    return 0;
  }

  bytes_in_flash += wrote_bytes;
  LOG_DBG("Total bytes writen so far: %d \n", bytes_in_flash);
  return 1;
}

uint8_t read_flash(uint8_t * buf, int offset, uint8_t len)
{
  if(!open_file(CFS_WRITE + CFS_READ))
  {
    return 255;
  }

  if(cfs_seek(fd, offset, CFS_SEEK_SET) != offset) {
    LOG_ERR("seek failed %i\n", offset);
    cfs_close(fd);
    open = 0;
    return 255;
  }

  int read_bytes = cfs_read(fd, buf, len);

  if(read_bytes != len)
  {
    LOG_ERR("Error when reading from flash. Read %i, exptected %i\n", read_bytes, len);
    cfs_close(fd);
    open = 0;
    return 255;
  }

  LOG_DBG("read %i from requested %i bytes. Seek was %i\n", read_bytes, len, offset);
  return read_bytes;
}

uint8_t read_from_flash_by_id(master_tsch_schedule_universall_config_t * config, master_tsch_schedule_t * schedule, int id)
{
  if(!open_file(CFS_WRITE + CFS_READ))
  {
    return 0;
  }

  if(cfs_seek(fd, 0, CFS_SEEK_SET) != 0) {
    LOG_ERR("seek failed\n");
    cfs_close(fd);
    open = 0;
    return 0;
  }

  uint16_t read_config_bytes = 0;

  //First read the general config
  //2 bytes for schedule length and flow number
  read_config_bytes += cfs_read(fd, &config->schedule_length, 1);
  read_config_bytes += cfs_read(fd, &config->slot_frames, 1);

  //16 bytes for sender_of_flow and receiver_of_flow
  read_config_bytes += cfs_read(fd, &config->sender_of_flow, MASTER_NUM_FLOWS);
  read_config_bytes += cfs_read(fd, &config->receiver_of_flow, MASTER_NUM_FLOWS);

  //16 bytes for first/last tx slot in flow
  read_config_bytes += cfs_read(fd, &config->first_tx_slot_in_flow, MASTER_NUM_FLOWS);
  read_config_bytes += cfs_read(fd, &config->last_tx_slot_in_flow, MASTER_NUM_FLOWS);

  if(read_config_bytes != 4*MASTER_NUM_FLOWS + 2) {
    LOG_ERR("Failed to read %d bytes, read only %d",4*MASTER_NUM_FLOWS + 2, read_config_bytes);
    cfs_close(fd);
    open = 0;
    return 0;
  }

  uint8_t idx = 0;
  uint16_t read_schedule_bytes = 0;
  int total_bytes_read = 0;
  int bytes_that_should_be_read = 0;

  do{
    read_schedule_bytes = cfs_read(fd, &idx, 1);
    bytes_that_should_be_read++;

    read_schedule_bytes += cfs_read(fd, &schedule->own_transmission_flow, 1);
    bytes_that_should_be_read++;

    //In this case own_transmission_flow = own_receiver = 0
    if(schedule->own_transmission_flow == 0)
    {
      schedule->own_receiver = 0;
    }
    else{
      read_schedule_bytes += cfs_read(fd, &schedule->own_receiver, 1);
      bytes_that_should_be_read++;
    }
    
    //Read links
    read_schedule_bytes += cfs_read(fd, &schedule->links_len, 1);
    bytes_that_should_be_read++;
    read_schedule_bytes += cfs_read(fd, &schedule->links, schedule->links_len * sizeof(scheduled_link_t));
    bytes_that_should_be_read += schedule->links_len * sizeof(scheduled_link_t);

    //Read MASTER_NUM_FLOW flow_forwards
    read_schedule_bytes += cfs_read(fd, &schedule->flow_forwards, 8);
    bytes_that_should_be_read += 8;

    //Read MASTER_NUM_FLOW max_transmission
    read_schedule_bytes += cfs_read(fd, &schedule->max_transmission, 8);
    bytes_that_should_be_read += 8;

    if(read_schedule_bytes != bytes_that_should_be_read) {
      LOG_INFO("Should read %d bytes; read %d bytes. End of schedule\n", bytes_that_should_be_read, read_schedule_bytes);
      cfs_close(fd);
      open = 0;
      return 0;
    }
    total_bytes_read += read_schedule_bytes;

    if(idx != id)
    {
      LOG_DBG("Read %i for node %i. not our id, next\n", read_schedule_bytes, idx + 1);
    }

    bytes_that_should_be_read = 0;
  }while(idx != id && total_bytes_read < bytes_in_flash);

  cfs_close(fd);
  open = 0;

  LOG_DBG("Read %i bytes for id %i\n", read_schedule_bytes, id);
  return 1;
}

//Start the serial input protothread
PROCESS_THREAD(serial_line_schedule_input, ev, data)
{
  PROCESS_BEGIN();
  LOG_INFO("Started listening\n");
  fd = 0;
  bytes_in_flash = 0;

  while(schedule_loading) {
    PROCESS_YIELD();
    if(ev == serial_line_event_message) {
      LOG_INFO("Received input %s\n", (uint8_t *)data);
    
      uint8_t message_prefix = asciiHex_to_int(data);

      switch ((int)message_prefix)
      {
      case MESSAGE_BEGIN:
        LOG_DBG("Message start\n");
        open_file(CFS_WRITE + CFS_READ);
        write_to_flash((uint8_t *)(data + 2));
        break;
      case MESSAGE_CONTINUE:
        LOG_DBG("Message continues\n");
        write_to_flash((uint8_t *)(data + 2));
        break;
      case MESSAGE_END:
        LOG_DBG("Message end\n");
        write_to_flash((uint8_t *)(data + 2));

        static struct pt child_pt;
        PT_SPAWN(&start_slot_operation_pt, &child_pt, setUploadDone(&child_pt));
        LOG_INFO("Finish reading\n");
        tsch_disassociate_synch();
        schedule_loading = 0;

        break;
      default:
        LOG_ERR("dont know messagepart %d \n", message_prefix);
        break;
      }
    }
  }

  LOG_INFO("Finished process serial input\n");
  PROCESS_END();
}

struct master_tsch_schedule_t* get_own_schedule()
{
  return &schedule;
}

int get_node_receiver()
{
  return get_own_schedule()->own_receiver;
}

int node_is_sender()
{
  //We are a sender if we have own_receiver != 0
  return get_node_receiver() != 0;
}

uint8_t get_forward_dest_by_slotframe(master_tsch_schedule_t* schedule, uint8_t link_idx)
{
  return schedule->flow_forwards[schedule->links[link_idx].slotframe_handle - 1];
}

uint8_t get_forward_dest(master_tsch_schedule_t* schedule, uint8_t flow)
{
  return schedule->flow_forwards[flow - 1];
}

uint8_t get_max_transmissions(master_tsch_schedule_t* schedule, uint8_t flow)
{
  return schedule->max_transmission[flow - 1];
}

void setBit(uint32_t *bit_vector, int k)
{
  bit_vector[k/32] |= 1 << (k%32);
}

void clearBit(uint32_t *bit_vector, int k)
{
  bit_vector[k/32] &= ~(1 << (k%32)); 
}

uint8_t isBitSet(uint32_t *bit_vector, int k)
{
  if ( (bit_vector[k/32] & (1 << (k%32) ))  ) 
  {
      return 1;
  }else{
      return 0;
  }
}

void setBit1Byte(uint8_t *bit_vector, int k)
{
  bit_vector[k/8] |= 1 << (k%8);
}

uint8_t isBitSet1Byte(uint8_t *bit_vector, int k)
{
  if ( (bit_vector[k/8] & (1 << (k%8) ))  ) 
  {
      return 1;
  }else{
      return 0;
  }
}

void resetBitVector(uint32_t *bit_vector, uint8_t n)
{
  memset(bit_vector, 0, n * sizeof(uint32_t));
}

int getMissingPacket(uint32_t *bit_vector, int last_packet)
{
  //We start with packets counting from 1
  int position = 1;
  while(isBitSet(bit_vector, position) && position <= last_packet)
  {
    position++;
  }

  if(position > last_packet)
  {
    return -1;
  }else{
    return position;
  }
}