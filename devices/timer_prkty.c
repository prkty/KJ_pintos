#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* 8254 타이머 칩의 하드웨어 세부 사항은 [8254]를 참조하세요. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* OS 부팅 이후 타이머 틱 수. (1초에 100번, 10ms)*/
static int64_t ticks;

/* 타이머 틱당 루프 수입니다.
   timer_calibrate()로 초기화됐습니다. */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

static struct list sleep_list;   // 잠재울 리스트 선언

/* 8254 프로그래머블 인터벌 타이머(PIT)를 설정하여
   초당 PIT_FREQ 횟수를 인터럽트하고
   해당 인터럽트를 등록합니다. */
void
timer_init (void) {
	/* 8254 입력 주파수를 TIMER_FREQ로 나누면, 
	   가장 가까운 쪽으로 반올림됩니다. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");  // 중요! 틱마다 타이머 인터럽트를 실행
	list_init(&sleep_list);    ////
}

/* 짧은 지연을 구현하는 데 사용되는 loops_per_tick을 보정합니다. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* loops_per_tick을 타이머 틱 하나보다 작은
	   2의 최대 거듭제곱으로 근사합니다. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* 다음 8비트 루프_per_tick을 정제합니다. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* OS 부팅 이후 타이머 틱 수를 반환합니다. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* timer_ticks()가 반환된 후 
   경과한 타이머 틱 수를 반환합니다. */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}


bool wakeup_cmp (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *T_A = list_entry(a, struct thread, elem);
	struct thread *T_B = list_entry(b, struct thread, elem);
	return T_A -> wakeup < T_B -> wakeup;
}

/* 대략적인 TICKS인 타이머 틱에 대한 실행을 일시 중지합니다. */
// 해당 코드의 문제점은 바쁜 대기로 리소스를 잡아 먹는다는 점이다.
// 해결을 위해서는 스레드 호출전까지 또는 일정 타이머 전까지 sleep 해야한다.
void
timer_sleep (int64_t ticks) {
	struct thread *curr = thread_current();
	curr -> start = timer_ticks();
	curr -> wakeup = timer_ticks() + ticks;

	ASSERT (intr_get_level () == INTR_ON);   // [오류] 현재 인터럽트가 ON이라면 함수 진행

	enum intr_level old_level = intr_disable ();
	list_insert_ordered(&sleep_list, &(curr -> elem), wakeup_cmp, NULL);
	thread_block();
	intr_set_level (old_level);
}




/* 약 MS 밀리초 동안 실행을 일시 중지합니다. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* 약 US 마이크로초 동안 실행을 일시 중지합니다. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* 약 NS 마이크로초 동안 실행을 일시 중지합니다. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* 타이머 통계 출력. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* 타이머 인터럽트 핸들러. */   // 아마 여기서 틱 단위로 확인하여 sleep list에서 
static void
timer_interrupt (struct intr_frame *args UNUSED) {

	ticks++;
	thread_tick ();

	while(!(list_empty(&sleep_list))) {
	struct thread *T1 = list_entry(list_front(&sleep_list), struct thread, elem);

	if (T1 -> wakeup <= ticks) {
	list_pop_front(&sleep_list);
    thread_unblock(T1);
	}
	else
		break;
	}
}

/* 루프 반복이 두 개 이상의 타이머 틱을 기다리는 경우 true를 반환하고, 
   그렇지 않으면 false를 반환합니다. */
static bool
too_many_loops (unsigned loops) {
	/* 타이머 틱을 기다립니다. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* LOOPS라는 루프를 실행합니다. */
	start = ticks;
	busy_wait (loops);

	/* 틱 카운트를 바꾼다면, 너무 오래 반복할 것입니다. */
	barrier ();
	return start != ticks;
}

/* 간단한 루프 루프를 반복하여 짧은 지연을 구현합니다.

   코드 정렬이 타이밍에 큰 영향을 미칠 수 있기 때문에
   NO_INLINE으로 표시되었습니다, 따라서 이 함수가 다른 위치에서 다르게 내부 처리되면
   결과를 예측하기 어려울 것입니다. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* 약 NUM/DENOM 초 동안 sleep 합니다. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* NUM/DENOM 초를 타이머 틱으로 변환하고 반올림합니다.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* 우리는 최소한 하나의 전체 타이머 틱을 기다리고 있습니다.
		   timer_sleep()을 사용하면 CPU가 다른 프로세스에 노출될 수 있습니다 */
		timer_sleep (ticks);
	} else {
		/* 그렇지 않으면 보다 정확한 서브 틱 타이밍을 위해 대기 대기 루프를 사용합니다.
		   오버플로우 가능성을 피하기 위해 분자와 분모를 1000으로 축소합니다 */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}

// 수정본