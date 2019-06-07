// SPDX-License-Identifier: GPL-2.0
/*
 * uepoll-test.c - Test cases for epoll_create2(), namely pollable
 * epoll from userspace.  Copyright (c) 2019 Roman Penyaev
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <syscall.h>
#include <numa.h>

#include "../kselftest.h"
#include "../kselftest_harness.h"

#include <linux/eventpoll.h>
#include <linux/types.h>

#define BUILD_BUG_ON(condition) ((void)sizeof(char [1 - 2*!!(condition)]))
#define READ_ONCE(v) (*(volatile typeof(v)*)&(v))

#define ITERS     1000000ull

/*
 * Add main epoll functions manually, because sys/epoll.h conflicts
 * with linux/eventpoll.h.
 */
extern int epoll_create1(int __flags);
extern int epoll_ctl(int __epfd, int __op, int __fd,
		     struct epoll_event *__event);
extern int epoll_wait(int __epfd, struct epoll_event *__events,
		      int __maxevents, int __timeout);

static inline long epoll_create2(int flags, size_t size)
{
	return syscall(__NR_epoll_create2, flags, size);
}

struct thread_ctx {
	pthread_t thread;
	int efd;
};

struct cpu_map {
	unsigned int nr;
	unsigned int map[];
};

static volatile unsigned int thr_ready;
static volatile unsigned int start;

static inline unsigned int max_index_nr(struct epoll_uheader *header)
{
	return header->index_length >> 2;
}

static int is_cpu_online(int cpu)
{
	char buf[64];
	char online;
	FILE *f;
	int rc;

	snprintf(buf, sizeof(buf), "/sys/devices/system/cpu/cpu%d/online", cpu);
	f = fopen(buf, "r");
	if (!f)
		return 1;

	rc = fread(&online, 1, 1, f);
	assert(rc == 1);
	fclose(f);

	return (char)online == '1';
}

static struct cpu_map *cpu_map__new(void)
{
	struct cpu_map *cpu;
	struct bitmask *bm;

	int i, bit, cpus_nr;

	cpus_nr = numa_num_possible_cpus();
	cpu = calloc(1, sizeof(*cpu) + sizeof(cpu->map[0]) * cpus_nr);
	if (!cpu)
		return NULL;

	bm = numa_all_cpus_ptr;
	assert(bm);

	for (bit = 0, i = 0; bit < bm->size; bit++) {
		if (numa_bitmask_isbitset(bm, bit) && is_cpu_online(bit)) {
			cpu->map[i++] = bit;
		}
	}
	cpu->nr = i;

	return cpu;
}

static void cpu_map__put(struct cpu_map *cpu)
{
	free(cpu);
}

static inline unsigned long long nsecs(void)
{
	struct timespec ts = {0, 0};

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((unsigned long long)ts.tv_sec * 1000000000ull) + ts.tv_nsec;
}

static void *thread_work(void *arg)
{
	struct thread_ctx *ctx = arg;
	uint64_t ucnt = 1;
	unsigned int i;
	int rc;

	__atomic_add_fetch(&thr_ready, 1, __ATOMIC_RELAXED);

	while (!start)
		;

	for (i = 0; i < ITERS; i++) {
		rc = write(ctx->efd, &ucnt, sizeof(ucnt));
		assert(rc == sizeof(ucnt));
	}

	return NULL;
}

static inline _Bool read_event(struct epoll_uheader *header,
			       unsigned int *index, unsigned int idx,
			       struct epoll_event *event)
{
	struct epoll_uitem *item;
	unsigned int *item_idx_ptr;
	unsigned int indeces_mask;

	indeces_mask = max_index_nr(header) - 1;
	if (indeces_mask & max_index_nr(header)) {
		assert(0);
		/* Should be pow2, corrupted header? */
		return 0;
	}

	item_idx_ptr = &index[idx & indeces_mask];

	/* Load index */
	idx = __atomic_load_n(item_idx_ptr, __ATOMIC_ACQUIRE);
	if (idx >= header->max_items_nr) {
		assert(0);
		/* Corrupted index? */
		return 0;
	}

	item = &header->items[idx];

	/*
	 * Fetch data first, if event is cleared by the kernel we drop the data
	 * returning false.
	 */
	event->data = (__u64) item->data;
	event->events = __atomic_exchange_n(&item->ready_events, 0,
					    __ATOMIC_RELEASE);

	return (event->events & ~EPOLLREMOVED);
}

static int uepoll_wait(struct epoll_uheader *header, unsigned int *index,
		       int epfd, struct epoll_event *events, int maxevents)

{
	/*
	 * Before entering kernel we do busy wait for ~1ms, naively assuming
	 * each iteration costs 1 cycle, 1 ns.
	 */
	unsigned int spins = 1000000;
	unsigned int tail;
	int i;

	assert(maxevents > 0);

again:
	/*
	 * Cache the tail because we don't want refetch it on each iteration
	 * and then catch live events updates, i.e. we don't want user @events
	 * array consist of events from the same fds.
	 */
	tail = READ_ONCE(header->tail);

	if (header->head == tail) {
		if (spins--)
			/* Busy loop a bit */
			goto again;

		i = epoll_wait(epfd, NULL, 0, -1);
		assert(i < 0);
		if (errno != ESTALE)
			return i;

		tail = READ_ONCE(header->tail);
		assert(header->head != tail);
	}

	for (i = 0; header->head != tail && i < maxevents; header->head++) {
		if (read_event(header, index, header->head, &events[i]))
			i++;
		else
			/* Event can't be removed under us */
			assert(0);
	}

	return i;
}

static void uepoll_mmap(int epfd, struct epoll_uheader **_header,
		       unsigned int **_index)
{
	struct epoll_uheader *header;
	unsigned int *index, len;

	BUILD_BUG_ON(sizeof(*header) != EPOLL_USERPOLL_HEADER_SIZE);
	BUILD_BUG_ON(sizeof(header->items[0]) != 16);

	len = sysconf(_SC_PAGESIZE);
again:
	header = mmap(NULL, len, PROT_WRITE|PROT_READ, MAP_SHARED, epfd, 0);
	if (header == MAP_FAILED)
		ksft_exit_fail_msg("Failed map(header)\n");

	if (header->header_length != len) {
		unsigned int tmp_len = len;

		len = header->header_length;
		munmap(header, tmp_len);
		goto again;
	}
	assert(header->magic == EPOLL_USERPOLL_HEADER_MAGIC);

	index = mmap(NULL, header->index_length, PROT_WRITE|PROT_READ,
		     MAP_SHARED, epfd, header->header_length);
	if (index == MAP_FAILED)
		ksft_exit_fail_msg("Failed map(index)\n");

	*_header = header;
	*_index = index;
}

static void uepoll_munmap(struct epoll_uheader *header,
			  unsigned int *index)
{
	int rc;

	rc = munmap(index, header->index_length);
	if (rc)
		ksft_exit_fail_msg("Failed munmap(index)\n");

	rc = munmap(header, header->header_length);
	if (rc)
		ksft_exit_fail_msg("Failed munmap(header)\n");
}

static int do_bench(struct cpu_map *cpu, unsigned int nthreads)
{
	struct epoll_event ev, events[nthreads];
	struct thread_ctx threads[nthreads];
	pthread_attr_t thrattr;
	struct thread_ctx *ctx;
	int rc, epfd, nfds;
	cpu_set_t cpuset;
	unsigned int i;

	struct epoll_uheader *header;
	unsigned int *index;

	unsigned long long epoll_calls = 0, epoll_nsecs;
	unsigned long long ucnt, ucnt_sum = 0, eagains = 0;

	thr_ready = 0;
	start = 0;

	epfd = epoll_create2(EPOLL_USERPOLL, nthreads);
	if (epfd < 0)
		ksft_exit_fail_msg("Failed epoll_create2()\n");

	for (i = 0; i < nthreads; i++) {
		ctx = &threads[i];

		ctx->efd = eventfd(0, EFD_NONBLOCK);
		if (ctx->efd < 0)
			ksft_exit_fail_msg("Failed eventfd()\n");

		ev.events = EPOLLIN | EPOLLET;
		ev.data = (uintptr_t) ctx;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, ctx->efd, &ev);
		if (rc)
			ksft_exit_fail_msg("Failed epoll_ctl()\n");

		CPU_ZERO(&cpuset);
		CPU_SET(cpu->map[i % cpu->nr], &cpuset);

		pthread_attr_init(&thrattr);
		rc = pthread_attr_setaffinity_np(&thrattr, sizeof(cpu_set_t),
						 &cpuset);
		if (rc) {
			errno = rc;
			ksft_exit_fail_msg("Failed pthread_attr_setaffinity_np()\n");
		}

		rc = pthread_create(&ctx->thread, NULL, thread_work, ctx);
		if (rc) {
			errno = rc;
			ksft_exit_fail_msg("Failed pthread_create()\n");
		}
	}

	/* Mmap all pointers */
	uepoll_mmap(epfd, &header, &index);

	while (thr_ready != nthreads)
		;

	/* Signal start for all threads */
	start = 1;

	epoll_nsecs = nsecs();
	while (1) {
		nfds = uepoll_wait(header, index, epfd, events, nthreads);
		if (nfds < 0)
			ksft_exit_fail_msg("Failed uepoll_wait()\n");

		epoll_calls++;

		for (i = 0; i < (unsigned int)nfds; ++i) {
			ctx = (void *)(uintptr_t) events[i].data;
			rc = read(ctx->efd, &ucnt, sizeof(ucnt));
			if (rc < 0) {
				assert(errno == EAGAIN);
				continue;
			}
			assert(rc == sizeof(ucnt));
			ucnt_sum += ucnt;
			if (ucnt_sum == nthreads * ITERS)
				goto end;
		}
	}
end:
	epoll_nsecs = nsecs() - epoll_nsecs;

	for (i = 0; i < nthreads; i++) {
		ctx = &threads[i];
		pthread_join(ctx->thread, NULL);
	}
	uepoll_munmap(header, index);
	close(epfd);

	ksft_print_msg("%7d   %8lld     %8lld\n",
	       nthreads,
	       ITERS*nthreads/(epoll_nsecs/1000/1000),
	       epoll_nsecs/1000/1000);

	return 0;
}

/**
 * uepoll loop
 */
TEST(uepoll_basics)
{
	unsigned int i, nthreads_arr[] = {8, 16, 32, 64};
	struct cpu_map *cpu;

	cpu = cpu_map__new();
	if (!cpu) {
		errno = ENOMEM;
		ksft_exit_fail_msg("Failed cpu_map__new()\n");
	}

	ksft_print_msg("threads  events/ms  run-time ms\n");
	for (i = 0; i < ARRAY_SIZE(nthreads_arr); i++)
		do_bench(cpu, nthreads_arr[i]);

	cpu_map__put(cpu);
}

/**
 * Checks different flags and args
 */
TEST(uepoll_args)
{
	struct epoll_event ev;
	int epfd, evfd, rc;

	/* Fail */
	epfd = epoll_create2(EPOLL_USERPOLL, (1<<16)+1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(epfd, -1);

	/* Fail */
	epfd = epoll_create2(EPOLL_USERPOLL, 0);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(epfd, -1);

	/* Success */
	epfd = epoll_create2(EPOLL_USERPOLL, (1<<16));
	ASSERT_GE(epfd, 0);

	/* Success */
	evfd = eventfd(0, EFD_NONBLOCK);
	ASSERT_GE(evfd, 0);

	/* Fail, expect EPOLLET */
	ev.events = EPOLLIN;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, evfd, &ev);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(rc, -1);

	/* Fail, no support for EPOLLEXCLUSIVE */
	ev.events = EPOLLIN | EPOLLET | EPOLLEXCLUSIVE;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, evfd, &ev);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(rc, -1);

	/* Success */
	ev.events = EPOLLIN | EPOLLET;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, evfd, &ev);
	ASSERT_EQ(rc, 0);

	/* Fail, expect events and maxevents as zeroes */
	rc = epoll_wait(epfd, &ev, 1, -1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(rc, -1);

	/* Fail, expect events as zero */
	rc = epoll_wait(epfd, &ev, 0, -1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(rc, -1);

	/* Fail, expect maxevents as zero */
	rc = epoll_wait(epfd, NULL, 1, -1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(rc, -1);

	/* Success */
	rc = epoll_wait(epfd, NULL, 0, 0);
	ASSERT_EQ(rc, 0);

	close(epfd);
	close(evfd);
}

static void *signal_eventfd_work(void *arg)
{
	unsigned long long cnt;

	int rc, evfd = *(int *)arg;

	sleep(1);
	cnt = 1;
	rc = write(evfd, &cnt, sizeof(cnt));
	assert(rc == 8);

	return NULL;
}

/**
 * Nested poll
 */
TEST(uepoll_poll)
{
	int epfd, uepfd, evfd, rc;

	struct epoll_event ev;
	pthread_t thread;

	/* Success */
	uepfd = epoll_create2(EPOLL_USERPOLL, 128);
	ASSERT_GE(uepfd, 0);

	/* Success */
	epfd = epoll_create2(0, 0);
	ASSERT_GE(epfd, 0);

	/* Success */
	evfd = eventfd(0, EFD_NONBLOCK);
	ASSERT_GE(evfd, 0);

	/* Success */
	ev.events = EPOLLIN | EPOLLET;
	rc = epoll_ctl(uepfd, EPOLL_CTL_ADD, evfd, &ev);
	ASSERT_EQ(rc, 0);

	/* Success */
	ev.events = EPOLLIN | EPOLLET;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, uepfd, &ev);
	ASSERT_EQ(rc, 0);

	/* Success */
	rc = epoll_wait(epfd, &ev, 1, 0);
	ASSERT_EQ(rc, 0);

	/* Success */
	rc = pthread_create(&thread, NULL, signal_eventfd_work, &evfd);
	ASSERT_EQ(rc, 0);

	/* Success */
	rc = epoll_wait(epfd, &ev, 1, 5000);
	ASSERT_EQ(rc, 1);

	close(uepfd);
	close(epfd);
	close(evfd);
}

/**
 * One shot
 */
TEST(uepoll_epolloneshot)
{
	int epfd, evfd, rc;
	unsigned long long cnt;

	struct epoll_uheader *header;
	unsigned int *index;

	struct epoll_event ev;
	pthread_t thread;

	/* Success */
	epfd = epoll_create2(EPOLL_USERPOLL, 128);
	ASSERT_GE(epfd, 0);

	/* Mmap all pointers */
	uepoll_mmap(epfd, &header, &index);

	/* Success */
	evfd = eventfd(0, EFD_NONBLOCK);
	ASSERT_GE(evfd, 0);

	/* Success */
	ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, evfd, &ev);
	ASSERT_EQ(rc, 0);

	/* Success */
	rc = pthread_create(&thread, NULL, signal_eventfd_work, &evfd);
	ASSERT_EQ(rc, 0);

	/* Fail, expect -ESTALE */
	rc = epoll_wait(epfd, NULL, 0, 3000);
	ASSERT_EQ(errno, ESTALE);
	ASSERT_EQ(rc, -1);

	/* Success */
	rc = uepoll_wait(header, index, epfd, &ev, 1);
	ASSERT_EQ(rc, 1);

	/* Success */
	rc = pthread_create(&thread, NULL, signal_eventfd_work, &evfd);
	ASSERT_EQ(rc, 0);

	/* Success */
	rc = epoll_wait(epfd, NULL, 0, 3000);
	ASSERT_EQ(rc, 0);


	/* Success */
	ev.events = EPOLLIN | EPOLLET;
	rc = epoll_ctl(epfd, EPOLL_CTL_MOD, evfd, &ev);
	ASSERT_EQ(rc, 0);

	/* Success */
	rc = pthread_create(&thread, NULL, signal_eventfd_work, &evfd);
	ASSERT_EQ(rc, 0);

	/* Success */
	rc = uepoll_wait(header, index, epfd, &ev, 1);
	ASSERT_EQ(rc, 1);

	/* Success */
	rc = pthread_create(&thread, NULL, signal_eventfd_work, &evfd);
	ASSERT_EQ(rc, 0);

	/* Success */
	rc = uepoll_wait(header, index, epfd, &ev, 1);
	ASSERT_EQ(rc, 1);

	uepoll_munmap(header, index);
	close(epfd);
	close(evfd);
}

TEST_HARNESS_MAIN
