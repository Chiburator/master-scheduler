/** 
 * \file
 *         Source file for the Master schedule module.
 * \author
 *         Viktor Paskal 
 *
 */
#include "contiki.h"
#include "master-schedule.h"
#include "dev/serial-line.h"
#include "os/storage/cfs/cfs.h"
#include "os/storage/cfs/cfs-coffee.h"
//#include "cpu/msp430/dev/uart0.h"
#define FILENAME "meinTest.bin"
/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "MASTER-S"
#define LOG_LEVEL LOG_LEVEL_DBG

PROCESS(serial_line_schedule_input, "Master scheduler serial line input");

static masternet_schedule_loaded schedule_loaded_callback = NULL;

uint8_t schedule_index;
int fd;
int total_bytes_written;
int idx_test;

void master_schedule_set_schedule_loaded_callback(masternet_schedule_loaded callback)
{
  schedule_loaded_callback = callback;
}

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
    lowerNibble = lowerNibble - (uint8_t)'A';
  }else{
    lowerNibble = lowerNibble - (uint8_t)'0';
  }


  if(HigherNibble >= (uint8_t) 'A')
  {
    HigherNibble = HigherNibble - (uint8_t)'A';
  }else{
    HigherNibble = HigherNibble - (uint8_t)'0';
  }

  
  uint8_t result = (HigherNibble << 4) + lowerNibble;
  //LOG_ERR("READTEST make %d to %d and %d to %d = %d\n",*(asciiHex), lowerNibble,  *(asciiHex + 1), HigherNibble, result);
  return result;
}

void open_file()
{
  fd = cfs_open(FILENAME, CFS_READ + CFS_WRITE);
  if(fd < 0) {
    LOG_ERR("READTEST failed to open %s\n", FILENAME);
  }
}

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

  total_bytes_written += r;
  LOG_ERR("Writting bytes %d \n", total_bytes_written);
}

void close_file()
{
 cfs_close(fd);
}

void read_file()
{
  /* To read back the message, we need to move the file pointer to the
  beginning of the file. */
  if(cfs_seek(fd, 0, CFS_SEEK_SET) != 0) {
    printf("READTEST seek failed\n");
    cfs_close(fd);
  }

  int count_bytes_read = 0;
  int count_bytes_to_be_read = 0;

  //first read the general part of the config
  //2 bytes for schedule length and flow number
  count_bytes_read += cfs_read(fd, &schedule_config.schedule_length, 1);
  count_bytes_read += cfs_read(fd, &schedule_config.slot_frames, 1);
  count_bytes_to_be_read += 2;

  //16 bytes for sender_of_flow and receiver_of_flow
  count_bytes_read += cfs_read(fd, &schedule_config.sender_of_flow, 8);
  count_bytes_read += cfs_read(fd, &schedule_config.receiver_of_flow, 8);
  count_bytes_to_be_read += 16;

  //16 bytes for first/last tx slot in flow
  count_bytes_read += cfs_read(fd, &schedule_config.first_tx_slot_in_flow, 8);
  count_bytes_read += cfs_read(fd, &schedule_config.last_tx_slot_in_flow, 8);
  count_bytes_to_be_read += 16;

  if(count_bytes_read != count_bytes_to_be_read) {
    LOG_ERR("Failed to read %d bytes, read only %d",count_bytes_to_be_read, count_bytes_read);
    cfs_close(fd);
    total_bytes_written = count_bytes_read;
  }
  total_bytes_written -= count_bytes_read;
  LOG_ERR("Wrote %d for general config, left %d\n", count_bytes_read, total_bytes_written);
  while (total_bytes_written > 0)
  {
    count_bytes_read = 0;
    count_bytes_to_be_read = 0;
    //1 byte = index of schedule
    //2  = if 0 then own_transmission_flow = 0 = own_receiver
    //otherwise byte 3 belongs to own-receiver and 2 to own_transmission_flow
    //4 links len + links following
    // afterwards MASTER_NUM_FLOW long flow_forwards
    //then len for cha_idx_to_dest + 2*MASTER_NUM_FLOW cha_idx
    //MASTER_NUM_FLOW long max_transmissions
    
    //Read schedule 
    uint8_t idx = 255;
    count_bytes_read += cfs_read(fd, &idx, 1);
    count_bytes_read += cfs_read(fd, &schedules[idx].own_transmission_flow, 1);
    
    count_bytes_to_be_read += 2;

    //In this case own_transmission_flow = own_receiver = 0
    if(schedules[idx].own_transmission_flow == 0)
    {
      schedules[idx].own_receiver = 0;
    }
    else{
      count_bytes_read += cfs_read(fd, &schedules[idx].own_receiver, 1);
      count_bytes_to_be_read++;
    }

    //Read links
    count_bytes_read += cfs_read(fd, &schedules[idx].links_len, 1);
    count_bytes_read += cfs_read(fd, &schedules[idx].links, schedules[idx].links_len * sizeof(scheduled_link_t));
    count_bytes_to_be_read += 1 + schedules[idx].links_len * sizeof(scheduled_link_t);
    //Read MASTER_NUM_FLOW flow_forwards
    count_bytes_read += cfs_read(fd, &schedules[idx].flow_forwards, 8);
    count_bytes_to_be_read += 8;
    //TODO:: Read 2*MASTER_NUM_FLOW cha_idx_to_dest
    //r = cfs_read(fd, &schedules[idx].cha_idx_to_dest, 8*2);
    //uint8_t len = 255;
    //int i;

    // uint8_t pos = 0;
    // count_bytes_read += cfs_read(fd, &len, 1);
    // count_bytes_to_be_read++;
    // for(i = 0; i < len; i+=2)
    // {
    //   count_bytes_read += cfs_read(fd, &pos, 1);
    //   count_bytes_read += cfs_read(fd, &schedules[idx].cha_idx_to_dest[pos], 1);
    //   count_bytes_to_be_read += 2;
    // }
    //Read MASTER_NUM_FLOW max_transmission
    count_bytes_read += cfs_read(fd, &schedules[idx].max_transmission, 8);
    count_bytes_to_be_read += 8;

    if(count_bytes_read != count_bytes_to_be_read) {
      LOG_ERR("Failed to read %d bytes, read only %d",count_bytes_to_be_read, count_bytes_read);
      cfs_close(fd);
      total_bytes_written = count_bytes_read;
    }

    total_bytes_written -= count_bytes_read;
    LOG_ERR("Wrote %d for idx %d, left %d\n", count_bytes_read, idx, total_bytes_written);
  }

  total_bytes_written = 0;
  cfs_close(fd);
}

void show_bytes()
{
  int i;
  char string[360] = {0};
  int string_offset = 0;

  if(idx_test >= 4)
  {
    idx_test = 0;
  }
  string_offset += sprintf(&string[string_offset], "%i ", (int) schedule_config.schedule_length);
  string_offset += sprintf(&string[string_offset], "%i ", (int) schedule_config.slot_frames);
  printf("schedule_length and slot_frames = %s for node %d\n", string, idx_test + 1);

  memset(string, 0, 360);
  string_offset = 0;
  for(i = 0; i < 8; i++)
  {
    string_offset += sprintf(&string[string_offset], "%i ", (int) schedule_config.sender_of_flow[i]);
  }
  printf("sender_of_flow = %s\n", string);

  memset(string, 0, 360);
  string_offset = 0;
  for(i = 0; i < 8; i++)
  {
    string_offset += sprintf(&string[string_offset], "%i ", (int) schedule_config.receiver_of_flow[i]);
  }
  printf("receiver_of_flow = %s\n", string);

  memset(string, 0, 360);
  string_offset = 0;
  for(i = 0; i < 8; i++)
  {
    string_offset += sprintf(&string[string_offset], "%i ", (int) schedule_config.first_tx_slot_in_flow[i]);
  }
  printf("first_tx_slot_in_flow = %s\n", string);

  memset(string, 0, 360);
  string_offset = 0;
  for(i = 0; i < 8; i++)
  {
    string_offset += sprintf(&string[string_offset], "%i ", (int) schedule_config.last_tx_slot_in_flow[i]);
  }
  printf("last_tx_slot_in_flow = %s\n", string);

  memset(string, 0, 360);
  string_offset = 0;
  string_offset += sprintf(&string[string_offset], "%i ", (int) schedules[idx_test].own_transmission_flow);
  string_offset += sprintf(&string[string_offset], "%i ", (int) schedules[idx_test].own_receiver);
  printf("own_transmission_flow and own_receiver = %s\n", string);

  memset(string, 0, 360);
  string_offset = 0;
  for(i = 0; i < 8; i++)
  {
    string_offset += sprintf(&string[string_offset], "%i ", (int) schedules[idx_test].flow_forwards[i]);
  }
  printf("own flow_forwards = %s\n", string);
  memset(string, 0, 360);
  string_offset = 0;
  for(i = 0; i < 8; i++)
  {
    string_offset += sprintf(&string[string_offset], "%i ", (int) schedules[idx_test].max_transmission[i]);
  }
  printf("own max_transmission = %s\n", string);
  memset(string, 0, 360);
  string_offset = 0;
  for(i = 0; i < schedules[idx_test].links_len; i++)
  {
    string_offset += sprintf(&string[string_offset], "%i ", (int) schedules[idx_test].links[i].slotframe_handle);
    string_offset += sprintf(&string[string_offset], "%i ", (int) schedules[idx_test].links[i].send_receive);
    string_offset += sprintf(&string[string_offset], "%i ", (int) schedules[idx_test].links[i].timeslot);
    string_offset += sprintf(&string[string_offset], "%i ", (int) schedules[idx_test].links[i].channel_offset);
  }
  printf("%d Links with links = %s\n", schedules[idx_test].links_len, string);
  idx_test++;
}

PROCESS_THREAD(serial_line_schedule_input, ev, data)
{
  PROCESS_BEGIN();
  schedule_index = 0;
  fd = 0;
  total_bytes_written = 0;
  idx_test= 0;
  uint8_t schedule_loaded = 0;

  while(schedule_loaded != 1) {
    PROCESS_YIELD();
    if(ev == serial_line_event_message) {
      int current_bytes = total_bytes_written;
    
      uint8_t message_prefix = asciiHex_to_int(data);

      switch ((int)message_prefix)
      {
      case MESSAGE_BEGIN:
        open_file();
        write_to_flash((uint8_t *)(data + 2));
        break;
      case MESSAGE_CONTINUE:
        write_to_flash((uint8_t *)(data + 2));
        break;
      case MESSAGE_END:
        write_to_flash((uint8_t *)(data + 2));
        read_file();
        if(schedule_loaded_callback != NULL)
        {
          schedule_loaded_callback();
          schedule_loaded = 1;
        }
        break;
      default:
        LOG_ERR("dont know messagepart %d \n", message_prefix);
        show_bytes();
        break;
      }
      LOG_ERR("received data. Read %d, total bytes written now %d\n", total_bytes_written - current_bytes, total_bytes_written);
    }
  }
  LOG_ERR("Finished process serial input\n");
  PROCESS_END();
}

struct master_tsch_schedule_t* get_own_schedule()
{
  return &schedules[linkaddr_node_addr.u8[NODE_ID_INDEX] - 1];
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