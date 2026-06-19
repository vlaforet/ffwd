#ifndef FFWD_H
#define FFWD_H

#include <stdint.h>

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

void ffwd_init(int num_of_servers);
void ffwd_init_thread();
void ffwd_shutdown();
void ffwd_lock(int server_id);
void ffwd_unlock(int server_id);

#endif