#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/panic.h>
#include <kernel/scheduler.h>
#include <kernel/sync.h>

static uint32_t atomic_xadd_u32(volatile uint32_t* target, int32_t delta) {
	int32_t previous = delta;
	asm volatile("lock xaddl %0, %1"
		: "+r"(previous), "+m"(*target)
		:
		: "memory", "cc");
	return (uint32_t) (previous + delta);
}

static uint32_t atomic_add_u32(volatile uint32_t* target, uint32_t value) {
	return atomic_xadd_u32(target, (int32_t) value);
}

static uint32_t atomic_sub_u32(volatile uint32_t* target, uint32_t value) {
	return atomic_xadd_u32(target, -(int32_t) value);
}

static bool atomic_try_lock_u32(volatile uint32_t* target) {
	uint32_t desired = 1;
	asm volatile("xchgl %0, %1"
		: "+r"(desired), "+m"(*target)
		:
		: "memory");
	return desired == 0;
}

void kspinlock_init(kspinlock_t* lock) {
	if (lock == NULL)
		return;
	lock->value = 0;
}

void kspin_lock(kspinlock_t* lock) {
	if (lock == NULL)
		return;

	for (;;) {
		if (atomic_try_lock_u32(&lock->value))
			return;

		while (lock->value != 0)
			asm volatile("pause");
	}
}

void kspin_unlock(kspinlock_t* lock) {
	if (lock == NULL)
		return;

	asm volatile("" : : : "memory");
	lock->value = 0;
}

void kwait_queue_init(kwait_queue_t* queue) {
	if (queue == NULL)
		return;
	queue->waiter_count = 0;
}

bool kwait_wait(kwait_queue_t* queue, uint32_t timeout_ticks) {
	if (queue == NULL)
		return false;

	atomic_add_u32(&queue->waiter_count, 1u);
	const bool woke = scheduler_wait_channel(queue, timeout_ticks);
	const uint32_t after = atomic_sub_u32(&queue->waiter_count, 1u);
	if (after == UINT_MAX)
		panic("kwait_waiter_count underflow");
	return woke;
}

uint32_t kwait_wake_one(kwait_queue_t* queue) {
	if (queue == NULL)
		return 0;
	return scheduler_wake_channel(queue, 1u);
}

uint32_t kwait_wake_all(kwait_queue_t* queue) {
	if (queue == NULL)
		return 0;
	return scheduler_wake_channel(queue, 0u);
}

uint32_t kwait_waiter_count(const kwait_queue_t* queue) {
	if (queue == NULL)
		return 0;
	return queue->waiter_count;
}

void kcondition_init(kcondition_t* condition) {
	if (condition == NULL)
		return;
	kwait_queue_init(&condition->queue);
	condition->sequence = 0;
}

bool kcondition_wait(kcondition_t* condition, kspinlock_t* lock, uint32_t timeout_ticks) {
	if (condition == NULL || lock == NULL)
		return false;

	const uint32_t observed_sequence = condition->sequence;
	kspin_unlock(lock);

	bool signaled = false;
	for (;;) {
		if (condition->sequence != observed_sequence) {
			signaled = true;
			break;
		}

		const bool woke = kwait_wait(&condition->queue, timeout_ticks);
		if (!woke) {
			if (condition->sequence != observed_sequence)
				signaled = true;
			break;
		}
	}

	kspin_lock(lock);
	return signaled;
}

void kcondition_signal(kcondition_t* condition) {
	if (condition == NULL)
		return;

	atomic_add_u32(&condition->sequence, 1u);
	kwait_wake_one(&condition->queue);
}

void kcondition_broadcast(kcondition_t* condition) {
	if (condition == NULL)
		return;

	atomic_add_u32(&condition->sequence, 1u);
	kwait_wake_all(&condition->queue);
}

void kref_init(kref_t* refcount, uint32_t initial_value) {
	if (refcount == NULL)
		return;
	refcount->value = initial_value;
}

void kref_get(kref_t* refcount) {
	if (refcount == NULL)
		return;

	const uint32_t new_value = atomic_add_u32(&refcount->value, 1u);
	if (new_value == 0)
		panic("kref_get overflow");
}

bool kref_put(kref_t* refcount) {
	if (refcount == NULL)
		return false;

	const uint32_t new_value = atomic_sub_u32(&refcount->value, 1u);
	if (new_value == UINT_MAX)
		panic("kref_put underflow");
	return new_value == 0;
}

uint32_t kref_read(const kref_t* refcount) {
	if (refcount == NULL)
		return 0;
	return refcount->value;
}

void kobject_init(kobject_t* object, void (*release)(kobject_t* object)) {
	if (object == NULL)
		return;
	kref_init(&object->refcount, 1u);
	object->release = release;
}

void kobject_get(kobject_t* object) {
	if (object == NULL)
		return;
	kref_get(&object->refcount);
}

void kobject_put(kobject_t* object) {
	if (object == NULL)
		return;

	if (!kref_put(&object->refcount))
		return;

	if (object->release != NULL)
		object->release(object);
}

uint32_t kobject_refcount(const kobject_t* object) {
	if (object == NULL)
		return 0;
	return kref_read(&object->refcount);
}
