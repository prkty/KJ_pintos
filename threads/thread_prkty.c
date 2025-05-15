#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 `magic` 멤버에 대한 무작위 값입니다.
   스택 오버플로 감지에 사용됩니다. 자세한 내용은 thread.h 파일의
   맨 위에 있는 큰 주석을 참조하세요. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드에 대한 무작위 값입니다.
   이 값을 수정하지 마세요. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태에 있는 프로세스 목록으로
   실행할 준비가 되었지만 실제로 실행되지 않는 프로세스입니다. */
static struct list ready_list;

/* 유휴 스레드 */
static struct thread *idle_thread;

/* 초기 스레드, init.c:main()을 실행하는 스레드. */
static struct thread *initial_thread;

/* allocate_tid()에서 사용되는 잠금입니다. */
static struct lock tid_lock;

/* 스레드 파괴 요청합니다 */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # 유휴 상태로 소비된 타이머 틱 */
static long long kernel_ticks;  /* # 커널 스레드의 타이머 틱 */
static long long user_ticks;    /* # 사용자 프로그램의 타이머 틱 */

/* Scheduling. */
#define TIME_SLICE 4            /* # 각 스레드에 타이머 틱을 제공합니다. */
static unsigned thread_ticks;   /* # 마지막 yeild 이후 타이머 틱. */

/* false(기본값)인 경우 라운드 로빈 스케줄러를 사용합니다.
   true인 경우 다단계 피드백 큐 스케줄러를 사용합니다.
   커널 명령줄 옵션 "-o mlfqs"로 제어됩니다. */
bool thread_mlfqs;

//////////////////////////////// 추가된 우선순위 비교
bool priority_cmp (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* T가 유효한 스레드를 가리키는 것으로 나타나면 true를 반환합니다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 실행 중인 스레드를 반환합니다.
 * CPU의 스택 포인터 `rsp'를 읽고, and then round that
 * 페이지의 시작 부분으로 내림합니다. `struct thread'는
 * 항상 페이지의 시작 부분에 있고 스택 포인터는
 * 중간 어딘가에 있으므로, 이 함수는 현재 스레드를 찾습니다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start에 대한 전역 설명자 테이블입니다.
// gdt는 thread_init 이후에 설정되므로
// 임시 GDT를 먼저 설정해야 합니다.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* 현재 실행 중인 코드를 스레드로 변환하여 스레딩 시스템을 초기화합니다.
이 방법은 일반적으로는 작동하지 않으며, 이 경우에는 loader.S가 스택 하단을 페이지 경계에 배치했기 때문에 가능합니다.

실행 큐와 tid 잠금도 초기화합니다.

이 함수를 호출한 후에는 thread_create()를 사용하여 스레드를 생성하기 전에 페이지 할당자를 초기화해야 합니다.

이 함수가 완료될 때까지 thread_current()를 호출하는 것은 안전하지 않습니다. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널의 임시 GDT를 다시 로드합니다.
	* 이 GDT에는 사용자 컨텍스트가 포함되지 않습니다.
	* 커널은 gdt_init()에서 사용자 컨텍스트를 사용하여 GDT를 다시 빌드합니다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* globla thread context를 초기화합니다 */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);

	/* 실행 중인 스레드에 대한 스레드 구조를 설정합니다. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작합니다.
   그리고 유휴 스레드를 생성합니다. */
void
thread_start (void) {
	/* 유휴 스레드를 생성합니다. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* 선점형 스레드 스케줄링 시작합니다. */
	intr_enable ();

	/* 유휴 스레드가 idle_thread를 초기화할 때까지 기다립니다. */
	sema_down (&idle_started);
}

/* 각 타이머 틱에서 타이머 인터럽트 핸들러에 의해 호출됩니다.
   따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* 선점 시행. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* 스레드 통계를 인쇄합니다. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 주어진 초기 PRIORITY를 사용하여 NAME이라는 이름의 새 커널 스레드를 생성합니다. 
   이 스레드는 AUX를 인수로 전달하여 FUNCTION을 실행하고 준비 큐에 추가합니다. 
   새 스레드의 스레드 식별자를 반환하거나, 생성에 실패하면 TID_ERROR를 반환합니다.

   thread_start()가 호출된 경우, 새 스레드는
   thread_create()가 반환되기 전에 스케줄링될 수 있습니다. 
   심지어 thread_create()가 반환되기 전에 종료될 수도 있습니다. 
   반대로, 원래 스레드는 새 스레드가 스케줄링되기 전까지 얼마든지 실행될 수 있습니다.
   순서를 보장해야 하는 경우 세마포어나 다른 형태의 동기화를 사용하세요.

   제공된 코드는 새 스레드의 'priority' 멤버를 PRIORITY로 설정하지만,
   실제 우선순위 스케줄링은 구현되지 않았습니다.
   우선순위 스케줄링은 문제 1-3의 목표입니다. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* 스레드 할당합니다. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 스레드를 초기화합니다. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스케줄된 경우 kernel_thread를 호출합니다.
	 * 참고) rdi는 첫 번째 인수이고, rsi는 두 번째 인수입니다. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* 실행 대기열에 추가합니다. */
	thread_unblock (t);

	struct thread *T1 = list_entry(list_front(&ready_list), struct thread, elem);
	enum intr_level old_level;
	old_level = intr_disable ();   // 인터럽트를 비활성화하고 이전 인터럽트 상태를 반환합니다.
	if ((thread_current() != idle_thread) && (thread_current() -> priority < T1 -> priority))
	thread_yield (); // 우선순위를 바꿨다면, 우선순위에 따라 yield 해줘야한다.
	intr_set_level (old_level);   // 인터럽트 다시 받게 재세팅

	return tid;
}

/* 현재 스레드를 휴면 상태로 전환합니다. thread_unblock()에 의해
   깨어날 때까지 다시 예약되지 않습니다.

   이 함수는 인터럽트를 끈 상태에서 호출해야 합니다.
   일반적으로 synch.h에 있는 동기화 기본 요소 중 하나를
   사용하는 것이 더 좋습니다. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 차단된 스레드 T를 실행 준비 상태로 전환합니다.
   T가 차단되지 않은 경우 오류가 발생합니다.
   (thread_yield()를 사용하여 실행 중인 스레드를 준비 상태로 만듭니다.)

   이 함수는 실행 중인 스레드를 선점하지 않습니다.
   이는 중요할 수 있습니다. 호출자가 인터럽트를 직접 비활성화한 경우,
   스레드 차단을 원자적으로 해제하고 다른 데이터를 
   업데이트할 수 있을 것으로 예상할 수 있습니다. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_insert_ordered (&ready_list, &t->elem, priority_cmp, NULL);    //////////////////////////////
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* 실행 중인 스레드의 이름을 반환합니다. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 실행 중인 스레드를 반환합니다.
   이것은 running_thread()와 몇 가지 점검입니다.
   자세한 내용은 thread.h 상단의 큰 댓글을 참조하세요. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 실제로 스레드인지 확인하세요.
	   이러한 assertions fire 중 하나라도 발생하면 
	   스레드가 스택 오버플로를 발생시켰을 수 있습니다. 
	   각 스레드는 4KB 미만의 스택을 가지고 있으므로, 
	   몇 개의 큰 자동 배열이나 적당한 재귀 호출만으로도 
	   스택 오버플로가 발생할 수 있습니다. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 실행 중인 스레드의 tid를 반환합니다. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드의 스케줄을 취소하고 소멸시킵니다. 
   호출자에게 반환하지 않습니다. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 상태를 죽음으로 설정하고 다른 프로세스를 스케줄링합니다.
	   schedule_tail() 호출 중에 프로세스가 종료됩니다. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

//////////////////////////////
// 스레드 스트럭쳐 내부의 인자 가져옴(priority가져와야함)
bool priority_cmp (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *T_A = list_entry(a, struct thread, elem);
	struct thread *T_B = list_entry(b, struct thread, elem);

	return T_A -> priority > T_B -> priority;
}

/* CPU를 양보합니다. 현재 스레드가 sleep 모드로 전환되지 않았습니다
   스케줄러의 요청에 따라 즉시 다시 사용할 수 있습니다. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());     // 외부 인터럽트 처리 중일때 함수 탈출(스레드 실행중)

	old_level = intr_disable ();   // 인터럽트를 비활성화하고 이전 인터럽트 상태를 반환합니다.
	if (curr != idle_thread)
		list_insert_ordered (&ready_list, &curr->elem, priority_cmp, NULL);   // LIST의 끝에 ELEM을 삽입하여 LIST의 뒤에 위치하도록 합니다.
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}
// 현재 yield의 방식이 현재 들어온 스레드를 바로 준비 리스트로 옮기니까, priority 확인해서
// 현재 스레드보다 높다면, yield 시키면 되지 않나 싶긴하다 일단 list_insert_ordered을 통해 구현해봐야겠다.
//////////////////////////////

//////////////////////////////
/* 현재 실행 중인 스레드의 우선순위를 동적으로 변경합니다. */
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
	if (list_empty(&ready_list))  return;   // 리스트가 비었을때 그냥 종료
	struct thread *T1 = list_entry(list_front(&ready_list), struct thread, elem);
	enum intr_level old_level;

	old_level = intr_disable ();   // 인터럽트를 비활성화하고 이전 인터럽트 상태를 반환합니다.
	if ((thread_current() != idle_thread) && (thread_current() -> priority < T1 -> priority))
	thread_yield (); // 우선순위를 바꿨다면, 우선순위에 따라 yield 해줘야한다.
	intr_set_level (old_level);   // 인터럽트 다시 받게 재세팅
}
//////////////////////////////

/* 현재 스레드의 우선순위를 반환합니다. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정합니다. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: 구현은 여기에 있습니다 */
}

/* 현재 스레드의 nice 값을 반환합니다. */
int
thread_get_nice (void) {
	/* TODO: 구현은 여기에 있습니다 */
	return 0;
}

/* 시스템 부하 평균의 100배를 반환합니다. */
int
thread_get_load_avg (void) {
	/* TODO: 구현은 여기에 있습니다 */
	return 0;
}

/* 현재 스레드의 최근 CPU 값의 100배를 반환합니다.. */
int
thread_get_recent_cpu (void) {
	/* TODO: 구현은 여기에 있습니다 */
	return 0;
}

/* 유휴 스레드. 다른 스레드가 실행할 준비가 되지 않았을 때 실행됩니다.

   유휴 스레드는 처음에 thread_start()에 의해 준비 목록에 추가됩니다. 
   이 스레드는 처음에 한 번 스케줄링되며, 이때 idle_thread를 초기화하고, 
   전달된 세마포어를 "활성화"하여 thread_start()가 계속 실행될 수 있도록 한 후 
   즉시 차단됩니다. 그 이후로 유휴 스레드는 준비 목록에 더 이상 표시되지 않습니다. 
   준비 목록이 비어 있을 때 next_thread_to_run()에 의해 특별한 경우로 반환됩니다. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 다시 활성화하고 다음 인터럽트를 기다립니다.

		   `sti` 명령어는 다음 명령어가 완료될 때까지 인터럽트를 
		   비활성화하므로 이 두 명령어는 원자적으로 실행됩니다. 
		   이 원자성은 중요합니다. 그렇지 않으면 인터럽트를 다시 
		   활성화하고 다음 인터럽트가 발생할 때까지 기다리는 사이에 
		   인터럽트가 처리되어 최대 한 클럭 틱만큼의 시간을 낭비하게 될 수 있습니다.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드의 기반으로 사용되는 함수입니다. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트가 꺼진 상태에서 실행됩니다. */
	function (aux);       /* 스레드 함수를 실행합니다. */
	thread_exit ();       /* function()이 반환되면 스레드를 종료합니다. */
}


/* T를 NAME이라는 차단된 스레드로 기본 초기화합니다. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
}

/* 스케줄링할 다음 스레드를 선택하고 반환합니다. 
   실행 대기열이 비어 있지 않은 한, 실행 대기열에서 스레드를 반환해야 합니다. 
   (실행 중인 스레드가 계속 실행될 수 있다면 실행 대기열에 있을 것입니다.) 
   실행 대기열이 비어 있으면, idle_thread를 반환합니다. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* iretq를 사용하여 스레드를 시작하세요 */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* 새 스레드의 페이지 테이블을 활성화하여 스레드를 전환하고, 
   이전 스레드가 죽어가고 있다면 이를 삭제합니다.

   이 함수를 호출할 때, 우리는 방금 PREV 스레드에서 전환했고, 
   새로운 스레드는 이미 실행 중이며, 인터럽트는 여전히 비활성화되어 있습니다.

   스레드 전환이 완료될 때까지 printf()를 호출하는 것은 안전하지 않습니다. 
   실제로는 printf()를 함수 끝에 추가해야 합니다. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* 주요 전환 로직.
	* 먼저 전체 실행 컨텍스트를 intr_frame으로 복원합니다.
	* 그런 다음 do_iret을 호출하여 다음 스레드로 전환합니다.
	* 전환이 완료될 때까지 여기에서 어떤 스택도 사용해서는 안 됩니다.
	* */
	__asm __volatile (
			/* 사용될 저장 레지스터입니다. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* 입력을 한 번 가져옵니다 */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새 프로세스를 예약합니다. 시작할 때는 인터럽트가 꺼져 있어야 합니다.
 * 이 함수는 현재 스레드의 상태를 상태로 수정한 다음
 * 실행할 다른 스레드를 찾아서 전환합니다.
 * 스케줄에서 printf()라고 부르는 것은 안전하지 않습니다. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* 전환한 스레드가 종료되면 해당 구조체 스레드를 삭제합니다. 
		이 작업은 thread_exit()가 스스로를 종료하지 않도록 늦게 수행해야 합니다.
		페이지가 현재 스택에서 사용 중이므로 페이지 비우기 요청을 여기에 대기열에 넣습니다.
		실제 삭제 로직은 schedule() 시작 부분에서 호출됩니다.. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* 스레드를 전환하기 전에 먼저 현재 실행 중인 정보를 저장합니다. */
		thread_launch (next);
	}
}

/* 새 스레드에 사용할 tid를 반환합니다. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
