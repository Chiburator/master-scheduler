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
* **master-unicast/project-conf.h**: The amount of neighbors changed to 16. Setting up a network with more than 16 neighbors at any node requires a change at variable `NBR_TABLE_CONF_MAX_NEIGHBORS`. The payload size was decreased from 64 bytes to 59 bytes to allow overhearing during schedule distribution in unicasts
* **frame802154e-ie.c**: New information elements were added.
* **master-schedule**: This module contains the process to receive serial line input on the distributor node and all read/write functions to access the flash memory. Reading the schedule and writing a new schedule to flash can be accomplished through this module. The bit-vector is implemented here as well.
* **master-routing**: This module contains the logic to handle ETX-metric gathering and schedule distribution, as well as the routing from the initial version.
* **tsch.c & tsch-slot-operation**: Overhearing of unicast transmissions is implemented through these modules.

# How to use the new version of MASTER

The new version of MASTER is able to upload a schedule at runtime. There are two ways to generate and upload a schedule. If a filepath and a filename is submitted to MASTER as a parameter, MASTER will create a schedule from the file.

    master_scheduler.py -dir "<FILE_PATH>" -f <FILE_NAME> ...
    
Otherwise, MASTER will create a schedule by connecting to the __CPAN__ and reading the output. Once the __CPAN__ received the complete ETX-metric, MASTER will upload the schedule to the __distributor node__. The __CPAN__ and the __distributor node__ are currently hard-coded depending on the environment. 
For Cooja, MASTER will connect to localhost at port 60007 to upload the schedule. Received the ETX-metric automatically from a simulated node in Cooja is not implemented.
For the testbed at Kiel, MASTER will connect to node 8 as the __CPAN__ and node 7 as the __distributor node__. Changing the __CPAN__ or __distributor node__ in the configurations requires a change in the python file for MASTER.

## MASTER with Cooja (Windows)

1. Start the docker container and Cooja
2. Connect with a second cmd to the docker container.


        docker exec -it <ID> bash
        
3. Start a Simulation in Cooja and open a *Serial Socket Server* for the __distributor node__ with port 60007.
4. Let the network run until node 7 stops participating in the Network. At this point, the ETX-metric was received.
5. Download the file and start MASTER with the filepath and filename as a parameter. The filename for the output file has to be **meinTest.bin**, e.G.
    
    
        -dir "<FILE_PATH>" -f <FILE_NAME>  -tb "cooja" -n_cooja 21 -flows "16,10" -with_cs -out "meinTest.bin" -m_len 50 -p_etx -p_prr -p_sched 
    
6. Runt the *upload_schedule.py* from the second cmd to upload the generated binary to the __distributor node__.
    
## MASTER with the testbed at Kiel
    
1. Start a test at the testbed
2. Start MASTER without a filepath and filename, e.G.

        python3 master_scheduler.py -n_cooja 20 -flows "18,4;2,21;8,20;9,1;6,17;16,10" -tb "kiel" -with_cs -out "../meinTest.bin" -m_len 101 -p_etx -p_prr -p_sched

3. MASTER will connect to node 8 as the __CPAN__ and node 7 as the __distributor node__. Once the Schedule is received, MASTER will upload the schedule automatically and keep two threads open to both nodes. Once the test is finished, MASTER will be terminated.

The threads are required since the testbed pipes the output of the nodes to a netcat server. If the output is not read, the pipe operator will drop the output after 64 KB.
