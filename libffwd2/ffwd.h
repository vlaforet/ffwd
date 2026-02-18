#ifndef FFWD_H
#define FFWD_H

#if defined(__has_include)
#if __has_include("platform_defs.h")
#include "platform_defs.h"
#else
#error "libffwd platforms_defs.h not found! Please run make in libffwd to generate it."
#endif
#else
#include "platform_defs.h" // Include anyway if __has_include is not supported
#endif

#ifndef MAX_NUMBER_SERVERS
#define MAX_NUMBER_SERVERS 4
#endif

#ifndef MAX_THREADS
#define MAX_THREADS 128
#endif

typedef uint64_t (*ffwd_func_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

void ffwd_init(int num_of_servers);
void ffwd_init_thread();
void ffwd_shutdown();
uint64_t ffwd_exec(int server_id, ffwd_func_t function, uint64_t arg);

#endif