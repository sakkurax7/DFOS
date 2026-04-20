#ifndef KERNEL_SYNC_H
#define KERNEL_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include <kernel/scheduler.h>

#define KWAIT_FOREVER SCHEDULER_WAIT_FOREVER

typedef struct kspinlock {
	volatile uint32_t value;
} kspinlock_t;

typedef uint32_t kirq_state_t;

typedef struct kwait_queue {
	volatile uint32_t waiter_count;
} kwait_queue_t;

typedef struct kcondition {
	kwait_queue_t queue;
	volatile uint32_t sequence;
} kcondition_t;

typedef struct kref {
	volatile uint32_t value;
} kref_t;

typedef struct kobject {
	kref_t refcount;
	void (*release)(struct kobject* object);
} kobject_t;

typedef struct kmutex {
	kspinlock_t lock;
	kcondition_t condition;
	volatile bool locked;
	volatile uint32_t owner_task_id;
} kmutex_t;

void kspinlock_init(kspinlock_t* lock);
void kspin_lock(kspinlock_t* lock);
void kspin_unlock(kspinlock_t* lock);
kirq_state_t kspin_lock_irqsave(kspinlock_t* lock);
void kspin_unlock_irqrestore(kspinlock_t* lock, kirq_state_t irq_state);

void kwait_queue_init(kwait_queue_t* queue);
bool kwait_wait(kwait_queue_t* queue, uint32_t timeout_ticks);
uint32_t kwait_wake_one(kwait_queue_t* queue);
uint32_t kwait_wake_all(kwait_queue_t* queue);
uint32_t kwait_waiter_count(const kwait_queue_t* queue);

void kcondition_init(kcondition_t* condition);
bool kcondition_wait(kcondition_t* condition, kspinlock_t* lock, uint32_t timeout_ticks);
void kcondition_signal(kcondition_t* condition);
void kcondition_broadcast(kcondition_t* condition);

void kref_init(kref_t* refcount, uint32_t initial_value);
void kref_get(kref_t* refcount);
bool kref_put(kref_t* refcount);
uint32_t kref_read(const kref_t* refcount);

void kobject_init(kobject_t* object, void (*release)(kobject_t* object));
void kobject_get(kobject_t* object);
void kobject_put(kobject_t* object);
uint32_t kobject_refcount(const kobject_t* object);

void kmutex_init(kmutex_t* mutex);
bool kmutex_try_lock(kmutex_t* mutex);
void kmutex_lock(kmutex_t* mutex);
bool kmutex_lock_timeout(kmutex_t* mutex, uint32_t timeout_ticks);
void kmutex_unlock(kmutex_t* mutex);
bool kmutex_is_locked(const kmutex_t* mutex);
uint32_t kmutex_owner_task(const kmutex_t* mutex);

#endif
