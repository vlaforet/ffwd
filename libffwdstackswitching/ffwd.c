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

static inline int request_is_pending(struct ffwd_request *req)
{
  return req && req->rsp && *(uint64_t *)req->rsp == 1247079108; // Check if the request flag is set to the magic value (request)
}

static inline void advance_server_cursor(struct ffwd_server_context *ctx)
{
  ctx->current_thread_id++;
  if (ctx->current_thread_id == MAX_THREADS)
  {
    ctx->current_thread_id = 0;
    ctx->current_numa_node++;
    if (ctx->current_numa_node == NUM_NUMA_NODES)
      ctx->current_numa_node = 0;
  }
}

#ifndef FFWD_NO_PINNING
void move_to_core(int core_id)
{
  int num_cpu = numa_num_configured_cpus();
  struct bitmask *cpumask = numa_bitmask_alloc(num_cpu);
  numa_bitmask_setbit(cpumask, core_id);
  numa_sched_setaffinity(0, cpumask);
}
#endif

void ffwd_lock(int server_id)
{
  // assert(server_id < server_counter);
  // assert(thread_context != NULL);

  struct ffwd_request *req = &thread_context->requests[server_id];
  ffwd_send_request(&req->rsp, (void **)&thread_context->local_shadow_stack_ptr);
}

void ffwd_unlock(int server_id)
{
  // assert(server_context != NULL);

  struct ffwd_request *curr_req = server_context->requests[server_context->current_numa_node][server_context->current_thread_id];
  struct ffwd_request *next_req = NULL;

  for (int i = 0; i < NUM_NUMA_NODES * MAX_THREADS - 1; i++)
  {
    advance_server_cursor(server_context);

    next_req = server_context->requests[server_context->current_numa_node][server_context->current_thread_id];
    if (request_is_pending(next_req))
    {
      context_switch(&next_req->rsp, &curr_req->rsp);
      goto reset;
    }
  }

  // Go back to server context to wait for new requests
  context_switch(&server_context->server_rsp, &curr_req->rsp);

reset:
  thread_context->requests[server_id].rsp = NULL;
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

  server_context->current_thread_id = 0;
  server_context->current_numa_node = 0;

  struct ffwd_request *curr_req = NULL;
  while (!server_context->stop)
  {
    curr_req = server_context->requests[server_context->current_numa_node][server_context->current_thread_id];
    if (request_is_pending(curr_req))
    {
      // printf("Server context switch\n");
      context_switch(&curr_req->rsp, &server_context->server_rsp);
    }

    advance_server_cursor(server_context);
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
  if (id >= MAX_THREADS)
  {
    fprintf(stderr, "Exceeded maximum number of FFWD threads: %d\n", MAX_THREADS);
    exit(EXIT_FAILURE);
  }

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
  thread_context->local_shadow_stack_ptr = (void *)shadow_stack_ptr + (8192 - 64);

  memset(thread_context->requests, 0, sizeof(thread_context->requests));

  for (int i = 0; i < server_counter; i++)
    ffwd_server_context[i]->requests[numa_node][id] = &thread_context->requests[i];
}

void ffwd_shutdown()
{
  for (int i = 0; i < server_counter; i++)
  {
    ffwd_server_context[i]->stop = 1;
    pthread_join(server_threads[i], NULL);
    numa_free(ffwd_server_context[i], sizeof(struct ffwd_server_context));
  }
}
