// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/wait.h>
#ifdef CONFIG_ARM64
#include <linux/hrtimer.h>
#include <linux/sched/signal.h>
#include <linux/timekeeping.h>
#endif

#include "axvisor_ffi.h"

struct axvisor_wait_queue {
	wait_queue_head_t head;
	atomic64_t generation;
};

unsigned long axvisor_linux_wait_queue_create(void)
{
	struct axvisor_wait_queue *queue;
	gfp_t flags = irqs_disabled() ? GFP_ATOMIC : GFP_KERNEL;

	queue = kmalloc(sizeof(*queue), flags);
	if (!queue)
		return 0;
	init_waitqueue_head(&queue->head);
	atomic64_set(&queue->generation, 0);
	return (unsigned long)queue;
}

void axvisor_linux_wait_queue_destroy(unsigned long handle)
{
	/* The owner must join all waiters and quiesce producers before destroy. */
	kfree((void *)handle);
}

u64 axvisor_linux_wait_queue_generation(unsigned long handle)
{
	struct axvisor_wait_queue *queue = (void *)handle;

	/* Pair with publication before wake; keep the predicate after snapshot. */
	return queue ? (u64)atomic64_read_acquire(&queue->generation) : 0;
}

void axvisor_linux_wait_queue_wait_since(unsigned long handle, u64 generation)
{
	struct axvisor_wait_queue *queue = (void *)handle;

	if (!queue)
		return;
	if (irqs_disabled() || in_atomic()) {
		while (axvisor_linux_wait_queue_generation(handle) == generation)
			cpu_relax();
		return;
	}
	/* wait_event rechecks after registration, covering the final sleep gap. */
	wait_event(queue->head,
		   axvisor_linux_wait_queue_generation(handle) != generation);
}

void axvisor_linux_wait_queue_wait(unsigned long handle)
{
	axvisor_linux_wait_queue_wait_since(handle,
			axvisor_linux_wait_queue_generation(handle));
}

#ifdef CONFIG_ARM64
/*
 * A control-vCPU wait is interruptible, unlike the existing SyncIf lock wait.
 * Rust samples generation before evaluating its predicate. Enrolling here
 * before rechecking generation closes the predicate-to-sleep wakeup gap.
 * The private deadline supports bounded KUnit probes only; the production
 * control contract waits for a notification or signal, with no timeout.
 * Return 0 for a notification, 1 for test timeout, or a negative errno.
 */
static int axvisor_control_wait_since_deadline(unsigned long handle, u64 generation,
					       const u8 *blocked_bytes, size_t length,
					       bool timed, u64 deadline_nanos)
{
	struct axvisor_wait_queue *queue = (void *)handle;
	sigset_t blocked, saved_real;
	ktime_t deadline = min_t(u64, deadline_nanos, KTIME_MAX);
	DEFINE_WAIT(wait);
	int ret;

	if (!queue || length > sizeof(blocked) || (length && !blocked_bytes))
		return -EINVAL;
	if (irqs_disabled() || in_atomic())
		return -EWOULDBLOCK;
	/* A custom KVM signal mask must affect the scheduler's signal_pending
	 * test as well as our explicit predicate. Otherwise an ignored signal
	 * spins the wait, or a newly unblocked signal never wakes it.
	 */
	if (length) {
		memset(&blocked, 0, sizeof(blocked));
		memcpy(&blocked, blocked_bytes, length);
		sigdelsetmask(&blocked, sigmask(SIGKILL) | sigmask(SIGSTOP));
		/* Like kvm_sigset_activate(), retain the original mask in
		 * real_blocked so signal generation does not mistake a temporary
		 * unblock for the userspace thread's permanent delivery mask.
		 */
		saved_real = current->real_blocked;
		sigprocmask(SIG_SETMASK, &blocked, &current->real_blocked);
	}
	for (;;) {
		prepare_to_wait(&queue->head, &wait, TASK_INTERRUPTIBLE);
		if (axvisor_linux_wait_queue_generation(handle) != generation) {
			ret = 0;
			break;
		}
		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}
		if (timed && ktime_compare(ktime_get(), deadline) >= 0) {
			ret = 1;
			break;
		}
		if (timed)
			schedule_hrtimeout_range_clock(&deadline, 0, HRTIMER_MODE_ABS,
						 CLOCK_MONOTONIC);
		else
			schedule();
	}
	finish_wait(&queue->head, &wait);
	if (length) {
		sigprocmask(SIG_SETMASK, &current->real_blocked, NULL);
		current->real_blocked = saved_real;
	}
	return ret;
}

int axvisor_linux_control_wait_since(unsigned long handle, u64 generation,
				    const u8 *blocked_bytes, size_t length)
{
	return axvisor_control_wait_since_deadline(handle, generation, blocked_bytes,
						 length, false, 0);
}
#endif

void axvisor_linux_wait_queue_wake_one(unsigned long handle)
{
	struct axvisor_wait_queue *queue = (void *)handle;

	if (!queue)
		return;
	/* Publish predicate writes even when no waiter is registered yet. */
	atomic64_inc_return_release(&queue->generation);
	wake_up(&queue->head);
}

void axvisor_linux_wait_queue_wake_all(unsigned long handle)
{
	struct axvisor_wait_queue *queue = (void *)handle;

	if (!queue)
		return;
	atomic64_inc_return_release(&queue->generation);
	wake_up_all(&queue->head);
}

#if IS_ENABLED(CONFIG_AXVISOR_LINUX_WAIT_KUNIT_TEST)
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>

static void axvisor_wait_before_registration(struct kunit *test)
{
	unsigned long queue = axvisor_linux_wait_queue_create();
	u64 generation;

	KUNIT_ASSERT_NE(test, queue, 0UL);
	generation = axvisor_linux_wait_queue_generation(queue);
	axvisor_linux_wait_queue_wake_one(queue);
	axvisor_linux_wait_queue_wait_since(queue, generation);
	KUNIT_EXPECT_NE(test, axvisor_linux_wait_queue_generation(queue), generation);
	axvisor_linux_wait_queue_destroy(queue);
}

static void axvisor_wait_atomic_and_wrap(struct kunit *test)
{
	unsigned long queue = axvisor_linux_wait_queue_create();
	struct axvisor_wait_queue *q = (void *)queue;
	unsigned long flags;
	u64 generation;

	KUNIT_ASSERT_NE(test, queue, 0UL);
	atomic64_set(&q->generation, -1);
	generation = axvisor_linux_wait_queue_generation(queue);
	axvisor_linux_wait_queue_wake_all(queue);
	local_irq_save(flags);
	axvisor_linux_wait_queue_wait_since(queue, generation);
	local_irq_restore(flags);
	KUNIT_EXPECT_EQ(test, axvisor_linux_wait_queue_generation(queue), 0ULL);
	axvisor_linux_wait_queue_destroy(queue);
}

struct axvisor_wait_test_context {
	unsigned long queue;
	u64 generation;
	struct completion done;
	wait_queue_head_t stop;
};

static int axvisor_wait_test_thread(void *arg)
{
	struct axvisor_wait_test_context *ctx = arg;

	axvisor_linux_wait_queue_wait_since(ctx->queue, ctx->generation);
	complete(&ctx->done);
	/* Keep the context and task alive until the test has checked the result. */
	wait_event(ctx->stop, kthread_should_stop());
	return 0;
}

static bool axvisor_wait_registered(struct axvisor_wait_queue *queue)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	unsigned long flags;
	bool registered;

	do {
		spin_lock_irqsave(&queue->head.lock, flags);
		registered = !list_empty(&queue->head.head);
		spin_unlock_irqrestore(&queue->head.lock, flags);
		if (registered)
			return true;
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));
	return false;
}

static void axvisor_wait_thread_case(struct kunit *test, bool old_order)
{
	struct axvisor_wait_test_context ctx;
	struct task_struct *task;
	bool registered;

	ctx.queue = axvisor_linux_wait_queue_create();
	KUNIT_ASSERT_NE(test, ctx.queue, 0UL);
	init_completion(&ctx.done);
	init_waitqueue_head(&ctx.stop);
	ctx.generation = axvisor_linux_wait_queue_generation(ctx.queue);
	/* Model a true predicate + wake between the old check and snapshot. */
	if (old_order) {
		axvisor_linux_wait_queue_wake_all(ctx.queue);
		ctx.generation = axvisor_linux_wait_queue_generation(ctx.queue);
	}
	/* Fix the token before spawning so even a delayed worker observes the
	 * recovery wake. The old-order token has already missed the first wake.
	 */
	task = kthread_run(axvisor_wait_test_thread, &ctx, "axvisor-wait-test");
	if (IS_ERR(task)) {
		axvisor_linux_wait_queue_destroy(ctx.queue);
		KUNIT_FAIL(test, "cannot start waiter");
		return;
	}
	registered = axvisor_wait_registered((void *)ctx.queue);
	KUNIT_EXPECT_TRUE(test, registered);
	KUNIT_EXPECT_FALSE(test, completion_done(&ctx.done));
	/* The negative control is detected without leaving a stuck task behind. */
	if (old_order && registered)
		kunit_info(test, "old-order lost notification detected; issuing recovery wake\n");
	axvisor_linux_wait_queue_wake_all(ctx.queue);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&ctx.done,
			msecs_to_jiffies(1000)), 0UL);
	kthread_stop(task);
	axvisor_linux_wait_queue_destroy(ctx.queue);
}

static void axvisor_wait_after_registration(struct kunit *test)
{
	axvisor_wait_thread_case(test, false);
}

static void axvisor_wait_old_order_control(struct kunit *test)
{
	axvisor_wait_thread_case(test, true);
}

#ifdef CONFIG_ARM64
static void axvisor_control_wait_generation_timeout(struct kunit *test)
{
	unsigned long queue = axvisor_linux_wait_queue_create();
	unsigned long flags;
	u64 generation;
	int ret;

	KUNIT_ASSERT_NE(test, queue, 0UL);
	generation = axvisor_linux_wait_queue_generation(queue);
	axvisor_linux_wait_queue_wake_all(queue);
	KUNIT_EXPECT_EQ(test, axvisor_linux_control_wait_since(queue, generation,
			NULL, 0), 0);
	generation = axvisor_linux_wait_queue_generation(queue);
	KUNIT_EXPECT_EQ(test, axvisor_control_wait_since_deadline(queue, generation,
			NULL, 0, true, ktime_get_ns() + 2 * NSEC_PER_MSEC), 1);
	local_irq_save(flags);
	ret = axvisor_linux_control_wait_since(queue, generation, NULL, 0);
	local_irq_restore(flags);
	KUNIT_EXPECT_EQ(test, ret, -EWOULDBLOCK);
	axvisor_linux_wait_queue_destroy(queue);
}

struct axvisor_control_signal_test {
	unsigned long queue;
	u64 generation;
	struct completion done;
	wait_queue_head_t stop;
	int result;
	bool restored;
};

static int axvisor_control_signal_thread(void *arg)
{
	struct axvisor_control_signal_test *ctx = arg;
	sigset_t original, original_real, blocked;

	allow_signal(SIGUSR1);
	allow_signal(SIGUSR2);
	/* Originally USR1 is allowed and USR2 is blocked. The wait must apply
	 * the opposite mask for both pending-signal checks and scheduler wake.
	 */
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR2);
	sigprocmask(SIG_BLOCK, &blocked, NULL);
	original = current->blocked;
	original_real = current->real_blocked;
	blocked = original;
	sigaddset(&blocked, SIGUSR1);
	sigdelset(&blocked, SIGUSR2);
	ctx->result = axvisor_linux_control_wait_since(ctx->queue, ctx->generation,
			(const u8 *)&blocked, sizeof(blocked));
	ctx->restored = sigequalsets(&original, &current->blocked) &&
			sigequalsets(&original_real, &current->real_blocked);
	flush_signals(current);
	complete(&ctx->done);
	wait_event(ctx->stop, kthread_should_stop());
	return 0;
}

static void axvisor_control_wait_signal_mask(struct kunit *test)
{
	struct axvisor_control_signal_test ctx;
	struct task_struct *task;
	bool registered;

	ctx.queue = axvisor_linux_wait_queue_create();
	KUNIT_ASSERT_NE(test, ctx.queue, 0UL);
	ctx.generation = axvisor_linux_wait_queue_generation(ctx.queue);
	init_completion(&ctx.done);
	init_waitqueue_head(&ctx.stop);
	ctx.result = -EINPROGRESS;
	ctx.restored = false;
	task = kthread_run(axvisor_control_signal_thread, &ctx, "axvisor-signal-test");
	if (IS_ERR(task)) {
		axvisor_linux_wait_queue_destroy(ctx.queue);
		KUNIT_FAIL(test, "cannot start signal waiter");
		return;
	}
	registered = axvisor_wait_registered((void *)ctx.queue);
	KUNIT_EXPECT_TRUE(test, registered);
	if (registered) {
		KUNIT_EXPECT_EQ(test, send_sig(SIGUSR1, task, 1), 0);
		KUNIT_EXPECT_EQ(test, wait_for_completion_timeout(&ctx.done,
				msecs_to_jiffies(20)), 0UL);
		KUNIT_EXPECT_EQ(test, send_sig(SIGUSR2, task, 1), 0);
		KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&ctx.done,
				msecs_to_jiffies(1000)), 0UL);
	}
	/* Also release a broken wait implementation, keeping failures bounded. */
	axvisor_linux_wait_queue_wake_all(ctx.queue);
	kthread_stop(task);
	KUNIT_EXPECT_EQ(test, ctx.result, -EINTR);
	KUNIT_EXPECT_TRUE(test, ctx.restored);
	axvisor_linux_wait_queue_destroy(ctx.queue);
}
#endif

static struct kunit_case axvisor_wait_cases[] = {
	KUNIT_CASE(axvisor_wait_before_registration),
	KUNIT_CASE(axvisor_wait_atomic_and_wrap),
	KUNIT_CASE(axvisor_wait_after_registration),
	KUNIT_CASE(axvisor_wait_old_order_control),
#ifdef CONFIG_ARM64
	KUNIT_CASE(axvisor_control_wait_generation_timeout),
	KUNIT_CASE(axvisor_control_wait_signal_mask),
#endif
	{}
};

static struct kunit_suite axvisor_wait_suite = {
	.name = "axvisor-linux-wait",
	.test_cases = axvisor_wait_cases,
};

kunit_test_suite(axvisor_wait_suite);
#endif
