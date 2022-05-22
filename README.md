## Master scheduler

Based on [Contiki-NG](https://github.com/contiki-ng/contiki-ng) and https://github.com/ds-kiel/master-scheduler

# Code Changes to the following files:

* master_scheduler.py
* neighbor_parser.py
* scheduling_contiki.py
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

* cfs-coffe-arch.h: Append only mode was deactivated to allow writing to different file locations.
* master-unicast/project_conf.h: The amount of neighbors changed now to 16. Setting up a network with more than 16 neighbors at any node requieres a change at this variable. The payload size was decreased from 64 bytes to 59 bytes to allow overhearing during schedule distribution in unicasts
* frame802154e-ie.c: New information elements were added.
* master-schedule: This module contains the process to receive serial line input on the distributor node and all read/write functions to access the flash memory.
* master-routing: This module contains the logic to handle ETX-metric gathering and schedule distribution as well as the routing from the initial version.
