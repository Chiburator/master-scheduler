CONTIKI_PROJECT = master-unicast
all: $(CONTIKI_PROJECT)

CONTIKI = ../../..

MAKE_MAC = MAKE_MAC_TSCH
MAKE_NET = MAKE_NET_OTHER

MODULES += os/net/master-net
MODULES += os/net/master-routing

#CFLAGS erweitern -fno-stack-protector
#CFLAGS += execstack -s obj_cooja/tsch.o

MODULES += os/services/simple-energest

CLFAGS += -fno-stack-protector
include $(CONTIKI)/Makefile.include