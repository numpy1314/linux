// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/wait.h>

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

static struct kunit_case axvisor_wait_cases[] = {
	KUNIT_CASE(axvisor_wait_before_registration),
	KUNIT_CASE(axvisor_wait_atomic_and_wrap),
	KUNIT_CASE(axvisor_wait_after_registration),
	KUNIT_CASE(axvisor_wait_old_order_control),
	{}
};

static struct kunit_suite axvisor_wait_suite = {
	.name = "axvisor-linux-wait",
	.test_cases = axvisor_wait_cases,
};

kunit_test_suite(axvisor_wait_suite);
#endif
