#ifdef FFWD_NO_PINNING
#define _GNU_SOURCE
#include <sched.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <numa.h>
#include "ffwd_internals.h"

#define FFWD_SHADOW_STACK_SIZE 8192

_Atomic uint64_t thread_counter = 0; // Atomic counter to assign unique thread IDs
__thread struct ffwd_context *thread_context = NULL;
__thread struct ffwd_server_context *server_context = NULL; // Pointer to the server context for server threads
__thread volatile char shadow_stack[FFWD_SHADOW_STACK_SIZE] __attribute__((aligned(CACHE_LINE_SIZE)));
__thread void *shadow_stack_ptr = NULL; // Top of this thread's shadow stack

_Atomic uint64_t server_counter = 0; // Atomic counter to assign unique server IDs
pthread_t server_threads[MAX_NUMBER_SERVERS];
struct ffwd_server_context *ffwd_server_context[MAX_NUMBER_SERVERS];

/* Real hardware cache line, as opposed to the padded CACHE_LINE_SIZE used to
   avoid false sharing. */
#define FFWD_LINE 64

/* __builtin_prefetch(p, 1, ...) only becomes PREFETCHW when the compiler is
   told the target has it (-mprfchw); otherwise gcc quietly emits PREFETCHT0,
   which fetches the line Shared and leaves the following store to pay for the
   read-for-ownership anyway. Emit the instruction directly instead. */
static inline void prefetch_w(const void *p)
{
#if defined(__x86_64__)
  __asm__ __volatile__("prefetchw %0" ::"m"(*(const char *)p));
#else
  __builtin_prefetch(p, 1, 3);
#endif
}

/* The switch pops 48 bytes from req->rsp and returns through rsp+48; the
 * critical section then grows the stack downwards from there. Those lines were
 * last modified by the client's core, so pulling them over costs a remote HITM
 * transfer -- worth starting as early as possible. */
static inline void prefetch_client_stack(struct ffwd_request *req)
{
  char *sp = (char *)req->rsp;
  prefetch_w(sp);                           /* saved registers, rewritten on the way back */
  __builtin_prefetch(sp + FFWD_LINE, 0, 3); /* resume address / caller frame */
  prefetch_w(sp - FFWD_LINE);               /* frames pushed by the critical section */
}

/* Publish a group's staged releases: one line write hands up to
 * FFWD_CLIENTS_PER_GROUP clients their resume stack pointer and wakes them.
 *
 * Only the slots actually staged are copied. Rewriting the whole group would
 * be harmless (an unstaged slot still holds the value its client already
 * consumed) but costs 15 stores to release what is often a single client,
 * which is what makes grouping lose to a plain per-client release at low
 * thread counts.
 *
 * "flags" is stored last and with release ordering, because a woken client
 * reads its resume_rsp slot immediately after seeing its bit toggle. */
static inline void publish_dirty(struct ffwd_server_context *ctx)
{
  int gidx = ctx->dirty_gidx;
  if (gidx < 0)
    return;

  struct ffwd_group_response *src = &ctx->staging[gidx];
  struct ffwd_group_response *dst = &ctx->responses[gidx];

  for (uint64_t m = ctx->dirty_mask; m; m &= m - 1)
  {
    int s = __builtin_ctzll(m);
    dst->resume_rsp[s] = src->resume_rsp[s];
  }

  __atomic_store_n((uint64_t *)&dst->flags, src->flags, __ATOMIC_RELEASE);

  ctx->dirty_gidx = -1;
  ctx->dirty_n = 0;
  ctx->dirty_mask = 0;
}

/* Sweep every registered client once and collect the pending requests into
 * ctx->batch.
 *
 * Probing costs a single remote load (req->seq) compared against a
 * server-private snapshot, and the probes of distinct clients are independent,
 * so the out-of-order engine overlaps their misses. Serving one request per
 * sweep would instead serialise a full remote miss chain per handoff. */
static void ffwd_sweep(struct ffwd_server_context *ctx)
{
  int n = (int)atomic_load_explicit(&thread_counter, memory_order_relaxed);
  if (n > MAX_THREADS)
    n = MAX_THREADS;
  if (__builtin_expect(n == 0, 0))
  {
    ctx->batch_n = 0;
    ctx->batch_i = 0;
    return;
  }

  int node = ctx->current_numa_node;
  int tid = ctx->current_thread_id;
  int found = 0;

  /* Resume where the previous sweep stopped so that a batch-size overflow
   * cannot starve the tail of the array. */
  for (int i = NUM_NUMA_NODES * n; i > 0 && found < FFWD_BATCH_MAX; i--)
  {
    struct ffwd_request *req = ctx->requests[node][tid];
    if (req)
    {
      uint64_t seq = req->seq;
      if (seq != ctx->prev_seq[node][tid])
      {
        /* Taking the request is recorded privately; the client's line is never
         * written, so it stays exclusively hers between requests. */
        ctx->prev_seq[node][tid] = seq;

        struct ffwd_batch_entry *e = &ctx->batch[found];
        e->req = req;
        e->gidx = node * FFWD_NUM_GROUPS + tid / FFWD_CLIENTS_PER_GROUP;
        e->slot = tid % FFWD_CLIENTS_PER_GROUP;

        if (found < FFWD_PREFETCH_DEPTH)
          prefetch_client_stack(req);
        found++;
      }
    }

    if (++tid >= n)
    {
      tid = 0;
      if (++node >= NUM_NUMA_NODES)
        node = 0;
    }
  }

  ctx->current_numa_node = node;
  ctx->current_thread_id = tid;
  ctx->batch_n = found;
  ctx->batch_i = 0;
}

#ifndef FFWD_NO_PINNING
void move_to_core(int core_id)
{
  int num_cpu = numa_num_configured_cpus();
  struct bitmask *cpumask = numa_bitmask_alloc(num_cpu);
  numa_bitmask_setbit(cpumask, core_id);
  numa_sched_setaffinity(0, cpumask);
  numa_bitmask_free(cpumask);
}
#endif

void ffwd_lock(int server_id)
{
  // assert(server_id < server_counter);
  // assert(thread_context != NULL);

  struct ffwd_context *c = thread_context;
  ffwd_send_request(&c->requests[server_id], shadow_stack_ptr,
                    c->resp_flags[server_id], c->mask, c->resume_slot[server_id]);
}

void ffwd_unlock(int server_id)
{
  // assert(server_context != NULL);
  (void)server_id;

  struct ffwd_server_context *ctx = server_context;
  int i = ctx->batch_i;
  struct ffwd_batch_entry *curr = &ctx->batch[i - 1];

  /* Releases staged before this handoff are complete: the switch that brought
   * us here is what filled in the previous entry's resume pointer. Publishing
   * here rather than at the switch keeps the client's wake-up off the server's
   * critical path. */
  if (ctx->dirty_gidx >= 0 &&
      (ctx->dirty_gidx != curr->gidx || ctx->dirty_n >= FFWD_RELEASE_BATCH))
    publish_dirty(ctx);

  /* Stage this client's release. Both stores are server-local; the switch
   * below fills in the resume pointer, and nothing is visible to the client
   * until the group is published. */
  struct ffwd_group_response *stage = &ctx->staging[curr->gidx];
  stage->flags ^= 1ull << curr->slot;
  ctx->dirty_gidx = curr->gidx;
  ctx->dirty_mask |= 1ull << curr->slot;
  ctx->dirty_n++;

  void **out = &stage->resume_rsp[curr->slot];

  if (i < ctx->batch_n)
  {
    struct ffwd_batch_entry *next = &ctx->batch[i];
    ctx->batch_i = i + 1;

    /* Keep the prefetch pipeline FFWD_PREFETCH_DEPTH handoffs deep, so the
     * stack we jump onto has already been pulled into the server's cache. */
    if (i + FFWD_PREFETCH_DEPTH < ctx->batch_n)
      prefetch_client_stack(ctx->batch[i + FFWD_PREFETCH_DEPTH].req);

    context_switch((void **)&next->req->rsp, out);
  }
  else
  {
    /* Batch drained. Publish the staged releases whose resume pointers are
     * already known, and let the switch itself publish this last one, so its
     * client does not have to wait for the server to get back onto its own
     * stack before being woken. */
    struct ffwd_group_response *dst = &ctx->responses[curr->gidx];

    for (uint64_t m = ctx->dirty_mask & ~(1ull << curr->slot); m; m &= m - 1)
    {
      int s = __builtin_ctzll(m);
      dst->resume_rsp[s] = stage->resume_rsp[s];
    }

    ctx->dirty_gidx = -1;
    ctx->dirty_n = 0;
    ctx->dirty_mask = 0;

    context_switch_release(&ctx->server_rsp, &dst->resume_rsp[curr->slot],
                           &dst->flags, stage->flags);
  }

  /* Reached on the client thread, once the server publishes this release. */
}

void *server_func(void *input)
{
  struct ffwd_server_context *ctx = (struct ffwd_server_context *)input;
  server_context = ctx;

#ifdef FFWD_SERVERS_FIFO
  if (sched_setscheduler(0, SCHED_FIFO, &(struct sched_param){.sched_priority = 1}) == -1)
  {
    perror("sched_setscheduler SCHED_FIFO for FFWD server");
    exit(EXIT_FAILURE);
  }
#endif

#ifndef FFWD_NO_PINNING
  move_to_core(ctx->server_core);
#endif

  ctx->current_thread_id = 0;
  ctx->current_numa_node = 0;
  ctx->dirty_gidx = -1;

  while (!ctx->stop)
  {
    ffwd_sweep(ctx);

    if (ctx->batch_n)
    {
      ctx->batch_i = 1;
      if (FFWD_PREFETCH_DEPTH < ctx->batch_n)
        prefetch_client_stack(ctx->batch[FFWD_PREFETCH_DEPTH].req);

      context_switch((void **)&ctx->batch[0].req->rsp, &ctx->server_rsp);

      /* The batch has drained, so the last entry's resume pointer is in place;
       * nothing may stay staged past this point or its client would hang. */
      publish_dirty(ctx);
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
    ffwd_server_context[id]->dirty_gidx = -1;

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
  thread_context->slot = id % FFWD_CLIENTS_PER_GROUP;
  thread_context->mask = 1ull << thread_context->slot;
  shadow_stack_ptr = (void *)shadow_stack + (FFWD_SHADOW_STACK_SIZE - CACHE_LINE_SIZE);

  memset(thread_context->requests, 0, sizeof(thread_context->requests));

  int gidx = numa_node * FFWD_NUM_GROUPS + id / FFWD_CLIENTS_PER_GROUP;
  for (int i = 0; i < server_counter; i++)
  {
    struct ffwd_group_response *resp = &ffwd_server_context[i]->responses[gidx];
    thread_context->resp_flags[i] = &resp->flags;
    thread_context->resume_slot[i] = &resp->resume_rsp[thread_context->slot];
  }

  /* Only publish the slots once they are fully initialised. */
  atomic_thread_fence(memory_order_release);

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
