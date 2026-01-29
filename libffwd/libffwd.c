#define _GNU_SOURCE
#include <pthread.h>
#include <numa.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/mman.h>
#include "ffwd.h"

pthread_key_t thr_context_key;
volatile int num_of_server_launched;

struct server_set *server_response_set[MAX_NUM_OF_SERVERS];
struct server_args *server_arg[MAX_NUM_OF_SERVERS];
struct request *chips[NUM_SOCKETS][MAX_NUM_OF_SERVERS];

int active_threads_per_socket = 0;
int global_id = 0;
int num_sockets = 0;
int cores_per_socket = 0;
int num_threads = 0;
char *platform;
int **cores;

volatile int finished[32] __attribute__((aligned(128))) = {0};
volatile int finished2[32] __attribute__((aligned(128))) = {0};
volatile int finished3[32] __attribute__((aligned(128))) = {0};
volatile int finished4[32] __attribute__((aligned(128))) = {0};
pthread_t server_thread[MAX_NUM_OF_SERVERS];

void move_to_core(int core_id)
{
	int num_cpu = numa_num_configured_cpus();
	struct bitmask *cpumask = numa_bitmask_alloc(num_cpu);
	numa_bitmask_setbit(cpumask, core_id);
	numa_sched_setaffinity(0, cpumask);
}

inline void prepare_request(struct request *myrequest, int arg_count, ...)
{
	va_list args;
	va_start(args, arg_count);
	int i;

	myrequest->argc = arg_count;
	for (i = 0; i < arg_count; i++)
	{
		myrequest->argv[i] = va_arg(args, uint64_t);
	}

	va_end(args);
}

struct ffwd_context *ffwd_get_context()
{
	return pthread_getspecific(thr_context_key);
}

void *server_func(void *input)
{
	struct server_args *this_server = (struct server_args *)input;

	move_to_core(this_server->server_core);

	uint64_t old_client_flags0 = 0;
	uint64_t old_client_flags1 = 0;

	uint64_t socket_client_flags0 = 0;
	uint64_t socket_client_flags1 = 0;

	uint64_t local_return_values0[(THREADS_PER_RESPONSE) + 1] = {0};
	uint64_t local_return_values1[(THREADS_PER_RESPONSE) + 1] = {0};
	uint64_t local_return_values4[(THREADS_PER_RESPONSE) + 1] = {0};
	uint64_t local_return_values5[(THREADS_PER_RESPONSE) + 1] = {0};

	struct request *current_chip;
	while (finished[0] != 1)
	{
		current_chip = this_server->chips[0];
		EVAL(REPEAT(NCLIENTS, CHIP_IMP15_1, SERVER_CODE, current_chip, 0, 0))
		if (old_client_flags0 ^ socket_client_flags0)
		{
			*(uint64_t *)(&local_return_values0[THREADS_PER_RESPONSE]) = socket_client_flags0;
			memcpy((void *)this_server->server_response->server_responses[0], (void *)local_return_values0, sizeof(local_return_values0));
		}
		old_client_flags0 = socket_client_flags0;

		EVAL(REPEAT(NCLIENTS, CHIP_IMP15_2, SERVER_CODE, current_chip, 0, 1))
		if (old_client_flags0 ^ socket_client_flags0)
		{
			*(uint64_t *)(&local_return_values1[THREADS_PER_RESPONSE]) = socket_client_flags0;
			memcpy((void *)this_server->server_response->server_responses[1], (void *)local_return_values1, sizeof(local_return_values0));
		}
		old_client_flags0 = socket_client_flags0;

		current_chip = this_server->chips[1];
		EVAL(REPEAT(NCLIENTS, CHIP_IMP15_1, SERVER_CODE, current_chip, 1, 4))
		if (old_client_flags1 ^ socket_client_flags1)
		{
			*(uint64_t *)(&local_return_values4[THREADS_PER_RESPONSE]) = socket_client_flags1;
			memcpy((void *)this_server->server_response->server_responses[4], (void *)local_return_values4, sizeof(local_return_values0));
		}
		old_client_flags1 = socket_client_flags1;

		EVAL(REPEAT(NCLIENTS, CHIP_IMP15_2, SERVER_CODE, current_chip, 1, 5))
		if (old_client_flags1 ^ socket_client_flags1)
		{
			*(uint64_t *)(&local_return_values5[THREADS_PER_RESPONSE]) = socket_client_flags1;
			memcpy((void *)this_server->server_response->server_responses[5], (void *)local_return_values5, sizeof(local_return_values0));
		}
		old_client_flags1 = socket_client_flags1;
	}

	return 0;
}

pthread_t *create_thread(void *(*func)(void *))
{
	pthread_t *thread = malloc(sizeof(pthread_t));
	pthread_create(thread, 0, func, &global_id);

	return thread;
}

void *ffwd_client_start(void *param)
{
	struct ffwd_context *context = param;
	move_to_core(context->id);

	pthread_setspecific(thr_context_key, context);
	void *retval = context->initfunc(context->initvalue);

	return retval;
}

void ffwd_thread_create(pthread_t *thread, pthread_attr_t *client_attr, void *(*func)(void *), void *value)
{
	struct ffwd_context *context = malloc(sizeof(struct ffwd_context));
	int id_in_chip = 0;
	int my_chip = 0;
	int i;
	struct request *myrequest[MAX_NUM_OF_SERVERS] = {0};

	// skip the first core in each chip (for server purpose)
	if (global_id == (cores_per_socket - 1) || global_id == (cores_per_socket * 2) - 1 || global_id == (cores_per_socket * 3) - 1 || global_id == (cores_per_socket * 4) - 1 || global_id == (cores_per_socket * 5) - 1 || global_id == (cores_per_socket * 6) - 1 || global_id == (cores_per_socket * 7) - 1)
	{
		global_id++;
	}
	global_id++;

#ifdef STC
	context->id = cores[global_id / cores_per_socket][global_id % cores_per_socket];
#else
	context->id = cores[((global_id / cores_per_socket) * 2) + ((1 - num_sockets * 2) * ((global_id / cores_per_socket) / (num_sockets)))][global_id % cores_per_socket];
#endif

	if (!strcmp(platform, "Xeon"))
	{
		my_chip = context->id / cores_per_socket - (context->id / (cores_per_socket * NUM_SOCKETS)) * NUM_SOCKETS;
		id_in_chip = (context->id % cores_per_socket) * 2 + (context->id / (cores_per_socket * NUM_SOCKETS)) - INDEX_DIFF;
	}
	else if (!strcmp(platform, "Opteron"))
	{
		my_chip = context->id / (cores_per_socket * 2);
		id_in_chip = (context->id % cores_per_socket) * 2 + (context->id / (cores_per_socket * ((my_chip * 2) + 1))) - INDEX_DIFF;
	}
	for (i = 0; i < num_of_server_launched; i++)
	{
		myrequest[i] = &(chips[my_chip][i][id_in_chip]);
	}

	context->initfunc = func;
	context->initvalue = value;
	context->id_in_chip = id_in_chip;
	context->mask = ((uint64_t)1 << id_in_chip);
	context->dead = 0;
	for (i = 0; i < num_of_server_launched; i++)
	{
		context->request[i] = myrequest[i];
		context->server_response[i] = server_response_set[i]->server_responses[(my_chip * NUM_SOCKETS) + ((context->id_in_chip) / NCLIENTS)];
		context->local_client_flag[i] = 0;
	}

	pthread_create(thread, client_attr, ffwd_client_start, context);
}

void ffwd_bind_main_thread()
{
	struct ffwd_context *context = malloc(sizeof(struct ffwd_context));
	int id_in_chip = 0;
	int my_chip = 0;
	int i;
	struct request *myrequest[MAX_NUM_OF_SERVERS] = {0};

	// skip the first core in each chip (for server purpose)
	if (global_id == (cores_per_socket - 1) || global_id == (cores_per_socket * 2) - 1 || global_id == (cores_per_socket * 3) - 1 || global_id == (cores_per_socket * 4) - 1 || global_id == (cores_per_socket * 5) - 1 || global_id == (cores_per_socket * 6) - 1 || global_id == (cores_per_socket * 7) - 1)
	{
		global_id++;
	}
	global_id++;

#ifdef STC
	context->id = cores[global_id / cores_per_socket][global_id % cores_per_socket];
#else
	context->id = cores[((global_id / cores_per_socket) * 2) + ((1 - num_sockets * 2) * ((global_id / cores_per_socket) / (num_sockets)))][global_id % cores_per_socket];
#endif

	if (!strcmp(platform, "Xeon"))
	{
		my_chip = context->id / cores_per_socket - (context->id / (cores_per_socket * NUM_SOCKETS)) * NUM_SOCKETS;
		id_in_chip = (context->id % cores_per_socket) * 2 + (context->id / (cores_per_socket * NUM_SOCKETS)) - INDEX_DIFF;
	}
	else if (!strcmp(platform, "Opteron"))
	{
		my_chip = context->id / (cores_per_socket * 2);
		id_in_chip = (context->id % cores_per_socket) * 2 + (context->id / (cores_per_socket * ((my_chip * 2) + 1))) - INDEX_DIFF;
	}

	for (i = 0; i < num_of_server_launched; i++)
	{
		myrequest[i] = &(chips[my_chip][i][id_in_chip]);
	}

	context->id_in_chip = id_in_chip;
	context->mask = ((uint64_t)1 << id_in_chip);
	context->dead = 0;
	for (i = 0; i < num_of_server_launched; i++)
	{
		context->request[i] = myrequest[i];
		context->server_response[i] = server_response_set[i]->server_responses[(my_chip * NUM_SOCKETS) + ((context->id_in_chip) / NCLIENTS)];
		context->local_client_flag[i] = 0;
	}

	int num_cpu = numa_num_configured_cpus();
	struct bitmask *cpumask = numa_bitmask_alloc(num_cpu);
	numa_bitmask_setbit(cpumask, context->id);
	numa_sched_setaffinity(0, cpumask);

	pthread_setspecific(thr_context_key, context);
}

void ffwd_shutdown()
{
	finished[0] = 1;
	finished2[0] = 1;
	finished3[0] = 1;
	finished4[0] = 1;
	int i, j;
	for (i = 0; i < num_of_server_launched; i++)
	{
		pthread_join(server_thread[i], 0);
	}

	for (i = 0; i < num_of_server_launched; i++)
	{
		for (j = 0; j < NUM_SOCKETS; j++)
		{
			numa_free(chips[j][i], 4096);
		}
		numa_free(server_response_set[i], sizeof(struct server_set));
		numa_free(server_arg[i], sizeof(struct server_args));
	}
}

static void initialize_core_ordering()
{
	FILE *file;
	char text[1024], *p, *saveptr;
	int i, all_threads = 0, total_sockets = 0;

	if (!(file = popen(READ_SPEC, "r")))
		perror("popen");

	while (fgets(text, 1024, file))
	{
		for (i = 0, p = strtok_r(text, " ", &saveptr); p && strlen(p); p = strtok_r(0, " ", &saveptr))
			i++;
		total_sockets++;
		all_threads += i;
	}

	if (!(file = popen(FIND_PLATFORM, "r")))
		perror("popen");

	while (fgets(text, 1024, file))
	{
		p = strtok_r(text, "\n", &saveptr);
		if (!strcmp(p, "Xeon"))
		{
			platform = "Xeon";
		}
		else if (!strcmp(p, "Opteron"))
		{
			platform = "Opteron";
		}
		else
		{
			printf("Only XEON and Opteron platforms are supported! \n");
			// TODO check this one
			exit(-1);
		}
	}

	cores_per_socket = all_threads / total_sockets;
	num_sockets = total_sockets / 2;
	active_threads_per_socket = all_threads / num_sockets;

	cores = (int **)malloc(sizeof(int *) * total_sockets);
	for (i = 0; i < total_sockets; i++)
	{
		cores[i] = (int *)malloc(active_threads_per_socket * sizeof(int));
	}
	if (!(file = popen(READ_SPEC, "r")))
		perror("popen");

	int socket_counter = 0;
	while (fgets(text, 1024, file))
	{
		for (i = 0, p = strtok_r(text, " ", &saveptr); p && strlen(p); p = strtok_r(0, " ", &saveptr), i++)
		{
			cores[socket_counter][i] = atoi(p);
		}
		socket_counter++;
	}

	num_threads = all_threads;
	active_threads_per_socket -= (active_threads_per_socket / cores_per_socket);

	// printf("Running on %s with %d sockets and %d threads\n", platform, num_sockets, num_threads);
}

void ffwd_init()
{
	if (numa_available() < 0)
	{
		printf("System does not support NUMA API!\n");
		exit(1);
	}

	initialize_core_ordering();
	pthread_key_create(&thr_context_key, NULL);
}

// launches upto 4 servers on the first core of each socket
void launch_servers(int num_of_servers)
{
	int i, j, s, server_core, server_numa_node;

	for (s = 0; s < num_of_servers; s++)
	{

		if (!strcmp(platform, "Opteron"))
		{
			server_core = cores_per_socket * 2 * s;
			server_numa_node = server_core / (cores_per_socket * 2);
		}
		else
		{
			server_core = cores_per_socket * s;
			server_numa_node = s % NUM_SOCKETS;
		}

		server_response_set[s] = (struct server_set *)numa_alloc_onnode(sizeof(struct server_set), server_numa_node);
		for (i = 0; i < NUM_SOCKETS * MAX_NUM_OF_SERVERS; i++)
		{
			(server_response_set[s]->server_responses[i]) = (struct server_response *)numa_alloc_onnode(sizeof(struct server_response), server_numa_node);
			server_response_set[s]->server_responses[i]->flags = 0;
		}

		for (j = 0; j < NUM_SOCKETS; j++)
		{
			chips[j][s] = (struct request *)numa_alloc_onnode(4096, j);
			if (mprotect((void *)chips[j][s], 4096, PROT_EXEC | PROT_READ | PROT_WRITE))
			{
				perror("mprotect\n");
				exit(1);
			}
		}

		// prepare server's input arguments
		server_arg[s] = (struct server_args *)numa_alloc_onnode(sizeof(struct server_args), server_numa_node);
		server_arg[s]->server_core = server_core;

		for (j = 0; j < NUM_SOCKETS; j++)
		{
			server_arg[s]->chips[j] = chips[j][s];
		}

		server_arg[s]->server_response = server_response_set[s];

		// launch the server
		pthread_create(&server_thread[s], 0, server_func, (void *)(server_arg[s]));
	}
	num_of_server_launched = num_of_servers;
}
