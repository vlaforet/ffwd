#ifndef FWD_INTERNALS_H
#define FWD_INTERNALS_H

#include <stdlib.h>
#include <stdint.h>
#include "ffwd.h"

/* Clients per grouped response line, and hence how many can be released by a
   single line write.
   7 resume pointers plus the flags word is exactly 64 bytes, so a publish
   dirties one hardware cache line. Larger groups amortise the release over
   more clients but make more of them poll (and so fight over) the same line;
   measured on 2x16-core Ice Lake, 3 is better below 16 threads and 15 is a few
   percent better above 47, with 7 the best compromise across the range. */
#ifndef FFWD_CLIENTS_PER_GROUP
#define FFWD_CLIENTS_PER_GROUP 7
#endif

#define FFWD_NUM_GROUPS ((MAX_THREADS + FFWD_CLIENTS_PER_GROUP - 1) / FFWD_CLIENTS_PER_GROUP)
#define FFWD_TOTAL_GROUPS (NUM_NUMA_NODES * FFWD_NUM_GROUPS)

/* How many pending requests one sweep may collect, and how far ahead of the
   request being served client stacks are prefetched. */
#ifndef FFWD_BATCH_MAX
#define FFWD_BATCH_MAX 64
#endif
#ifndef FFWD_PREFETCH_DEPTH
#define FFWD_PREFETCH_DEPTH 4
#endif

/* Staged releases are published once this many have accumulated in a group (or
   earlier, when the batch moves to another group or drains). Larger values
   amortise the coherence traffic over more clients but keep those clients
   blocked longer, which drains the request pipeline. */
#ifndef FFWD_RELEASE_BATCH
#define FFWD_RELEASE_BATCH FFWD_CLIENTS_PER_GROUP
#endif

/* Client -> server. Written only by the owning client, read only by the
 * server: unlike the previous design the server never writes here, so the
 * client's spin loop no longer fights the server for ownership of this line.
 *
 * "rsp" and "seq" share one hardware cache line, so the single remote transfer
 * the sweep pays for already carries the stack pointer the switch needs.
 *
 * Offsets are mirrored in ffwd_asm.S -- keep the two in sync. */
struct ffwd_request
{
  union
  {
    struct
    {
      void *volatile rsp;    /* 0: stack pointer the client parked on */
      volatile uint64_t seq; /* 8: toggled by the client to post a request */
    };
    uint8_t padding[CACHE_LINE_SIZE];
  };
} __attribute__((aligned(CACHE_LINE_SIZE)));

/* Server -> clients. One line shared by FFWD_CLIENTS_PER_GROUP clients, so a
 * single line write releases up to 15 of them and hands each its resume stack
 * pointer. Allocated on the server's NUMA node: the server writes it on the
 * critical path, the clients only read it once woken.
 *
 * "flags" must be stored last -- a client that sees its bit toggle immediately
 * reads its resume_rsp slot, and x86-TSO only orders the two if the pointer is
 * written first.
 *
 * Offsets are mirrored in ffwd_asm.S -- keep the two in sync. */
struct ffwd_group_response
{
  void *resume_rsp[FFWD_CLIENTS_PER_GROUP]; /* 0:   where each client resumes */
  volatile uint64_t flags;                  /* 120: bit s toggles to release client s */
} __attribute__((aligned(CACHE_LINE_SIZE)));

struct ffwd_batch_entry
{
  struct ffwd_request *req;
  int gidx; /* flattened node * FFWD_NUM_GROUPS + group */
  int slot; /* index within the group */
};

struct ffwd_server_context
{
  /* Hot, server-private. */
  int batch_i;           /* next entry to serve */
  int batch_n;           /* entries collected by the last sweep */
  int dirty_gidx;        /* group holding staged, unpublished releases (-1: none) */
  int dirty_n;           /* how many releases are staged in it */
  uint64_t dirty_mask;   /* which slots of that group are staged */
  int current_numa_node; /* sweep resume cursor */
  int current_thread_id;
  void *server_rsp; /* the server loop's own saved stack pointer */

  struct ffwd_batch_entry batch[FFWD_BATCH_MAX];

  /* Server-local staging; copied into "responses" to publish. */
  struct ffwd_group_response staging[FFWD_TOTAL_GROUPS];

  /* Last request sequence the server has already taken from each client. */
  uint64_t prev_seq[NUM_NUMA_NODES][MAX_THREADS];

  struct ffwd_request *requests[NUM_NUMA_NODES][MAX_THREADS];

  /* Shared with the clients. */
  struct ffwd_group_response responses[FFWD_TOTAL_GROUPS];

  /* Cold: written by ffwd_shutdown, kept away from the hot fields. */
  int server_core;
  volatile int stop;
};

struct ffwd_context
{
  union
  {
    struct
    {
      int id;
      int slot;      /* id % FFWD_CLIENTS_PER_GROUP */
      uint64_t mask; /* 1 << slot */
      
      volatile uint64_t *resp_flags[MAX_NUMBER_SERVERS];
      void **resume_slot[MAX_NUMBER_SERVERS];
      struct ffwd_request requests[MAX_NUMBER_SERVERS];
    };
    uint8_t padding[CACHE_LINE_SIZE];
  };
} __attribute__((aligned(CACHE_LINE_SIZE)));

/* Park the calling client on "shadow_sp" and hand "req" to the server, waiting
   until bit "mask" of *resp_flags toggles; resume from *resume_slot. */
extern void ffwd_send_request(struct ffwd_request *req, void *shadow_sp,
                              volatile uint64_t *resp_flags, uint64_t mask,
                              void **resume_slot);

/* Save the current stack pointer into *out_rsp_slot and resume *in_rsp_slot. */
extern void context_switch(void **in_rsp_slot, void **out_rsp_slot);

/* As context_switch, but also stores flags_value into *flags afterwards, which
   releases the client whose resume pointer just went into *out_rsp_slot. */
extern void context_switch_release(void **in_rsp_slot, void **out_rsp_slot,
                                   volatile uint64_t *flags, uint64_t flags_value);

#endif