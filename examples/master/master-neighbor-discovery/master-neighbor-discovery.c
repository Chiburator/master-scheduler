/**
 * \file
 *         Neighbor discovery
 * \author
 *         Oliver Harms <oha@informatik.uni-kiel.de>
 *
 */

// includes
  #include "contiki.h"
  #include "net/master-routing/master-routing.h"
  #include "dev/serial-line.h"
  #include <string.h>
  #include "os/storage/cfs/cfs.h"
  #include "os/storage/cfs/cfs-coffee.h"
  //#include "cpu/msp430/dev/uart0.h"
  #define FILENAME "meinTest.bin"
/* Log configuration */
  #include "sys/log.h"
  #include "tsch-log.h"
  #define LOG_MODULE "App"
  #define LOG_LEVEL LOG_LEVEL_INFO

enum MessagePart{
  MESSAGE_BEGIN,
  MESSAGE_CONTINUE,
  MESSAGE_END
};

/*---------------------------------------------------------------------------*/
PROCESS(master_neighbor_discovery_process, "Master neighbor discovery example");
PROCESS(test_serial, "Master neighbor discovery example");
PROCESS(read_file, "Master neighbor discovery example");
AUTOSTART_PROCESSES(&master_neighbor_discovery_process);

//PROCESS(test_serial, "Serial line test process");

/*---------------------------------------------------------------------------*/
// void input_callback(const void *data, uint16_t len,
//  const linkaddr_t *src, const linkaddr_t *dest)
// {
//  if(len == sizeof(unsigned)) {
//    LOG_INFO("Received %u from ", *(unsigned *)data);
//    LOG_INFO_LLADDR(src);
//    LOG_INFO_("\n");
//  }
// }
/*---------------------------------------------------------------------------*/
uint8_t schedule_index = 0;
int fd = 0;
int total_bytes_written = 0;

// int
// test(unsigned char c)
// {
//   LOG_ERR("READTEST got byte %X", c);

//   return 1;
// }

uint8_t asciiHex_to_int(uint8_t *asciiHex)
{
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

  while(*(data + len) != (uint8_t)'\0')
  {
    *(data + (len/2)) = asciiHex_to_int(data + len);
    len += 2;
  }
  LOG_ERR("READTEST read len = %d and len/2 = %d\n", len, len/2);
  r = cfs_write(fd, data, len/2);
  if(r != len/2) {
    LOG_ERR("READTEST failed to write %d bytes to %s\n", len/2, FILENAME);
    cfs_close(fd);
    return;
  }

  total_bytes_written += r;
  LOG_ERR("READTEST Finished with %d\n", total_bytes_written);
}

void close_file()
{
 cfs_close(fd);
}

PROCESS_THREAD(read_file, ev, data)
{
  PROCESS_BEGIN();
  int r = 0;
  /* To read back the message, we need to move the file pointer to the
  beginning of the file. */
  if(cfs_seek(fd, 0, CFS_SEEK_SET) != 0) {
    printf("READTEST seek failed\n");
    cfs_close(fd);
  }
    TSCH_LOG_ADD(tsch_log_message,
    snprintf(log->message, sizeof(log->message),
    "READTEST before cfs_read\n"));
  //TODO:: process gets interrupted by TSCH and does not resume. check serial_line_init() and process_init()
  while (total_bytes_written > 0)
  {
    uint8_t testbuff[10] = {0};

    r = cfs_read(fd, testbuff, 10);
    //printf("READTEST after cfs_read %d\n", r);
    if(r != 10) {
      printf("READTEST failed to read %d bytes, read only %d",total_bytes_written, r);
      cfs_close(fd);
      total_bytes_written = r;
    }else{
      //printf("READTEST read %d bytes and %d left\n", r, total_bytes_written);
    }
    //printf("READTEST next itter\n");
    total_bytes_written -= r;
  }
  //printf("READTEST closing file\n");
  total_bytes_written = 0;
  cfs_close(fd);
  TSCH_LOG_ADD(tsch_log_message,
  snprintf(log->message, sizeof(log->message),
  "READTEST cfs_read done\n"));
  PROCESS_END();
}

PROCESS_THREAD(test_serial, ev, data)
{
  PROCESS_BEGIN();

  while(1) {
    LOG_ERR("READTEST going to yield\n");
    PROCESS_YIELD();
    if(ev == serial_line_event_message) {
      int current_bytes = total_bytes_written;
    
      uint8_t result = asciiHex_to_int(data);
      //uint8_t message_prefix = *(uint8_t *)data;
      switch ((int)result)
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
        process_start(&read_file, NULL);
        break;
      default:
        LOG_ERR("READTEST dont know messagepart %d \n", result);
        break;
      }
      LOG_ERR("READTEST received data. Read %d, total bytes written now %d\n", total_bytes_written - current_bytes, total_bytes_written);
    }
  }
  PROCESS_END();
}

PROCESS_THREAD(master_neighbor_discovery_process, ev, data)
{
  static struct etimer periodic_timer;
  //uint8_t success;

  PROCESS_BEGIN();

  process_start(&test_serial, NULL);

  /* Initialize Master */
  init_master_routing();
  //master_routing_set_input_callback(input_callback);

  etimer_set(&periodic_timer, CLOCK_SECOND);

  do {
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
      etimer_reset(&periodic_timer);
  } while(!master_routing_configured());

  if (node_is_sender()){
    while(1){
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
      //success = 
      master_routing_sendto(NULL, 0, 0); //TODO: nullpointer might break it
      //LOG_INFO("Success: %u", success);
      etimer_reset(&periodic_timer);
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/