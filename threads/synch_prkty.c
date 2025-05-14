/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* 세마포어 SEMA를 VALUE로 초기화합니다. 세마포어는
   음이 아닌 정수이며, 이를 조작하기 위한 
   두 개의 원자 연산자가 함께 제공됩니다.:

   - down or "P": 값이 양수가 될 때까지 기다린 후 감소시킵니다.

   - up or "V": 값을 증가시키고 (대기 중인 스레드가 있으면 하나 깨웁니다). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/////////////////////
// 스레드 스트럭쳐 내부의 인자 가져옴(priority가져와야함)
bool priority_sema_cmp (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *T_A = list_entry(a, struct thread, elem);
	struct thread *T_B = list_entry(b, struct thread, elem);

	return T_A -> priority > T_B -> priority;
}

/* 세마포어에 대한 Down 또는 "P" 연산. 
   SEMA 값이 양수가 될 때까지 기다린 후 원자적으로 감소시킵니다.

   이 함수는 휴면 상태에 있을 수 있으므로 인터럽트 핸들러 내에서 호출해서는 안 됩니다.
   이 함수는 인터럽트가 비활성화된 상태에서 호출될 수 있지만, 
   휴면 상태에 들어가면 다음에 예약된 스레드가 인터럽트를 다시 활성화할 가능성이 높습니다.
   이 함수는 sema_down 함수입니다. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		list_insert_ordered (&sema->waiters, &thread_current ()->elem, priority_sema_cmp, NULL);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}
// 세마포어가 공유자원을 엑세스할 수 있는 여부를 숫자로 표사하므로 세마포어 값을 올리거나 낮출때, 
// waiters 리스트에서 스레드의 prority 우선순위를 비교하여 정렬하여 삽입하고 빼내야할 것 같다.
//////////////////////////

/* 세마포어에 대한 Down 또는 "P" 연산(세마포어가 이미 0이 아닌 경우에만 해당).
   세마포어가 감소하면 true를 반환하고, 그렇지 않으면 false를 반환합니다.

   이 함수는 인터럽트 핸들러에서 호출될 수 있습니다. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* 세마포어에 대한 Up 또는 "V" 연산. SEMA 값을 증가시키고
   SEMA를 기다리는 스레드 중 하나를 깨웁니다(있는 경우).

   이 함수는 인터럽트 핸들러에서 호출될 수 있습니다. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (!list_empty (&sema->waiters)) {
		list_sort(&sema->waiters, priority_sema_cmp, NULL);
		struct thread *T1 = list_entry(list_front(&sema->waiters), struct thread, elem);
		thread_unblock (T1);
	}
	sema->value++;
	intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* 두 스레드 사이에서 제어를 "핑퐁"처럼 하는 세마포어에 대한 자체 테스트입니다.
   printf() 함수를 호출하여 어떤 일이 일어나고 있는지 확인하세요. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* sema_self_test()에서 사용하는 스레드 함수 */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* LOCK을 초기화합니다. 잠금은 주어진 시간에 최대 
   하나의 스레드만 보유할 수 있습니다. 이 잠금은 "재귀적"이지 않습니다. 
   즉, 현재 잠금을 보유한 스레드가 해당 잠금을 획득하려고 시도하면 오류가 발생합니다.

   잠금은 초기값이 1인 세마포어의 특수화입니다. 잠금과 이러한 세마포어의 차이점은 두 가지입니다. 
   첫째, 세마포어는 1보다 큰 값을 가질 수 있지만 잠금은 한 번에 하나의 스레드만 소유할 수 있습니다. 
   둘째, 세마포어에는 소유자가 없습니다. 
   즉, 한 스레드가 세마포어를 "다운"한 다음 다른 스레드가 세마포어를 "업"할 수 있지만, 
   잠금을 사용하면 같은 스레드가 세마포어를 획득하고 해제해야 합니다. 
   이러한 제한이 부담스러울 때는 잠금 대신 세마포어를 사용해야 한다는 좋은 신호입니다. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* LOCK을 획득하고, 필요한 경우 사용 가능해질 때까지 대기합니다.
   현재 스레드에서 잠금을 이미 보유하고 있어서는 안 됩니다.

   이 함수는 휴면 상태가 될 수 있으므로
   인터럽트 핸들러 내에서 호출해서는 안 됩니다. 
   이 함수는 인터럽트가 비활성화된 상태에서 호출될 수 있지만,
   휴면 상태가 되어야 하는 경우 인터럽트가 다시 활성화됩니다. */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	sema_down (&lock->semaphore);
	lock->holder = thread_current ();
}

/* LOCK을 획득하려고 시도하고 성공하면 true를, 
   실패하면 false를 반환합니다.
   현재 스레드에서 잠금을 이미 보유하고 있으면 안 됩니다.

   이 함수는 절전 모드로 전환되지 않으므로
   인터럽트 처리기 내에서 호출될 수 있습니다. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* 현재 스레드가 소유해야 하는 LOCK을 해제합니다.
   이 함수는 lock_release 함수입니다.  

   인터럽트 핸들러는 잠금을 획득할 수 없으므로, 
   인터럽트 핸들러 내에서 잠금을 해제하려고 
   시도하는 것은 의미가 없습니다. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	lock->holder = NULL;
	sema_up (&lock->semaphore);
}

/* 현재 스레드가 LOCK을 보유하고 있으면 true를 반환하고, 
   그렇지 않으면 false를 반환합니다. 
   (다른 스레드가 잠금을 보유하고 있는지 테스트하는 것은 무작위적인 작업입니다.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* 목록에 있는 세마포어 하나 */
struct semaphore_elem {
	struct list_elem elem;              /* 리스트 요소. */
	struct semaphore semaphore;         /* 세마포어. */
};

/* 조건 변수 COND를 초기화합니다. 
   조건 변수는 한 코드에서 조건을 신호로 보내고, 
   다른 코드에서는 신호를 수신하여 그에 따라 동작하도록 합니다. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* LOCK을 원자적으로 해제하고 다른 코드에서 COND가 신호될 때까지 기다립니다. 
   COND가 신호되면 LOCK은 반환되기 전에 다시 획득됩니다. 
   이 함수를 호출하기 전에 LOCK을 유지해야 합니다.

   이 함수가 구현하는 모니터는 "Hoare" 스타일이 아닌 "Mesa" 스타일입니다. 
   즉, 신호를 보내고 받는 것은 원자적 연산이 아닙니다. 
   따라서 일반적으로 호출자는 대기가 완료된 후 조건을 
   다시 확인하고 필요한 경우 재대기해야 합니다.

   주어진 조건 변수는 하나의 잠금에만 연결되지만, 
   하나의 잠금은 여러 개의 조건 변수와 연결될 수 있습니다. 
   즉, 잠금에서 조건 변수로의 일대다 매핑이 ​​존재합니다.

   이 함수는 휴면 상태가 될 수 있으므로
   인터럽트 핸들러 내에서 호출해서는 안 됩니다.
   이 함수는 인터럽트가 비활성화된 상태에서 호출될 수 있지만,
   휴면 상태가 되어야 하는 경우 인터럽트가 다시 활성화됩니다. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	list_push_back (&cond->waiters, &waiter.elem);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* COND(LOCK으로 보호됨)를 기다리는 스레드가 있으면, 
   이 함수는 그 중 하나를 대기 상태에서 깨어나도록 신호를 보냅니다.
   이 함수를 호출하기 전에 LOCK을 유지해야 합니다.

   인터럽트 핸들러는 잠금을 획득할 수 없으므로, 
   인터럽트 핸들러 내에서 조건 변수에 신호를 보내는 것은 의미가 없습니다. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters))
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
}

/* COND(LOCK으로 보호됨)를 대기 중인 모든 스레드를 깨웁니다. (있는 경우)
   이 함수를 호출하기 전에 LOCK을 유지해야 합니다.

   인터럽트 핸들러는 잠금을 획득할 수 없으므로 
   인터럽트 핸들러 내에서 조건 변수에 신호를 보내는 것은 의미가 없습니다. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
