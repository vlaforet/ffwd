#ifndef FWD_INTERNALS_H
#define FWD_INTERNALS_H

#include <stdlib.h>
#include <stdint.h>
#include "ffwd.h"

struct ffwd_request
{
  union
  {
    struct
    {
      void *rsp;
    };
    uint8_t padding[CACHE_LINE_SIZE];
  };
};

struct ffwd_server_context
{
  volatile int stop;
  int server_core;
  void *server_rsp;

  int current_numa_node;
  int current_thread_id;

  struct ffwd_request *requests[NUM_NUMA_NODES][MAX_THREADS];
};

struct ffwd_context
{
  union
  {
    struct
    {
      int id;
      struct ffwd_request requests[MAX_NUMBER_SERVERS];

      void *volatile local_shadow_stack_ptr;
    };
    uint8_t padding[CACHE_LINE_SIZE];
  };
} __attribute__((aligned(CACHE_LINE_SIZE)));

extern void ffwd_send_request(void **old_sp_slot, void **new_sp_slot);
extern void context_switch(void **incoming_rsp_ptr, void **outgoing_rsp_ptr);

#endif