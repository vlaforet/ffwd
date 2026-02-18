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
      uint32_t argc;
      ffwd_func_t fptr;
      uint64_t argv[5];
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
struct server_args
{
  volatile int stop;
  int server_core;
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
    };
    uint8_t padding[CACHE_LINE_SIZE];
  };
} __attribute__((aligned(CACHE_LINE_SIZE)));

#endif