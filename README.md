## Master scheduler

Based on [Contiki-NG](https://github.com/contiki-ng/contiki-ng) and https://github.com/ds-kiel/master-scheduler

# Code Changes to the following files:

* master_scheduler.py
* neighbor_parser.py
* scheduling_contiki.py
* upload_schedule.py
* neighbor_parser_enhanced_beacon.py
* cfs-coffe-arch.h
* master-neighbor-discovery.c
* master-neighbor-discovery/project_conf.h
* master-unicast.c
* master-unicast/project-conf.h
* frame802154e-ie.c
* frame802154e-ie.h
* framer-802154.c
* tsch-conf.h
* tsch-packet.c
* tsch-packet.h
* tsch-queue.c
* tsch-queue.h
* tsch-schedule.c
* tsch-schedule.h
* tsch-slot-operation.c
* tsch-slot-operation.h
* tsch.c
* tsch.h
* master-net.c
* master-net.h
* master-conf.h
* master-project-conf.h
* master-routing.c
* master-routing.h
* master-schedule.c
* master-schedule.h
* packetbuf.h

# Important Code changes:

* **cfs-coffe-arch.h**: Append only mode was deactivated to allow writing to different file locations.
* **master-unicast/project-conf.h**: The amount of neighbors changed to 16. Setting up a network with more than 16 neighbors at any node requieres a change at variable `NBR_TABLE_CONF_MAX_NEIGHBORS`. The payload size was decreased from 64 bytes to 59 bytes to allow overhearing during schedule distribution in unicasts
* **frame802154e-ie.c**: New information elements were added.
* **master-schedule**: This module contains the process to receive serial line input on the distributor node and all read/write functions to access the flash memory. Reading the schedule and writing a new schedule to flash can be accomplished through this module. The bit-vector is implemented here as well.
* **master-routing**: This module contains the logic to handle ETX-metric gathering and schedule distribution as well as the routing from the initial version.
* **tsch.c & tsch-slot-operation**: Overhearing of unicast transmissions is implemented through this modules.

# How to use the new version of MASTER

The new version of MASTER is able to upload a schedule at runtime. There are two ways to generate and upload a schedule. If a file path and a file name is submited to MASTER as a parameter, MASTER will create a schedule from the file.

    master_scheduler.py -dir "<FILE_PATH>" -f <FILE_NAME> ...
    
Otherwise, MASTER will create a schedule by connecting to the __CPAN__ and reading the output. Once the __CPAN__ received the complete ETX-metric, MASTER will upload the schedule to the __distributor node__.
The __CPAN__ and the __distributor node__ are currently hard-coded depending on the environment.

## MASTER in Cooja (Windows)

1. Start the docker container and Cooja
2. Connect with a second cmd to the docker container 

    docker exec -it <ID> bash
 
3. Start a Simulation in Cooja and open a *Serial Socker Server* for the __distributor node__.
