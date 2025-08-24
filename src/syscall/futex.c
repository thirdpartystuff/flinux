/*
 * This file is part of Foreign Linux.
 *
 * Copyright (C) 2014, 2015 Xiangyan Sun <wishstudio@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <common/errno.h>
#include <common/futex.h>
#include <common/time.h>
#include <syscall/futex.h>
#include <syscall/process_info.h>
#include <syscall/sig.h>
#include <syscall/syscall.h>
#include <syscall/mm.h>
#include <lib/list.h>
#include <log.h>
#include <emmintrin.h>

#include <ntdll.h>

/* TODO: How to implement interprocess futex? */

#define FUTEX_HASH_BUCKETS		256

struct futex_wait_block
{
	struct thread *thread;
	int *addr;
	struct list_node list;
};

struct futex_hash_bucket
{
	volatile LONG spin_lock;
	struct list wait_list;
};

struct futex_data
{
	struct futex_hash_bucket hash[FUTEX_HASH_BUCKETS];
} static _futex;

static struct futex_data *const futex = &_futex;

static void lock_bucket(int bucket)
{
	while (InterlockedCompareExchange(&futex->hash[bucket].spin_lock, 1, 0))
		YieldProcessor();
}

static void unlock_bucket(int bucket)
{
	futex->hash[bucket].spin_lock = 0;
}

/* Lock two buckets in deterministic order to avoid dead lock */
static void lock_buckets(int bucket1, int bucket2)
{
	if (bucket1 < bucket2)
	{
		lock_bucket(bucket1);
		lock_bucket(bucket2);
	}
	else
	{
		lock_bucket(bucket2);
		lock_bucket(bucket1);
	}
}

static int futex_hash(size_t addr)
{
	/* TODO: Improve this silly hash function */
	return addr % FUTEX_HASH_BUCKETS;
}

int futex_wait(volatile int *addr, int val, DWORD timeout)
{
	struct futex_wait_block wait_block;
	int bucket = futex_hash((size_t)addr);
	lock_bucket(bucket);
	if (*addr != val)
	{
		/* The value changed */
		unlock_bucket(bucket);
		return 0;
	}
	/* Append wait block */
	wait_block.thread = current_thread;
	wait_block.addr = (int *)addr;
	list_add(&futex->hash[bucket].wait_list, &wait_block.list);
	unlock_bucket(bucket);
	DWORD result = signal_wait(1, &current_thread->wait_event, timeout);
	if (result == WAIT_OBJECT_0)
	{
		log_info("Wait successful.");
		/* Wait successful, the waker should have removed us from the wait list */
		return 0;
	}
	else
	{
		/* Wait unsuccessful, we need to remove us from the wait list */
		lock_bucket(bucket);
		/* Check again if we're not woke, to avoid data race when we are woken between
		 * signal_wait() and lock_bucket()
		 */
		if (WaitForSingleObject(current_thread->wait_event, 0) != WAIT_OBJECT_0)
			list_remove(&futex->hash[bucket].wait_list, &wait_block.list);
		unlock_bucket(bucket);
		if (result == WAIT_INTERRUPTED)
		{
			log_info("Wait interrupted.");
			return -L_EINTR;
		}
		else //if (result == WAIT_TIMEOUT)
		{
			log_info("Wait timeout.");
			return -L_ETIMEDOUT;
		}
	}
}

static int futex_wake_requeue(int *addr, int count, int *requeue_addr, int *requeue_val)
{
	int bucket = futex_hash((size_t)addr);
	int bucket2;
	if (requeue_addr)
	{
		bucket2 = futex_hash((size_t)requeue_addr);
		if (bucket == bucket2) /* Lucky! */
			lock_bucket(bucket);
		else
			lock_buckets(bucket, bucket2);
	}
	else
		lock_bucket(bucket);
	if (requeue_addr && requeue_val)
	{
		if (*addr != *requeue_val)
		{
			/* The value changed */
			unlock_bucket(bucket);
			unlock_bucket(bucket2);
			return -L_EAGAIN;
		}
	}
	/* Wake up to count threads */
	struct list_node *prev = NULL;
	int num_woken = 0;
	for (;;)
	{
		if (requeue_addr == NULL && num_woken == count)
			break;
		struct list_node *cur;
		if (prev == NULL)
			cur = list_head(&futex->hash[bucket].wait_list);
		else
			cur = list_next(prev);
		if (cur == NULL)
			break;
		struct futex_wait_block *wait_block = list_entry(cur, struct futex_wait_block, list);
		if (wait_block->addr == addr)
		{
			if (num_woken == count && requeue_addr)
			{
				wait_block->addr = requeue_addr;
				if (bucket == bucket2)
				{
					/* We're lucky! The two addresses are in the same hash bucket. */
					prev = cur; /* Continue to handle next wait block */
				}
				else
				{
					/* Move this wait block to destination bucket */
					list_remove(&futex->hash[bucket].wait_list, cur);
					list_add(&futex->hash[bucket2].wait_list, cur);
				}
			}
			else
			{
				/* Remove current wait block and notify corresponding thread */
				list_remove(&futex->hash[bucket].wait_list, cur);
				NtSetEvent(wait_block->thread->wait_event, NULL);
				num_woken++;
			}
		}
		else
			prev = cur; /* Continue to handle next wait block */
	}
	unlock_bucket(bucket);
	if (requeue_addr && bucket != bucket2)
		unlock_bucket(bucket2);
	return num_woken;
}

int futex_wake(int *addr, int count)
{
	return futex_wake_requeue(addr, count, NULL, NULL);
}

int futex_requeue(int *addr, int count, int *requeue_addr, int *requeue_val)
{
	return futex_wake_requeue(addr, count, requeue_addr, requeue_val);
}

DEFINE_SYSCALL6(futex, int *, uaddr, int, op, int, val, const struct linux_timespec *, timeout, int *, uaddr2, int, val3)
{
	log_info("futex(%p, %d, %d, %p, %p, %d)", uaddr, op, val, timeout, uaddr2, val3);
	if (!mm_check_read(uaddr, sizeof(int)))
		return -L_EACCES;
	switch (op & FUTEX_CMD_MASK)
	{
	case FUTEX_WAIT:
	{
		if (timeout && !mm_check_read(timeout, sizeof(struct linux_timespec)))
			return -L_EFAULT;
		DWORD time = timeout ? timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000 : INFINITE;
		return futex_wait((volatile int *)uaddr, val, time);
	}

	case FUTEX_WAKE:
		return futex_wake(uaddr, val);

	case FUTEX_REQUEUE:
	{
		if (!mm_check_read(uaddr2, sizeof(int)))
			return -L_EACCES;
		return futex_requeue(uaddr, val, uaddr2, NULL);
	}

	case FUTEX_CMP_REQUEUE:
	{
		if (!mm_check_read(uaddr2, sizeof(int)))
			return -L_EACCES;
		return futex_requeue(uaddr, val, uaddr2, &val3);
	}

	default:
		log_error("Unsupported futex operation, returning ENOSYS");
		return -L_ENOSYS;
	}
}

DEFINE_SYSCALL2(set_robust_list, struct robust_list_head *, head, int, len)
{
	log_info("set_robust_list(head=%p, len=%d)", head, len);
	if (len != sizeof(struct robust_list_head))
		log_error("len (%d) != sizeof(struct robust_list_head)", len);
	log_error("set_robust_list() not supported.");
	return 0;
}
