#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <numa.h>
#include "ffwd_internals.h"

_Atomic uint64_t thread_counter = 0; // Atomic counter to assign unique thread IDs
__thread struct ffwd_context *thread_context = NULL;

_Atomic uint64_t server_counter = 0; // Atomic counter to assign unique server IDs
pthread_t server_threads[MAX_NUMBER_SERVERS];
struct server_args *server_args[MAX_NUMBER_SERVERS];

void move_to_core(int core_id)
{
  int num_cpu = numa_num_configured_cpus();
  struct bitmask *cpumask = numa_bitmask_alloc(num_cpu);
  numa_bitmask_setbit(cpumask, core_id);
  numa_sched_setaffinity(0, cpumask);
}

uint64_t inline ffwd_exec(int server_id, ffwd_func_t function, uint64_t arg)
{
  // assert(server_id < server_counter);
  // assert(thread_context != NULL);

  struct ffwd_request *req = &thread_context->requests[server_id];
  req->fptr = function;
  req->argc = 1;
  req->argv[0] = arg;

  uint64_t flag = req->flag ^ thread_context->mask; // Get the previous flag value
  req->flag ^= thread_context->mask;                // Toggle the flag to signal the server

  while ((thread_context->responses[server_id]->flags ^ flag) & thread_context->mask)
  {
    __asm__ __volatile__("rep;nop" : : : "memory");
  }

  return thread_context->responses[server_id]->return_values[thread_context->id % NUM_THREADS_PER_GROUP];
}

#define FFWD_EXECUTE_SWITCH
static inline uint64_t execute_req(struct ffwd_request *req)
{
#if defined(FFWD_EXECUTE_SWITCH_ASM) // Clever trick to avoid the switch overhead.
  uint64_t ret;

  __asm__ __volatile__(
      "mov    %c[argc](%[req]), %%ecx \n\t" // ecx = argc
      "cmp    $4,               %%ecx \n\t" // If argc > 4,
      "ja     5f                      \n\t" //    jump to 5 args handling
      "lea    6f(%%rip),        %%r9  \n\t" // r9 = &6 (jump table)
      "movslq (%%r9,%%rcx,4),   %%r11 \n\t" // r11 = *(&6 + argc*4) (jump offset for given argc)
      "add    %%r9,             %%r11 \n\t" // r11 += r9 -> r11 = &6 + *(&6 + argc*4)
      "jmp    *%%r11                  \n\t" // Jump to r11 which points to target for given argc

      // Jump table for argc = 0..4 (>=5 is handled by cmp/ja 5f above)
      "6: .long 0f-6b, 1f-6b, 2f-6b, 3f-6b, 4f-6b \n\t"

      "5:  mov  %c[argv]+32(%[req]), %%r8  \n\t" // a4 -> r8
      "4:  mov  %c[argv]+24(%[req]), %%rcx \n\t" // a3 -> rcx
      "3:  mov  %c[argv]+16(%[req]), %%rdx \n\t" // a2 -> rdx
      "2:  mov  %c[argv]+8(%[req]),  %%rsi \n\t" // a1 -> rsi
      "1:  mov  %c[argv]+0(%[req]),  %%rdi \n\t" // a0 -> rdi
      "0:  call *%c[fptr](%[req])          \n\t"
      : "=a"(ret)
      : [req] "r"(req),
        [fptr] "i"(offsetof(struct ffwd_request, fptr)),
        [argv] "i"(offsetof(struct ffwd_request, argv)),
        [argc] "i"(offsetof(struct ffwd_request, argc))
      : "rdi", "rsi", "rdx", "rcx", "r8", "r9", "r10", "r11", "cc", "memory");

  return ret;
#elif defined(FFWD_EXECUTE_ASM) // This is almost exactly fptr(arg0, arg1, arg2, arg3, arg4).
  register uint64_t ret asm("rax");
  register uint64_t a4 asm("r8") = req->argv[4];

  __asm__ __volatile__(
      "call *%[fn]\n\t"
      : [ret] "=r"(ret)
      : [fn] "rm"(req->fptr),
        [a0] "D"(req->argv[0]),
        [a1] "S"(req->argv[1]),
        [a2] "d"(req->argv[2]),
        [a3] "c"(req->argv[3]),
        [a4] "r"(a4) // Can't use "r8" directly in the constraints, so we load it from a register variable.
      : "r9", "r10", "r11", "cc", "memory");
  return ret;
#else
#if defined(FFWD_EXECUTE_SWITCH) // This is the most portable version for 0-4 arguments.
  switch (__builtin_expect(req->argc, 1)) // Optimize for a single argument (common case).
  {
  case 0:
    return ((uint64_t (*)())req->fptr)();
  case 1:
    return ((uint64_t (*)(uint64_t))req->fptr)(req->argv[0]);
  case 2:
    return ((uint64_t (*)(uint64_t, uint64_t))req->fptr)(req->argv[0], req->argv[1]);
  case 3:
    return ((uint64_t (*)(uint64_t, uint64_t, uint64_t))req->fptr)(req->argv[0], req->argv[1], req->argv[2]);
  case 4:
    return ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t))req->fptr)(req->argv[0], req->argv[1], req->argv[2], req->argv[3]);
  }
#endif
  // This should always work. Performs worse with less than 5 arguments (5x args overhead).
  // But may be faster for 5 arguments since it avoids the switch.
  return req->fptr(req->argv[0], req->argv[1], req->argv[2], req->argv[3], req->argv[4]);
#endif
}

void *server_func(void *input)
{
  struct server_args *me = (struct server_args *)input;
  move_to_core(me->server_core);

  uint64_t group_prev_flags[NUM_THREAD_GROUPS] = {0};
  struct ffwd_thread_group_response group_response[NUM_THREAD_GROUPS] = {0};

  struct ffwd_request *current_req;

  int g, t, i, numa_node;
  while (!me->stop)
  {
    for (numa_node = 0; numa_node < NUM_NUMA_NODES; numa_node++)
    {
      for (g = 0; g < (thread_counter + NUM_THREADS_PER_GROUP - 1) / NUM_THREADS_PER_GROUP; g++)
      {
        for (t = 0; t < NUM_THREADS_PER_GROUP; t++)
        {
          i = g * NUM_THREADS_PER_GROUP + t;

          current_req = me->requests[numa_node][i];
          if (current_req && (current_req->flag ^ group_prev_flags[g]) & 1ull << t)
          {
            group_response[g].return_values[t] = execute_req(current_req);
            group_response[g].flags ^= 1ull << t;
          }
        }

        if (group_response[g].flags ^ group_prev_flags[g])
        {
          me->responses[numa_node][g] = group_response[g];
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

    server_args[id] = (struct server_args *)numa_alloc_onnode(sizeof(struct server_args), numa_node_of_cpu(id));
    memset(server_args[id], 0, sizeof(struct server_args));
    server_args[id]->server_core = id; // For simplicity, assign core = server ID

    pthread_create(&server_threads[id], NULL, server_func, (void *)(server_args[id]));
  }

  // ffwd_init_thread();
}

// Per-thread initialization function (init thread_context)
void ffwd_init_thread()
{
  if (thread_context != NULL)
    return; // Already initialized

  int id = atomic_fetch_add(&thread_counter, 1);
  int cpu_id = server_counter + id;
  int numa_node = numa_node_of_cpu(cpu_id);
  move_to_core(cpu_id);

  thread_context = (struct ffwd_context *)numa_alloc_onnode(sizeof(struct ffwd_context), numa_node);
  thread_context->id = id;
  thread_context->mask = ((uint64_t)1 << (id % NUM_THREADS_PER_GROUP));
  memset(thread_context->requests, 0, sizeof(thread_context->requests));

  for (int i = 0; i < server_counter; i++)
  {
    thread_context->responses[i] = &server_args[i]->responses[numa_node][id / NUM_THREADS_PER_GROUP];
    server_args[i]->requests[numa_node][id] = &thread_context->requests[i];
  }
}

void ffwd_shutdown()
{
  // Shutdown all servers
}