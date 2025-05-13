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

#if TIMER_FREQ < 19 // 초당 타이머 인터럽트 발생 빈도
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* OS 부팅 이후 타이머 틱 수. (1초에 100번, 10ms)*/
static int64_t ticks;

/* 웨이팅 리스트 작성*/
static struct list waiting_list;

/* 타이머 틱당 루프 수입니다.
   timer_calibrate()로 초기화됐습니다. */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void timer_wakeup (int64_t ticks);
static bool wakeup_cmp (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
/* 8254 프로그래머블 인터벌 타이머(PIT)를 설정하여
   초당 PIT_FREQ 횟수를 인터럽트하고
   해당 인터럽트를 등록합니다. */
void
timer_init (void) {
	/* 8254 입력 주파수를 TIMER_FREQ로 나누면, 
	   가장 가까운 쪽으로 반올림됩니다. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	
	// count 값의 LSB를 카운터 0의 데이터 포트에 씀.
	outb (0x40, count & 0xff);

	// count 값의 MSB를 카운터 0의 데이터 포트에 씀.
	outb (0x40, count >> 8);

	/* 웨이팅 리스트 초기화 */
	list_init (&waiting_list);
	intr_register_ext (0x20, timer_interrupt, "8254 Timer");  // 중요! 틱마다 타이머 인터럽트를 실행
}

/* 짧은 지연을 구현하는 데 사용되는 loops_per_tick을 보정합니다. */
void
timer_calibrate (void) {
	// loops_per_tick 변수의 값을 결정.

	unsigned high_bit, test_bit;

	// 인터럽트가 활성화 되어 있어야 함. (타이머 틱 발생에 의존하기 때문.)
	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* loops_per_tick을 타이머 틱 하나보다 작은
	   2의 최대 거듭제곱으로 근사합니다. */
	loops_per_tick = 1u << 10;

	// loops_per_tick 값을 두 배씩 늘려가며, too_many_loops 함수를 호출하여 두 배로 늘린 루프 횟수가 한 타이머 틱을 초과하는지 확인.
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}
	// 정교화 과정
	// 찾은 가장 큰 2의 거듭제곱 값을 high_bit에 저장.
	/* 다음 8비트 루프_per_tick을 정제합니다. */
	high_bit = loops_per_tick;

	// high_bit 보다 작은 비트들을 하나씩 테스트함.
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)

		// high_bit에 test_bit를 더한 루프 횟수가 한 틱을 초과하지 않으면,
		if (!too_many_loops (high_bit | test_bit))
			// loops_per_tick에 해당 test_bit을 설정함. (더 정확한 loops_per_tick 값을 찾는다.)
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}
/* OS 부팅 이후 타이머 틱 수를 반환합니다. */
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
// 여기서 then이란? -> 과거 특정 시점의 timer_ticks() 반환 값.
/* 특정 시점 이후부터 발생한 타이머 틱 횟수를 반환한다. */
int64_t
timer_elapsed (int64_t then) {

	// 현재 틱 수에서 과거의 틱 수를 빼서 경과 시간을 계산함.
	return timer_ticks () - then;
}


static bool wakeup_cmp (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *T_A = list_entry(a, struct thread, elem);
	struct thread *T_B = list_entry(b, struct thread, elem);
	
	if(T_A -> wakeup == T_B -> wakeup)
		return T_A -> priority > T_B -> priority;
	return T_A -> wakeup < T_B -> wakeup;
}

/* 대략적인 TICKS인 타이머 틱에 대한 실행을 일시 중지합니다. */
// 해당 코드의 문제점은 바쁜 대기로 리소스를 잡아 먹는다는 점이다.
// 해결을 위해서는 스레드 호출전까지 또는 일정 타이머 전까지 sleep 해야한다.
// 현재 실행 중인 스레드를 약 ticks_to_sleep 만큼의 타이머 틱 동안 일시 중단 시킴.
// CPU를 다른 스레드에게 양보.
void
timer_sleep (int64_t ticks) {
	struct thread *curr = thread_current();
	curr -> start = timer_ticks();
	curr -> wakeup = timer_ticks() + ticks;

	ASSERT (intr_get_level () == INTR_ON);   // [오류] 현재 인터럽트가 ON이라면 함수 진행

	enum intr_level old_level = intr_disable ();
	list_insert_ordered(&waiting_list, &(curr -> elem), wakeup_cmp, NULL);
	thread_block();
	intr_set_level (old_level);
}

/* 약 MS 밀리초 동안 실행을 일시 중지합니다. */
void
timer_msleep (int64_t ms) {

	// 함수를 호출하여 ms / 1000 초 동안 대기.
	real_time_sleep (ms, 1000);
}
// 현재 실행 중인 스레드를 약 us 마이크로초동안 일시 중단.
/* 약 US 마이크로초 동안 실행을 일시 중지합니다. */
void
timer_usleep (int64_t us) {

	// 함수를 호출하여 us / (1000*1000) 초 동안 대기.
	real_time_sleep (us, 1000 * 1000);
}

/* 약 NS 마이크로초 동안 실행을 일시 중지합니다. */
void
timer_nsleep (int64_t ns) {

	// 함수를 호출하여 ns / (1000*1000*1000) 초 동안 대기.
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

// 타이머 관련 통계 (현재까지의 총 틱 수)를 출력.
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}
/* 타이머 인터럽트 핸들러. */   
// 아마 여기서 틱 단위로 확인하여 sleep list에서 
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	// 전역 틱 카운터를 1 증가.
	ticks++;

	// 스레드 스케줄러 관련 함수를 호출.
	// 이 함수내에서 실행 중인 스레드의 시간 할당량(time slice)을 감소시키고, 
	// 필요한 경우 스레드 전환(context switching)을 수행.
	// timer_sleep 등으로 잠든 스레드를 꺠우는 등의 작업을 수행.
	timer_wakeup(ticks);
	thread_tick();

	// 1. 슬립 리스트의 맨 앞 스레드의 wakeup_tick 확인
	// 2. 현재 ticks 값 이상이면 리스트에서 제거하고 thread_unblock()으로 깨움
	// 3. 리스트가 정렬되어 있으므로, 첫 번째 스레드가 아직 깨어날 시간이 아니면 종료

}

static void timer_wakeup (int64_t ticks){
	while(!list_empty(&waiting_list)){
		struct thread *next = list_entry (list_front (&waiting_list), struct thread, elem);
		if(ticks < next -> wakeup)
			break;
		list_pop_front(&waiting_list);
		thread_unblock(next);
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
	/* Run LOOPS loops. */

	// 현재 틱 수를 기록하고, loops 횟수만큼 busy_wait()를 실행.
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */

	// 메모리 연산이 완료되었음을 보장.
	/* LOOPS라는 루프를 실행합니다. */
	start = ticks;
	busy_wait (loops);

	/* 틱 카운트를 바꾼다면, 너무 오래 반복할 것입니다. */
	barrier ();

	// busy_wait() 실행 중에 틱 카운트(ticks)가 변경되었다면, 루프 실행 시간이 한 틱 이상 걸렸다는 의미이므로, true를 반환.
	return start != ticks;
}

/* 간단한 루프 루프를 반복하여 짧은 지연을 구현합니다.
   코드 정렬이 타이밍에 큰 영향을 미칠 수 있기 때문에
   NO_INLINE으로 표시되었습니다, 따라서 이 함수가 다른 위치에서 다르게 내부 처리되면
   결과를 예측하기 어려울 것입니다. */

// 매우 짧은 시간 동안 CPU를 점유하여 대기.
static void NO_INLINE // 이 함수가 인라인되지 안도록 함. 인라인될 경우 코드 위치에 따라 실행 시간이 달라져 예측하기 어려워짐.
busy_wait (int64_t loops) {

	// loops 횟수만큼 단순 반복문을 실행.
	while (loops-- > 0)

		// 컴파일러가 이 루프를 최적화하여 없애버리는 것을 방지하고, 실제로 CPU 사이클을 소모하도록 함.
		barrier ();
}

// 약 num/denom 초 동안 실행을 일시 중단. msleep, usleep, nsleep의 내부 구현에 사용됨.
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* NUM/DENOM 초를 타이머 틱으로 변환하고 반올림합니다.

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
		// timer_sleep()을 사용하여, CPU를 양보하며 대기.
		timer_sleep (ticks);
	} 
	
	// 대기 시간이 한 틱 미만이면 (매우 짧은 시간),
	// busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	// -> busy_wait()를 사용하여 정밀하게 대기
	else {
		/* 그렇지 않으면 보다 정확한 서브 틱 타이밍을 위해 대기 대기 루프를 사용합니다.
		   오버플로우 가능성을 피하기 위해 분자와 분모를 1000으로 축소합니다 */
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

// 수정본