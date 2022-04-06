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

#define FILENAME "meinTest.bin"
/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "MASTER-S"
#define LOG_LEVEL LOG_LEVEL_DBG

PROCESS(serial_line_schedule_input, "Master scheduler serial line input");

uint8_t schedule_loading = 1;

int fd;
int bytes_in_flash;
int idx_test;
uint8_t open = 0;

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
  //LOG_ERR("READTEST make %d to %d and %d to %d = %d\n",*(asciiHex), lowerNibble,  *(asciiHex + 1), HigherNibble, result);
  return result;
}

void open_file(uint8_t mode)
{
  open = 1;
  fd = cfs_open(FILENAME, mode);
  if(fd < 0) {
    LOG_ERR("READTEST failed to open %s\n", FILENAME);
    open = 0;
  }
}

/* Only called from the Distributor when receiving data over serial input */
void write_to_flash(uint8_t * data)
{
  int r = 0;
  int len = 0;
  //int c = 0;
  while(*(data + len) != 0)
  {
    // if(c == 19)
    // {
    //   c = 0;
    //   printf("\n");
    // }
    *(data + (len/2)) = asciiHex_to_int(data + len);
    //printf("%x ", *(data + (len/2)));
    len += 2;
    //c++;
  }
  //printf("\n");
  r = cfs_write(fd, data, len/2);
  if(r != len/2) {
    LOG_ERR("Failed to write %d bytes to %s\n", len/2, FILENAME);
    cfs_close(fd);
    return;
  }

  bytes_in_flash += r;
  LOG_ERR("Total bytes writen so far: %d \n", bytes_in_flash);
}

/* Used by all nodes, except distributor node, when receiving the schedule over radio */
void write_to_flash_offset(uint8_t * data, int offset, uint8_t len)
{
  if(!open)
  {
    open_file(CFS_WRITE + CFS_READ);
  }

  if(cfs_seek(fd, offset, CFS_SEEK_SET) != offset) {
    printf("write to flash failed! seek failed!\n");
    cfs_close(fd);
  }
  printf("write %i to %i\n", len, offset);
  int wrote_bytes = cfs_write(fd, data, len);
  if(wrote_bytes != len) {
    LOG_ERR("Failed to write %d bytes to %s. Wrote %d\n", len, FILENAME, wrote_bytes);
    cfs_close(fd);
  }

  bytes_in_flash += wrote_bytes;
  LOG_ERR("Total bytes writen so far: %d \n", bytes_in_flash);
}

uint8_t read_flash(uint8_t * buf, int offset, uint8_t len)
{
  if(!open)
  {
    open_file(CFS_WRITE + CFS_READ);
  }

  if(cfs_seek(fd, offset, CFS_SEEK_SET) != offset) {
    printf("seek failed\n");
    cfs_close(fd);
  }

  int read_bytes = cfs_read(fd, buf, len);

  if(read_bytes != len)
  {
    printf("Error when reading from flash. Read %i, exptected %i\n", read_bytes, len);
    cfs_close(fd);
    open = 0;
  }
  printf("read %i from requested %i bytes. Seek was %i\n", read_bytes, len, offset);
  return read_bytes;
}

void get_next_link(scheduled_link_t *link)
{
  cfs_read(fd, link, 4);
}

void get_test(uint8_t *own_receiver, uint8_t *own_transmission_flow, uint8_t *links_len)
{
  cfs_read(fd, own_transmission_flow, 1);

  //In this case own_transmission_flow = own_receiver = 0
  if(*own_transmission_flow == 0)
  {
    *own_receiver = 0;
  }
  else{
    cfs_read(fd, own_receiver, 1);
  }

  cfs_read(fd, links_len, 1);
}

uint8_t find_schedule(master_tsch_schedule_universall_config_t * config, int id)
{
  if(!open)
  {
    open_file(CFS_WRITE + CFS_READ);
  }

  if(cfs_seek(fd, 0, CFS_SEEK_SET) != 0) {
    printf("READTEST seek failed\n");
    cfs_close(fd);
  }

  uint16_t read_config_bytes = 0;
  //First read the general config
  //2 bytes for schedule length and flow number
  read_config_bytes += cfs_read(fd, &schedule_config.schedule_length, 1);
  read_config_bytes += cfs_read(fd, &schedule_config.slot_frames, 1);

  //16 bytes for sender_of_flow and receiver_of_flow
  read_config_bytes += cfs_read(fd, &schedule_config.sender_of_flow, MASTER_NUM_FLOWS);
  read_config_bytes += cfs_read(fd, &schedule_config.receiver_of_flow, MASTER_NUM_FLOWS);

  //16 bytes for first/last tx slot in flow
  read_config_bytes += cfs_read(fd, &schedule_config.first_tx_slot_in_flow, MASTER_NUM_FLOWS);
  read_config_bytes += cfs_read(fd, &schedule_config.last_tx_slot_in_flow, MASTER_NUM_FLOWS);

  if(read_config_bytes != 4*MASTER_NUM_FLOWS + 2) {
    LOG_ERR("Failed to read %d bytes, read only %d",4*MASTER_NUM_FLOWS + 2, read_config_bytes);
    cfs_close(fd);
    return 0;
  }

  LOG_DBG("Wrote %d for general config\n", read_config_bytes);

  
  uint8_t idx = 0;
  uint16_t read_schedule_bytes = 0;
  int total_bytes_read = 0;

  cfs_read(fd, &idx, 1);
  while(idx != id && total_bytes_read < bytes_in_flash)
  {
    printf("not our id (%i)\n", idx);
    uint8_t trans_flow;
    cfs_read(fd, &trans_flow, 1);

    //In this case own_transmission_flow = own_receiver = 0
    if(trans_flow != 0)
    {
      cfs_read(fd, &trans_flow, 1);
    }

    int links_len;
    read_schedule_bytes += cfs_read(fd, &links_len, 1);

    if(cfs_seek(fd, 16+links_len*4, CFS_SEEK_CUR) != 16+links_len*4) {
      printf("READTEST seek failed %i\n", 16+links_len*4);
      cfs_close(fd);
    }
  }

  return idx == id;
}

uint8_t read_from_flash_by_id(master_tsch_schedule_universall_config_t * config, master_tsch_schedule_t * schedule, int id)
{
  if(!open)
  {
    open_file(CFS_WRITE + CFS_READ);
  }

  if(cfs_seek(fd, 0, CFS_SEEK_SET) != 0) {
    printf("READTEST seek failed\n");
    cfs_close(fd);
  }

  uint16_t read_config_bytes = 0;
  //First read the general config
  //2 bytes for schedule length and flow number
  read_config_bytes += cfs_read(fd, &schedule_config.schedule_length, 1);
  read_config_bytes += cfs_read(fd, &schedule_config.slot_frames, 1);

  //16 bytes for sender_of_flow and receiver_of_flow
  read_config_bytes += cfs_read(fd, &schedule_config.sender_of_flow, MASTER_NUM_FLOWS);
  read_config_bytes += cfs_read(fd, &schedule_config.receiver_of_flow, MASTER_NUM_FLOWS);

  //16 bytes for first/last tx slot in flow
  read_config_bytes += cfs_read(fd, &schedule_config.first_tx_slot_in_flow, MASTER_NUM_FLOWS);
  read_config_bytes += cfs_read(fd, &schedule_config.last_tx_slot_in_flow, MASTER_NUM_FLOWS);

  if(read_config_bytes != 4*MASTER_NUM_FLOWS + 2) {
    LOG_ERR("Failed to read %d bytes, read only %d",4*MASTER_NUM_FLOWS + 2, read_config_bytes);
    cfs_close(fd);
    return 0;
  }

  LOG_DBG("Wrote %d for general config\n", read_config_bytes);

  uint8_t idx = 0;
  uint16_t read_schedule_bytes = 0;
  int total_bytes_read = 0;

  int i = 0;
  char print_bytes[500] = {0};
  int offset=0;

  do{
    memset(print_bytes, 0, 500);
    offset = 0;
    cfs_read(fd, &idx, 1);

    read_schedule_bytes = cfs_read(fd, &schedule->own_transmission_flow, 1);

    //In this case own_transmission_flow = own_receiver = 0
    if(schedule->own_transmission_flow == 0)
    {
      schedule->own_receiver = 0;
    }
    else{
      cfs_read(fd, &schedule->own_receiver, 1);
    }
    read_schedule_bytes++;

    offset += sprintf(&print_bytes[offset], "%i ", (int) schedule->own_transmission_flow);
    offset += sprintf(&print_bytes[offset], "%i ", (int) schedule->own_receiver);


    //Read links
    read_schedule_bytes += cfs_read(fd, &schedule->links_len, 1);
    read_schedule_bytes += cfs_read(fd, &schedule->links, schedule->links_len * sizeof(scheduled_link_t));

    offset += sprintf(&print_bytes[offset], "%i ", (int) schedule->links_len);
    // for(i = 0; i < schedule->links_len; i++)
    // {
    //   offset += sprintf(&print_bytes[offset], "%i ", (int) schedule->links[i].slotframe_handle);
    //   offset += sprintf(&print_bytes[offset], "%i ", (int) schedule->links[i].send_receive);
    //   offset += sprintf(&print_bytes[offset], "%i ", (int) schedule->links[i].timeslot);
    //   offset += sprintf(&print_bytes[offset], "%i ", (int) schedule->links[i].channel_offset);
    // }

    //Read MASTER_NUM_FLOW flow_forwards
    read_schedule_bytes += cfs_read(fd, &schedule->flow_forwards, 8);
    for(i = 0; i < MASTER_NUM_FLOWS; i++)
    {
      offset += sprintf(&print_bytes[offset], "%i ", (int) schedule->flow_forwards[i]);
    }


    //Read MASTER_NUM_FLOW max_transmission
    read_schedule_bytes += cfs_read(fd, &schedule->max_transmission, 8);
    for(i = 0; i < MASTER_NUM_FLOWS; i++)
    {
      offset += sprintf(&print_bytes[offset], "%i ", (int) schedule->max_transmission[i]);
    }
    int total_size = schedule->links_len * sizeof(scheduled_link_t) + 2 * MASTER_NUM_FLOWS + 3;

    if(read_schedule_bytes != total_size) {
      printf("Failed to read %d bytes, read only %d", total_size, read_schedule_bytes);
      cfs_close(fd);
      return 0;
    }
    total_bytes_read += read_schedule_bytes;

    if(idx != id)
    {
      LOG_DBG("Read %i for node %i. not our id, next\n", read_schedule_bytes, idx + 1);
    }

  }while(idx != id && total_bytes_read < bytes_in_flash);

  if(schedule->links > 0){
    LOG_DBG("Read %i bytes for id %i\n", read_schedule_bytes, id);
    printf("read %s\n", print_bytes);
  }else{
    LOG_DBG("This node has nothing to do\n");
  }

  cfs_close(fd);

  return 1;
}

void close_file()
{
 cfs_close(fd);
}

static struct pt test_pt;
//static struct ctimer testctimer;
PROCESS_THREAD(serial_line_schedule_input, ev, data)
{
  PROCESS_BEGIN();
  LOG_ERR("Started listening\n");
  fd = 0;
  bytes_in_flash = 0;
  idx_test= 0;

  while(schedule_loading) {
    PROCESS_YIELD();
    if(ev == serial_line_event_message) {
      LOG_ERR("Received input %s\n", (uint8_t *)data);
    
      uint8_t message_prefix = asciiHex_to_int(data);

      switch ((int)message_prefix)
      {
      case MESSAGE_BEGIN:
        printf("Message start\n");
        open_file(CFS_WRITE + CFS_READ);
        write_to_flash((uint8_t *)(data + 2));
        break;
      case MESSAGE_CONTINUE:
        printf("Message continues\n");
        write_to_flash((uint8_t *)(data + 2));
        break;
      case MESSAGE_END:
        printf("Message end\n");
        write_to_flash((uint8_t *)(data + 2));

        static struct pt child_pt;
        PT_SPAWN(&test_pt, &child_pt, setUploadDone(&child_pt));
        printf("Finish reading\n");
        tsch_disasssociate_synch();
        schedule_loading = 0;

        break;
      default:
        LOG_ERR("dont know messagepart %d \n", message_prefix);
        break;
      }
    }
  }

  LOG_ERR("Finished process serial input\n");
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

//Flow_forwards array starts at 0 and flow index at 1, therefore access the correct forward by slotframe - 1
uint8_t get_forward_dest_by_slotframe(master_tsch_schedule_t* schedule, uint8_t link_idx)
{
  return schedule->flow_forwards[schedule->links[link_idx].slotframe_handle - 1];
}

//Flow_forwards array starts at 0 and flow index at 1, therefore access the correct forward by slotframe - 1
uint8_t get_forward_dest(master_tsch_schedule_t* schedule, uint8_t flow)
{
  return schedule->flow_forwards[flow - 1];
}

//max transmissions array starts at 0 and flow index at 1, therefore access the correct forward by slotframe - 1
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