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

/* 프로토타입 함수 */
bool cond_priority_comp(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);

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
		list_insert_ordered (&sema->waiters, &thread_current ()->elem, priority_comp, NULL);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

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
	if (!list_empty (&sema->waiters)){
		list_sort(&sema->waiters, priority_comp, NULL);
		thread_unblock (list_entry (list_pop_front (&sema->waiters),
					struct thread, elem));
	}
	sema->value++;
	thread_check_preempt();
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

/* Thread function used by sema_self_test(). */
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
	enum intr_level old = intr_disable();
	struct thread *curr = thread_current();

	if(lock -> holder != NULL && lock -> holder -> priority < thread_current() -> priority){
		curr -> waiting_lock = lock;
		donate_priority();
	}
	intr_set_level(old);

	sema_down (&lock->semaphore);
	curr -> waiting_lock = NULL;
	lock->holder = thread_current ();

}

void donate_priority(void){
	enum intr_level old = intr_disable();
	int depth = 0;
	struct thread *donor = thread_current(); 
	while(depth < 8){
		if(!donor -> waiting_lock) break;
		struct thread *receiver = donor -> waiting_lock -> holder;
		receiver -> priority = donor -> priority;
		donor = receiver;
		depth++;
	}
	intr_set_level(old);
}

/*----------------------------------------*/

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
	lock-> holder->priority = lock-> holder->original_priority;
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
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
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
	list_insert_ordered(&cond->waiters, &waiter.elem, cond_priority_comp, NULL);
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

	if (!list_empty (&cond->waiters)){
		list_sort(&cond->waiters, cond_priority_comp, NULL);
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
	}
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

/* 세마포어 비교함수 */
bool cond_priority_comp(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
	struct semaphore_elem *SE_A = list_entry(a, struct semaphore_elem, elem);
	struct semaphore_elem *SE_B = list_entry(b, struct semaphore_elem, elem);
	
	struct list *waiter_l_sema = &(SE_A->semaphore.waiters);
	struct list *waiter_s_sema = &(SE_B->semaphore.waiters);

	return list_entry (list_begin (waiter_l_sema), struct thread, elem)->priority
		 > list_entry (list_begin (waiter_s_sema), struct thread, elem)->priority;
}

/*
	cond에서 세마포어는 단일 쓰레드만 웨이팅리스트에 가지고 있다.
	빵집으로 비유를 한다면 빵(공유자원)을 빵집에서 가져가고싶은 상황에서
	단일스레드가 입장권(세마포어)를 가지고 빵집에 출입하는 상황인것이다.
	때문에 여기서 비교함수는 쓰레드의 우선순위를 이용해서 세마포어를 줄세워 공유자원에 차례대로 접근하기 위해 사용한다.
*/