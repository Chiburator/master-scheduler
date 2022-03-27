#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

#undef MASTER_MSG_LENGTH
#define MASTER_MSG_LENGTH 59

#undef  TESTBED
//#define TESTBED TESTBED_COOJA
#define TESTBED TESTBED_KIEL

#undef NUM_COOJA_NODES
#define NUM_COOJA_NODES 20

#undef NBR_TABLE_CONF_MAX_NEIGHBORS
#define NBR_TABLE_CONF_MAX_NEIGHBORS 16

#undef TSCH_SCHEDULE_CONF_MAX_LINKS //use output of Scheduler
#define TSCH_SCHEDULE_CONF_MAX_LINKS 40

//TX-POWER: // probably only one of the two options below needed
#undef MASTER_CONF_CC2538_TX_POWER
#define MASTER_CONF_CC2538_TX_POWER -3

#ifdef CONTIKI_TARGET_ZOUL
#undef CC2538_RF_CONF_TX_POWER
#define CC2538_RF_CONF_TX_POWER  0xA1 // 0x88: -7 dBm; 0xA1: -3 dBm; 0xB0: -1 dBm; 0xB6: 0 dBm; 0xC5: 1 dBm; 0xD5: 3 dBm; 0xED: 5 dBm; 0xFF: 7 dBm
#endif

#define TSCH_CONF_WITH_CENTRAL_SCHEDULING    1
#define TSCH_CONF_FLOW_BASED_QUEUES          1
#define TSCH_CONF_TTL_BASED_RETRANSMISSIONS  1
//#define MASTER_TSCH_COORDINATOR 0x01
//hohe last = node 12 oder 13  0x0C / 0x0D
#define MASTER_TSCH_COORDINATOR 0x08
#define MASTER_TSCH_DISTRIBUTOR 0x07

#define LOG_CONF_LEVEL_MAC                         LOG_LEVEL_DBG

#include "net/master-routing/master-project-conf.h"

#endif /* PROJECT_CONF_H_ */