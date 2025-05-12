#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19 // 초당 타이머 인터럽트 발생 빈도
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks; // OS 부팅 후 발생한 총 타이머 틱 수

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick; // 한 번의 타이머 틱 동안 실행될 수 있는 CPU의 반복문 횟수를 나타냄.

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	// 8254PIT를 설정하여 TIMER_FREQ에 정의된 빈도로 인터럽트를 발생시키고, 해당 인터 럽트 핸들러를 등록.

	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */

	// PIT 카운터에 설정할 값을 계산.
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	// PIT의 제어 워드 레지스터에 0x34를 씀.
	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	
	// count 값의 LSB를 카운터 0의 데이터 포트에 씀.
	outb (0x40, count & 0xff);

	// count 값의 MSB를 카운터 0의 데이터 포트에 씀.
	outb (0x40, count >> 8);

	// 인터럽트 벡터에 timer_interrupt 함수를 핸들러로 등록.
	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	// loops_per_tick 변수의 값을 결정.

	unsigned high_bit, test_bit;

	// 인터럽트가 활성화 되어 있어야 함. (타이머 틱 발생에 의존하기 때문.)
	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */

	// 초기 근사값을 1024으로 설정.
	loops_per_tick = 1u << 10;

	// loops_per_tick 값을 두 배씩 늘려가며, too_many_loops 함수를 호출하여 두 배로 늘린 루프 횟수가 한 타이머 틱을 초과하는지 확인.
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */

	// 정교화 과정

	// 찾은 가장 큰 2의 거듭제곱 값을 high_bit에 저장.
	high_bit = loops_per_tick;

	// high_bit 보다 작은 비트들을 하나씩 테스트함.
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)

		// high_bit에 test_bit를 더한 루프 횟수가 한 틱을 초과하지 않으면,
		if (!too_many_loops (high_bit | test_bit))
			// loops_per_tick에 해당 test_bit을 설정함. (더 정확한 loops_per_tick 값을 찾는다.)
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */

// OS 부팅 후 경과된 타이머 틱 수를 반환.
int64_t
timer_ticks (void) {

	// ticks 변수를 읽는 동안 인터럽트로 인해 값이 변경되는 것을 막기 위해 인터럽트를 비활성화함
	enum intr_level old_level = intr_disable ();

	// ticks 값을 지역 변수 t에 복사
	int64_t t = ticks;
	
	// 이전 인터럽트 상태를 복원.	
	intr_set_level (old_level);

	// 컴파일러나 CPU가 메모리 접근 순서를 재배치하지 않도록 하는 메모리 배리어.
	// ticks 값을 읽은 후에 다른 작업이 수행되도록 보장.
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */

// then 이후로 경과된 타이머 틱 수를 계산.
// 여기서 then이란? -> 과거 특정 시점의 timer_ticks() 반환 값.
int64_t
timer_elapsed (int64_t then) {

	// 현재 틱 수에서 과거의 틱 수를 빼서 경과 시간을 계산함.
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. */

// 현재 실행 중인 스레드를 약 ticks_to_sleep 만큼의 타이머 틱 동안 일시 중단 시킴.
// CPU를 다른 스레드에게 양보.
void
timer_sleep (int64_t ticks) {

	// 현재 틱 수를 기록.
	int64_t start = timer_ticks ();

	// 인터럽트 활성화 O -> timer_interrupt가 ticks를 증가시키고, thread_yield()가 제대로 동작.
	ASSERT (intr_get_level () == INTR_ON);

	// 목표한 ticks_to_sleep 만큼의 시간이 경과할 떄까지 thread_yield()룰 호출하여 CPU를 다른 스레드에 양보함.
	// 스케줄러에 의해 다시 실행될 떄마다 조건을 확인함.
	while (timer_elapsed (start) < ticks)
		thread_yield ();
}

/* Suspends execution for approximately MS milliseconds. */

// 현재 실행 중인 스레드를 약 ms 밀리초동안 일시 중단.
void
timer_msleep (int64_t ms) {

	// 함수를 호출하여 ms / 1000 초 동안 대기.
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */

// 현재 실행 중인 스레드를 약 us 마이크로초동안 일시 중단.
void
timer_usleep (int64_t us) {

	// 함수를 호출하여 us / (1000*1000) 초 동안 대기.
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */

// 현재 실행 중인 스레드를 약 ns 나노초동안 일시 중단.
void
timer_nsleep (int64_t ns) {

	// 함수를 호출하여 ns / (1000*1000*1000) 초 동안 대기.
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */

// 타이머 관련 통계 (현재까지의 총 틱 수)를 출력.
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */

// 8254 PIT에 의해 주기적으로 호출되는 인터럽트 핸들러.
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	
	// 전역 틱 카운터를 1 증가.
	ticks++;

	// 스레드 스케줄러 관련 함수를 호출.
	// 이 함수내에서 실행 중인 스레드의 시간 할당량(time slice)을 감소시키고, 
	// 필요한 경우 스레드 전환(context switching)을 수행.
	// timer_sleep 등으로 잠든 스레드를 꺠우는 등의 작업을 수행.
	thread_tick ();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */

// timer_calibrate()에서 사용되고, 주어진 loops의 횟수만큼 busy_wait()를 실행하는데 한 타이머 틱보다 많은 시간이 걸리는지 여부를 반환.
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */

	// 다음 타이머 틱이 발생할 떄까지 대기. 측정을 위한 동기화 단계.
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */

	// 현재 틱 수를 기록하고, loops 횟수만큼 busy_wait()를 실행.
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */

	// 메모리 연산이 완료되었음을 보장.
	barrier ();

	// busy_wait() 실행 중에 틱 카운트(ticks)가 변경되었다면, 루프 실행 시간이 한 틱 이상 걸렸다는 의미이므로, true를 반환.
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */

// 매우 짧은 시간 동안 CPU를 점유하여 대기.
static void NO_INLINE // 이 함수가 인라인되지 안도록 함. 인라인될 경우 코드 위치에 따라 실행 시간이 달라져 예측하기 어려워짐.
busy_wait (int64_t loops) {

	// loops 횟수만큼 단순 반복문을 실행.
	while (loops-- > 0)

		// 컴파일러가 이 루프를 최적화하여 없애버리는 것을 방지하고, 실제로 CPU 사이클을 소모하도록 함.
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */

// 약 num/denom 초 동안 실행을 일시 중단. msleep, usleep, nsleep의 내부 구현에 사용됨.
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */

	// 대기할 시간을 타이머 틱 단위로 변환. (num/denom 초) * (TIMER_FREQ 틱/초) = ticks 틱.
	int64_t ticks = num * TIMER_FREQ / denom;

	// 인터럽트 활성화 상태를 확인.
	ASSERT (intr_get_level () == INTR_ON);

	// 대기해야 할 시간이 한 틱 이상이면,
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */

		// timer_sleep()을 사용하여, CPU를 양보하며 대기.
		timer_sleep (ticks);
	} 
	
	// 대기 시간이 한 틱 미만이면 (매우 짧은 시간),
	// busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	// -> busy_wait()를 사용하여 정밀하게 대기
	else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);

		// loops_per_tick * num * TIMER_FREQ / denom 만큼의 루프 실행.
		// loops_per_tick : 한 틱당 루프 수
		// num * TIMER_FREQ / denom : 대기해야 할 시간을 틱 단위로 나타낸 것.
		// -> 둘을 곱하게 되면 총 실행해야 할 루프 수가 됨.
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));

		// num / 1000 및 denom / 1000은 중간 계산 과정에서의 오버플로 가능성을 줄이기 위한 스케일링.
		// denom이 1000의 배수라고 가정. (밀리초, 마이크로초, 나노초의 경우 denom은 각각 1000, 10의 6승, 10의 9승이므로 성립.)
	}
}
