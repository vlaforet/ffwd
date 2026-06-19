#ifdef FFWD_NO_PINNING
#define _GNU_SOURCE
#include <sched.h>
#endif

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <numa.h>
#include "ffwd_internals.h"

_Atomic uint64_t thread_counter = 0; // Atomic counter to assign unique thread IDs
__thread struct ffwd_context *thread_context = NULL;
__thread struct ffwd_server_context *server_context = NULL; // Pointer to the server context for server threads
__thread volatile char shadow_stack_ptr[8192];

_Atomic uint64_t server_counter = 0; // Atomic counter to assign unique server IDs
pthread_t server_threads[MAX_NUMBER_SERVERS];
struct ffwd_server_context *ffwd_server_context[MAX_NUMBER_SERVERS];

#ifndef FFWD_NO_PINNING
void move_to_core(int core_id)
{
  int num_cpu = numa_num_configured_cpus();
  struct bitmask *cpumask = numa_bitmask_alloc(num_cpu);
  numa_bitmask_setbit(cpumask, core_id);
  numa_sched_setaffinity(0, cpumask);
}
#endif

__attribute__((noipa, noinline)) static void make_request(int server_id)
{
  // assert(!server_context);

  struct ffwd_request *req = &thread_context->requests[server_id];
  req->rsp = &thread_context->rsp;

  uint64_t flag = req->flag ^ thread_context->mask; // Get the previous flag value
  req->flag ^= thread_context->mask;                // Toggle the flag to signal the server

  while ((thread_context->responses[server_id]->flags ^ flag) & thread_context->mask)
  {
    __asm__ __volatile__("rep;nop" : : : "memory");
  }
}

void ffwd_lock(int server_id)
{
  assert(server_id < server_counter);
  assert(thread_context != NULL);

  call_on_stack(&thread_context->rsp, (void *)&thread_context->local_shadow_stack_ptr, make_request, server_id);
}

void ffwd_unlock(int server_id)
{
  if (server_context != NULL)
  {
    context_switch(&server_context->server_rsp, server_context->current_req->rsp);
  }
}


void *server_func(void *input)
{
  server_context = (struct ffwd_server_context *)input;

#ifdef FFWD_SERVERS_FIFO
  if (sched_setscheduler(0, SCHED_FIFO, &(struct sched_param){.sched_priority = 1}) == -1)
  {
    perror("sched_setscheduler SCHED_FIFO for FFWD server");
    exit(EXIT_FAILURE);
  }
#endif

#ifndef FFWD_NO_PINNING
  move_to_core(server_context->server_core);
#endif

  uint64_t group_prev_flags[NUM_THREAD_GROUPS] = {0};
  struct ffwd_thread_group_response group_response[NUM_THREAD_GROUPS] = {0};

  int g, t, i, numa_node;
  while (!server_context->stop)
  {
    for (numa_node = 0; numa_node < NUM_NUMA_NODES; numa_node++)
    {
      for (g = 0; g < (thread_counter + NUM_THREADS_PER_GROUP - 1) / NUM_THREADS_PER_GROUP; g++)
      {
        for (t = 0; t < NUM_THREADS_PER_GROUP; t++)
        {
          i = g * NUM_THREADS_PER_GROUP + t;

          server_context->current_req = server_context->requests[numa_node][i];
          if (server_context->current_req && (server_context->current_req->flag ^ group_prev_flags[g]) & 1ull << t)
          {
            context_switch(server_context->current_req->rsp, &server_context->server_rsp);
            group_response[g].flags ^= 1ull << t;
          }
        }

        if (group_response[g].flags ^ group_prev_flags[g])
        {
          server_context->responses[numa_node][g] = group_response[g];
          group_prev_flags[g] = group_response[g].flags;
        }
      }
    }
  }

  return NULL;
}

// Global initialization function
void ffwd_init(int num_of_servers)
{
  int id;
  for (int s = 0; s < num_of_servers; s++)
  {
    id = atomic_fetch_add(&server_counter, 1);

    ffwd_server_context[id] = (struct ffwd_server_context *)numa_alloc_onnode(sizeof(struct ffwd_server_context), numa_node_of_cpu(id));
    memset(ffwd_server_context[id], 0, sizeof(struct ffwd_server_context));
    ffwd_server_context[id]->server_core = id; // For simplicity, assign core = server ID

    pthread_create(&server_threads[id], NULL, server_func, (void *)(ffwd_server_context[id]));
  }

  // ffwd_init_thread();
}

// Per-thread initialization function (init thread_context)
void ffwd_init_thread()
{
  if (thread_context != NULL)
    return; // Already initialized

  int id = atomic_fetch_add(&thread_counter, 1);
  int numa_node;
#ifndef FFWD_NO_PINNING
  int cpu_id = server_counter + (id % (numa_num_configured_cpus() - server_counter));
  numa_node = numa_node_of_cpu(cpu_id);
  move_to_core(cpu_id);
#else
  numa_node = numa_node_of_cpu(sched_getcpu());
#endif

  thread_context = (struct ffwd_context *)numa_alloc_onnode(sizeof(struct ffwd_context), numa_node);
  thread_context->id = id;
  thread_context->mask = ((uint64_t)1 << (id % NUM_THREADS_PER_GROUP));
  thread_context->local_shadow_stack_ptr = (void *)shadow_stack_ptr + (8192 - 64);

  memset(thread_context->requests, 0, sizeof(thread_context->requests));

  for (int i = 0; i < server_counter; i++)
  {
    thread_context->responses[i] = &ffwd_server_context[i]->responses[numa_node][id / NUM_THREADS_PER_GROUP];
    ffwd_server_context[i]->requests[numa_node][id] = &thread_context->requests[i];
  }
}

void ffwd_shutdown()
{
  // Shutdown all servers
}