/*
 * Copyright (c) 2017, Inria.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

/**
 * \file
 *         Header file for the logging system
 * \author
 *         Simon Duquennoy <simon.duquennoy@inria.fr>
 */

/** \addtogroup sys
 * @{ */

/**
 * \defgroup log Per-module, per-level logging
 * @{
 *
 * The log module performs per-module, per-level logging
 *
 */

#ifndef __LOG_H__
#define __LOG_H__

#include <stdio.h>
#include "net/linkaddr.h"
#include "sys/log-conf.h"
#if NETSTACK_CONF_WITH_IPV6
#include "net/ipv6/uip.h"
#endif /* NETSTACK_CONF_WITH_IPV6 */

/* The different log levels available */
#define LOG_LEVEL_NONE         0 /* No log */
#define LOG_LEVEL_ERR          1 /* Errors */
#define LOG_LEVEL_WARN         2 /* Warnings */
#define LOG_LEVEL_INFO         3 /* Basic info */
#define LOG_LEVEL_DBG          4 /* Detailled debug */

/* Per-module log level */

struct log_module {
  const char *name;
  int *curr_log_level;
  int max_log_level;
};

//extern int curr_log_level_rpl;
//extern int curr_log_level_tcpip;
extern int curr_log_level_ipv6;
//extern int curr_log_level_6lowpan;
extern int curr_log_level_nullnet;
extern int curr_log_level_mac;
extern int curr_log_level_framer;
//extern int curr_log_level_6top;
extern int curr_log_level_main;
/* Main log function */
extern int count;
extern struct log_module all_modules[];

//#define LOG_LEVEL_RPL                         MIN((LOG_CONF_LEVEL_RPL), curr_log_level_rpl)
//#define LOG_LEVEL_TCPIP                       MIN((LOG_CONF_LEVEL_TCPIP), curr_log_level_tcpip)
#define LOG_LEVEL_IPV6                        MIN((LOG_CONF_LEVEL_IPV6), curr_log_level_ipv6)
//#define LOG_LEVEL_6LOWPAN                     MIN((LOG_CONF_LEVEL_6LOWPAN), curr_log_level_6lowpan)
#define LOG_LEVEL_NULLNET                     MIN((LOG_CONF_LEVEL_NULLNET), curr_log_level_nullnet)
#define LOG_LEVEL_MAC                         MIN((LOG_CONF_LEVEL_MAC), curr_log_level_mac)
#define LOG_LEVEL_FRAMER                      MIN((LOG_CONF_LEVEL_FRAMER), curr_log_level_framer)
//#define LOG_LEVEL_6TOP                        MIN((LOG_CONF_LEVEL_6TOP), curr_log_level_6top)
#define LOG_LEVEL_MAIN                        MIN((LOG_CONF_LEVEL_MAIN), curr_log_level_main)


#define LOG(newline, level, levelstr, ...) do {  \
                            if(level <= (LOG_LEVEL)) { \
                              if(newline) { \
                                if(LOG_WITH_MODULE_PREFIX) { \
                                  LOG_OUTPUT_PREFIX(level, levelstr, LOG_MODULE); \
                                } \
                                if(LOG_WITH_LOC) { \
                                  LOG_OUTPUT("[%s: %d] ", __FILE__, __LINE__); \
                                } \
                              } \
                              LOG_OUTPUT(__VA_ARGS__); \
                            } \
                          } while (0)

/* For Cooja annotations */
#define LOG_ANNOTATE(...) do {  \
                            if(LOG_WITH_ANNOTATE) { \
                              LOG_OUTPUT(__VA_ARGS__); \
                            } \
                        } while (0)

/* Link-layer address */
#define LOG_LLADDR(level, lladdr) do {  \
                            if(level <= (LOG_LEVEL)) { \
                              if(LOG_WITH_COMPACT_ADDR) { \
                                log_lladdr_compact(lladdr); \
                              } else { \
                                log_lladdr(lladdr); \
                              } \
                            } \
                        } while (0)

/* IPv6 address */
#define LOG_6ADDR(level, ipaddr) do {  \
                           if(level <= (LOG_LEVEL)) { \
                             if(LOG_WITH_COMPACT_ADDR) { \
                               log_6addr_compact(ipaddr); \
                             } else { \
                               log_6addr(ipaddr); \
                             } \
                           } \
                         } while (0)

/* More compact versions of LOG macros */
#define LOG_ERR(...)           LOG(1, LOG_LEVEL_ERR, "ERR", __VA_ARGS__)
#define LOG_WARN(...)          LOG(1, LOG_LEVEL_WARN, "WARN", __VA_ARGS__)
#define LOG_INFO(...)          LOG(1, LOG_LEVEL_INFO, "INFO", __VA_ARGS__)
#define LOG_DBG(...)           LOG(1, LOG_LEVEL_DBG, "DBG", __VA_ARGS__)

#define LOG_ERR_(...)           LOG(0, LOG_LEVEL_ERR, "ERR", __VA_ARGS__)
#define LOG_WARN_(...)          LOG(0, LOG_LEVEL_WARN, "WARN", __VA_ARGS__)
#define LOG_INFO_(...)          LOG(0, LOG_LEVEL_INFO, "INFO", __VA_ARGS__)
#define LOG_DBG_(...)           LOG(0, LOG_LEVEL_DBG, "DBG", __VA_ARGS__)

#define LOG_ERR_LLADDR(...)    LOG_LLADDR(LOG_LEVEL_ERR, __VA_ARGS__)
#define LOG_WARN_LLADDR(...)   LOG_LLADDR(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_INFO_LLADDR(...)   LOG_LLADDR(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_DBG_LLADDR(...)    LOG_LLADDR(LOG_LEVEL_DBG, __VA_ARGS__)

#define LOG_ERR_6ADDR(...)     LOG_6ADDR(LOG_LEVEL_ERR, __VA_ARGS__)
#define LOG_WARN_6ADDR(...)    LOG_6ADDR(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_INFO_6ADDR(...)    LOG_6ADDR(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_DBG_6ADDR(...)     LOG_6ADDR(LOG_LEVEL_DBG, __VA_ARGS__)

/* For testing log level */
#define LOG_ERR_ENABLED        (MIN(LOG_LEVEL, curr_log_level) >= LOG_LEVEL_ERR)
#define LOG_WARN_ENABLED       (MIN(LOG_LEVEL, curr_log_level) >= LOG_LEVEL_WARN)
#define LOG_INFO_ENABLED       (MIN(LOG_LEVEL, curr_log_level) >= LOG_LEVEL_INFO)
#define LOG_DBG_ENABLED        (MIN(LOG_LEVEL, curr_log_level) >= LOG_LEVEL_DBG)

#if NETSTACK_CONF_WITH_IPV6

/**
 * Logs an IPv6 address
 * \param ipaddr The IPv6 address
*/
void log_6addr(const uip_ipaddr_t *ipaddr);

/**
 * Logs an IPv6 address with a compact format
 * \param ipaddr The IPv6 address
*/
void log_6addr_compact(const uip_ipaddr_t *ipaddr);

#endif /* NETSTACK_CONF_WITH_IPV6 */

/**
 * Logs a link-layer address
 * \param lladdr The link-layer address
*/
void log_lladdr(const linkaddr_t *lladdr);

/**
 * Logs a link-layer address with a compact format
 * \param lladdr The link-layer address
*/
void log_lladdr_compact(const linkaddr_t *lladdr);

/**
 * Sets a log level at run-time. Logs are included in the firmware via
 * the compile-time flags in log-conf.h, but this allows to force lower log
 * levels, system-wide.
 * \param module The target module string descriptor
 * \param level The log level
*/
void log_set_level(const char *module, int level);

/**
 * Returns the current log level.
 * \param module The target module string descriptor
 * \return The current log level
*/
int log_get_level(const char *module);

/**
 * Returns a textual description of a log level
 * \param level log level
 * \return The textual description
*/
const char *log_level_to_str(int level);

#endif /* __LOG_H__ */

/** @} */
/** @} */
