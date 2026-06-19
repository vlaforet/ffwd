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
      uint64_t flag;
      void **rsp;
    };
    uint8_t padding[CACHE_LINE_SIZE];
  };
};

#define NUM_THREADS_PER_GROUP 15

// Contains responses for a thread group (NUM_THREADS_PER_GROUP threads)
struct ffwd_thread_group_response
{
  uint64_t return_values[NUM_THREADS_PER_GROUP]; // 120 bytes
  uint64_t flags;                                // 8 bytes
} __attribute__((aligned(CACHE_LINE_SIZE)));

#define NUM_THREAD_GROUPS ((MAX_THREADS + NUM_THREADS_PER_GROUP - 1) / NUM_THREADS_PER_GROUP)
struct ffwd_server_context
{
  volatile int stop;
  int server_core;
  void *server_rsp;
  struct ffwd_request *current_req;

  struct ffwd_request *requests[NUM_NUMA_NODES][MAX_THREADS];
  struct ffwd_thread_group_response responses[NUM_NUMA_NODES][NUM_THREAD_GROUPS];
};

struct ffwd_context
{
  union
  {
    struct
    {
      int id;
      uint64_t mask;
      struct ffwd_request requests[MAX_NUMBER_SERVERS];
      struct ffwd_thread_group_response *responses[MAX_NUMBER_SERVERS];

      void *rsp;
      void *volatile local_shadow_stack_ptr;
    };
    uint8_t padding[CACHE_LINE_SIZE];
  };
} __attribute__((aligned(CACHE_LINE_SIZE)));

extern void call_on_stack(
    void **old_sp_slot,
    void **new_sp_slot,
    void (*fn)(int),
    int arg);

extern void context_switch(void **incoming_rsp_ptr,
                           void **outgoing_rsp_ptr);

#endif