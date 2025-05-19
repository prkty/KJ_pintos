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

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b // 스레드 구조체의 유효성 검사 및 스태 오버플로우 감지를 위한 매직 넘버

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210 // 스레드 구조체의 유효성 검사 및 스태 오버플로우 감지를 위한 매직 넘버

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list; // 실행 준비가 되었지만 현재 실행 중이 아닌 스레드들(THREAD_READY 상태)를 관리하는 리스트.

/* Idle thread. */
static struct thread *idle_thread; // 실행할 다른 스레드가 없을 때 실행되는 특수한 idle 스레드를 가리키는 포인터.

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread; // OS가 부팅될 때 init.c:main() 함수를 실행하는 초기 스레드를 가리키는 포인터.

/* Lock used by allocate_tid(). */
static struct lock tid_lock; // 스레드 ID(tid)를 할당할 때 경쟁 상태를 방지하기 위한 락.

/* Thread destruction requests */
static struct list destruction_req; // 파괴가 요청된 스레드들을 잠시 보관하는 리스트. 스케줄링 과정에서 실제 메모리 해제가 이루어짐.

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */
// -> 각각 idle 스레드, 커널 스레드, 사용자 프로그램에서 소비된 타이머 틱 수를 저장하는 통계 변수.

/* Scheduling. */
// 각 스레드에게 주어지는 기본 실행 시간(타이머 틱 단위). 라운드 로빈 스케줄링에 사용됨.
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */

// 마지막으로 CPU를 양보한 후 현재 스레드가 사용한 타이머 틱 수.
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */

// 멀티레벨 피드백 큐 스케줄러 사용 여부를 나타내는 불리언 값. 커널 명령행 옵션으로 제어됨.
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.

// 임시 전역 디스크립터 테이블. 스레드 시스템 초기화 시 사용됨.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */

// 스레딩 시스템을 초기화. 현재 실행 중인 코드를 첫 번째 스레드(initial)thread)로 변환, 준비 큐 및 TID 할당에 필요한 락 등을 초기화.
void
thread_init (void) {

	// 인터럽트가 비활성화된 상태에서 호출되어야 함을 확인.
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */

	// 임시 GDT를 로드함. 
	// 커널 세그먼크만 포함하며, 나중에 gdt_init()에서 사용자 컨텍스트를 포함하여 재구성됨.
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock); // TID 할당용 락 초기화
	list_init (&ready_list); // 준비 큐 초기화
	list_init (&destruction_req); // 파괴 요청 큐 초기화

	/* Set up a thread structure for the running thread. */

	// running_thread() 매크로를 사용해 현재 실행 중인 스택을 기반으로 initial_thread 구조체의 위치를 찾음.
	initial_thread = running_thread ();
	
	// init_thread()를 호출하여 initial_thread를 main이라는 이름과 기본 우선순위로 초기화.
	init_thread (initial_thread, "main", PRI_DEFAULT);

	// initial_thread의 상태를 THREAD_RUNNING으로 설정하고, allocate_tid()를 호출하여 TID를 할당.
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */

// 선점형 스레드 스케줄링을 시작하고, idle 스레드를 생성.
void
thread_start (void) {
	/* Create the idle thread. */

	// idle 스레드를 생성. idle 스레드는 가장 낮은 우선순위(PRI_MIN)를 가지며, idle() 함수를 실행.

	// idle_started 세마포어는 idle 스레드가 초기화를 완료하고 idle_thread 전역 변수를 설정했음을 동기화하는 데 사용됨.
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	
	// 인터럽트를 활성화하여 선점형 스케쥴링을 시작. 타이머 인터럽트가 발생하면 스케줄링이 일어날 수 있음.
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */

	// idle 스레드가 idle_thread 변수를 초기화하고 세마포어를 up할 때까지 대기.
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */

// 매 타이머 인터럽트마다 타이머 인터럽트 핸들러에 의해 호출됨. 스레드 실행 통계를 업데이트하고, 필요한 경우 선점을 강제.
void
thread_tick (void) {

	// 현재 실행 중인 스레드 t를 가져옴.
	struct thread *t = thread_current ();

	/* Update statistics. */

	// t가 idle_thread이면 idle_ticks 증가.
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG

	// 사용자 프로세스 페이지 테이블이 존재하면 user_ticks 증가
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	
	// thread_ticks가 TIME_SLICE 이상이 되면, intr_yield_on_return()을 호출.
	// 인터럽트 핸들러가 반활될 때 현재 스레드가 CPU를 양보(yield)하도록 스케줄러에게 요청하는 플래그를 설정.
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */

// 스레드 실행 통계를 출력.
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */

// name이라는 이름과 priorityt 우선순위를 갖는 새로운 커널 스레드를 생성. 이 스레드는 function 함수를 aux 인자와 함께 실행하며, 생성된 스레드는 준비 큐에 추가됨.
// 성공 시 새 스레드의 TID, 실패시 TIP_ERROR를 반환.
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	// 실행할 함수가 NULL이 아님을 확인.
	ASSERT (function != NULL);

	/* Allocate thread. */

	// 스레드 구조체와 스택으로 사용할 페이지를 할당받고 0으로 초기화. 실패 시 TID_ERROR 반환.
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	
	// 할당된 스레드 구조체 t를 name, priority로 기본 초기화. (status는 THREAD_BLOCKED로 설정.)
	init_thread (t, name, priority);

	// 새 스레드의 TID를 할당하고 t->tid에 저장.
	tid = t->tid = allocate_tid ();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */

	// 스레드의 초기 실행 컨텍스트 설정 (t->tif, 인터럽트 프레임)
	t->tf.rip = (uintptr_t) kernel_thread; // Kernel_thread 함수의 주소로 설정 (새 스레드가 처음 실행할 함수.)
	
	// kernel_thread에 전달될 인자들, 즉 function과 aux로 설정
	// x86-64 호출 규약에 따라 첫 번째 인자는 RDI, 두 번째는 RSI 레지스터 사용.
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;

	// 세그먼트 레지스터(ds, es, ss, cs를 커널 세그먼트 값으로 설정.
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;

	// FLAG_IF (인터럽트 활성화 플래그)를 설정.
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */

	// 스레드 t의 상태를 THREAD_READY로 변경하고 준비 큐(ready_list)에 추가.
	thread_unblock (t);

	// 할당된 tid를 반환.
	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */

// 현재 실행 중인 스레드를 잠들게 함.(blocked 상태로 만듦.) thread_unblocked()에 의해 깨어날 때까지 다시 스케줄 되지 않음.
void
thread_block (void) {

	// 인터럽트 컨텍스트에서 호출되지 않았음을 확인함. (일반 스레드 컨텍스트에서만 호출 가능.)
	ASSERT (!intr_context ());

	// 인터럽트가 비활성화된 상태에서 호출되어야 함을 확인. (경쟁 상태 방지.)
	ASSERT (intr_get_level () == INTR_OFF);

	// 현재 스레드의 상태를 THREAD_BLOCKED로 설정.
	thread_current ()->status = THREAD_BLOCKED;

	// 다른 스레드를 실행하도록 스케줄러를 호출.
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */

// 블록된 스레드 t를 실행 준비 상태(THREAD_READY)로 전환하고 준비 큐에 추가.
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	// t가 유효한 스레드인지 확인.
	ASSERT (is_thread (t));

    // 경쟁 상태를 방지하기 위해 인터럽트를 비활성화하고 이전 인터럽트 상태를 저장.
	old_level = intr_disable ();

	// 스레드 t가 실제로 블록된 상태인지 확인.
	ASSERT (t->status == THREAD_BLOCKED);

	// 스레드 t를 준비 큐의 맨 뒤에 추가.
	list_push_back (&ready_list, &t->elem);

	// 스레드 t의 상태를 THREAD_READY로 변경함.
	t->status = THREAD_READY;

	// 이전 인터럽트 상태를 복원.
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */

// 현재 실행 중인 스레드의 이름을 반환.
const char *
thread_name (void) {

	// 이름을 반환.
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */

// 현재 CPU에서 실행 중인 스레드의 struct thread 포인터를 반환.
struct thread *
thread_current (void) {

	// 매크로를 호출하여 현재 스택 포인터를 기준으로 스레드 구조체의 시작 주소를 계산하여 t에 저장.
	// struct thread는 항상 페이지의 시작 부분에 위치하고, 스택 포인터는 그 페이지 내 어딘가를 가리킨다는 점을 이용.
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */

	// t가 유효한 스레드인지 (매직 넘버 확인) 검사.
	ASSERT (is_thread (t));

	// t의 상태가 THREAD_RUNNING인지 검사.
	ASSERT (t->status == THREAD_RUNNING);

	// t를 반환.
	return t;
}

/* Returns the running thread's tid. */

// 현재 실행 중인 스레드의 TID(Thread Identifier)를 반환.
tid_t
thread_tid (void) {

	// TID 반환
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */

// 현재 실행 중인 스레드를 종료. 이 함수는 호출자에게 절대 반환하지 않음.
void
thread_exit (void) {

	// 인터럽트 컨텍스트에서 호출되지 않았음을 확인.
	ASSERT (!intr_context ());

// 사용자 프로그램 지원 시, 관련된 프로세스 종료 처리를 수행.
#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */

	// 인터럽트 비활성화.
	intr_disable ();

	// 현재 스레드의 상태를 THREAD_DYING으로 설정하고, 다른 스레드를 스케줄함. 실제 스레드 구조체 파괴는 schedule() 함수 내에서 처리됨.
	do_schedule (THREAD_DYING);

	// 실행되어서는 안된다는 뜻을 가지고 있음.
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */

// 현재 스레드가 CPU 사용을 자발적으로 양보함. 현재 스레드는 준비 큐로 돌아가며, 스케줄러의 결정에 따라 즉시 다시 스케줄될 수도 있음.
void
thread_yield (void) {

	// 현재 스레드를 가져옴.
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	// 인터럽트 컨텍스트가 아님을 확인.
	ASSERT (!intr_context ());

	// 인터러브를 비활성화하고 이전 상태 저장.
	old_level = intr_disable ();

	// 현재 스레드가 idle_thread가 아니면 준비 큐(ready_list)에 추가. (idle_thread는 일반적인 준비 큐 관리를 받지 않음.)
	if (curr != idle_thread)
		list_push_back (&ready_list, &curr->elem);
	
	// 현재 스레드의 상태를 THREAD_READY로 설정하고 다른 스레드를 스케줄함.
	do_schedule (THREAD_READY);

	// 이전 인터럽트 상태 복원.
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */

// 현재 실행 중인 스레드의 우선순위를 new_priority로 설정.
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
}

/* Returns the current thread's priority. */

// 현재 실행 중인 스레드의 우선순위를 반환.
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */

//-------------------------------------------------------------------------
// 멀티레벨 피드백 큐 스케줄러(MLFQS)와 관련된 값을 설정하거나 가져오기 위한 인터페이스들.
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}
//-------------------------------------------------------------------------

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */

// idle 스레드가 실행하는 함수. 다른 실행 가능한 스레드가 없을 때 CPU를 차지함.
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	// 전역 변수 idle_thread에 자기 자신(현재 실행 중인 idle 스레드)을 설정.
	idle_thread = thread_current ();

	// thread_start 함수에서 대기 중인 세마포어를 up하여, idle 스레드 초기화가 완료되었음을 알림.
	sema_up (idle_started);

	// 무한 루프
	for (;;) {
		/* Let someone else run. */
		
		// 인터럽트 비활성화
		intr_disable ();

		// idle 스레드 자신을 블록 상태로 만듦. (이렇게 하면 schedule()이 호출되어 다른 스레드가 있으면 실행됨.)
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */

		// 인터럽트를 다시 활성화(sti)하고 즉시 CPU를 정지(hlt)시킴.
		// hlt는 다음 인터럽트가 발생할 때까지 CPU를 저전력 상태로 만듦.
		// 이 두 명령어는 원자적으로 실행되어 불필요한 시간 낭비를 막음.
		// 다음 인터럽트(주로 타이머 인터럽트)가 발생하면, hlt에서 깨어나고, 스케줄러는 다시 실행할 스레드가 있는지 확인.
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */

// thread_create로 생성된 모든 커널 스레드의 실행 시작점 역할을 하는 래퍼 함수.
static void
kernel_thread (thread_func *function, void *aux) {

	// 스레드가 실행할 실제 함수 function이 유효한지 확인함.
	ASSERT (function != NULL);

	// 스케줄러는 인터럽트가 꺼진 상태에서 실행되므로, 스레드가 실제 작업을 시작하기 전에 인터럽트를 활성화.
	intr_enable ();       /* The scheduler runs with interrupts off. */

	// thread_create시 전달된 사용자 정의 함수 function을 인자 aux와 함께 호출. 스레드의 주된 작업 내용.
	function (aux);       /* Execute the thread function. */

	// 만약 function(aux)가 반환하면(즉, 스레드의 작업이 끝나면), thread_exit()을 호출하여 스레드를 정상적으로 종료시킴.
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */

// 스레드 구조체 t의 멤버들을 기본적인 값으로 초기화함. 이 함수는 t를 블록된 상태(THREAD_BLOCKED)로 설정함.
static void
init_thread (struct thread *t, const char *name, int priority) {

	// t, priority, name이 유효한지 확인함.
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	// 스레드 구조체 t의 전체 메모리를 0으로 초기화.
	memset (t, 0, sizeof *t);

	// 스레드의 초기 상태를 THREAD_BLOKED로 성정. (실행될려면 thread_unblock 필요)
	t->status = THREAD_BLOCKED;

	// 스레드 이름 복사
	strlcpy (t->name, name, sizeof t->name);

	// 스레드의 커널 스택 포인터(rsp)를 설정. t는 페이지의 시작 주소를 가리키고, 스택은 페이지의 높은 주소에서 낮은 주소로 자라므로, 페이지의 끝에서 약간 아래로 설정.
	// sizeof(void *)는 초기 kernel_thread 호출 시 rip를 위한 공간일 수 있음.
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);

	// 우선순위를 설정.
	t->priority = priority;

	// 스택 오버플로우 감지 등을 위한 매직 넘버를 설정.
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */

// 다음에 실행할 스레드를 스케줄링 정책에 따라 선택하여 반환. (현재 코드는 간단한 라운드 로빈 방식)
static struct thread *
next_thread_to_run (void) {

	// 준비 큐(ready_list)가 비어있으면, 
	if (list_empty (&ready_list))
	
		// idle 스레드를 반환.
		return idle_thread;

	// 준비 큐에 스레드가 있으면,
	else
		// 준비 큐의 가장 앞에 있는 스레드를 꺼내어 반환.(FIFO 방식, 라운드 로빈 스케줄링의 기본)
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */

// 인터럽트 프레임 tf에 저장된 레지스터 값들을 CPU 레지스터로 복원하고, iretq 명령을 실행하여 스레드의 실행을 재개(또는 시작)함.
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(

			// 스택 포인터(rsp)를 tf(인터럽트 프레임의 주소)로 설정. tf의 내용이 스택 최상단에 있는 것처럼 접근할 수 있음.
			// movq: 스택(즉, tf가 가리키는 메모리)에서 일반 목적 레지스터들(r15부터 rax까지 순서대로)의 값을 꺼내어 해당 CPU 레지스터에 복원.
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

			// 복원한 일반 목적 레지스터들(15개 * 8바이트 = 120바이트) 만큼 스택 포인터를 증가.
			"addq $120,%%rsp\n"

			// 스택에서 데이터 세그먼트(ds)의 추가 세그먼트(es) 레지스터 값을 복원함.
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"

			// es, ds 몇 예약 공간(또는 패딩)만큼 스택 포인터를 증가시킴.
			"addq $32, %%rsp\n"

			// 인터럽트 변환 명령. 스택에서 rip(명령어 포인터), cs(코드 세그먼트), eflages(플래그 레지스터), rsp(스택 포인터), ss(스택 세그먼트)를 순서대로 꺼내어 CPU에 복원하고,
			// 새 rip와 cs로 실행을 재개. 권한 수준 변경도 처리 가능.
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */

// 현재 실행 중인 스레드에서 th로 지정된 다음 스레드로 컨텍스트 스위칭을 수행함. 현재 스레드의 실행 상태(레지스터 값들)를 해당 스레드의 인터럽트 프레임에 저장하고,
// do_iret을 호출하여 th 스레드의 실행을 시작.
static void
thread_launch (struct thread *th) {

	// 현재 실행 중인 스레드의 인터럽트 프레임 주소르 가져옴.
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	
	// 새로 실행될 스레드 th의 인터럽트 프레임 주소를 가져옴.
	uint64_t tf = (uint64_t) &th->tf;

	// 인터럽트가 꺼진 상태여야 함.
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */

			// push 명령어: 임시로 사용할 레지스터(rdx, rbx, rcx) 값을 스택에 저장.
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */

			// tf_cur룰 rax에, tf(새 스레드의 인터럽트 프레임)를 rcx에 로드.
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"

			// 현재 CPU의 일반 목적 레지스터들(r15 ~ rax 까지)의 값을 tf_cur(현재 스레드의 인터럽트 프레임)에 순서대로 저장.
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
			
			// 자징한 레지스터들 크기만큼 rax (즉, td_cur 내의 포인터)를 이동.
			"addq $120, %%rax\n"

			// 현재 es, ds 세그먼트 레지스터 값을 tf_cur에 저장.
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"

			// 세그먼트 레지스터 등 크기만큼 rax 이동.
			"addq $32, %%rax\n"

			// 현재 실행 위치(rip)를 계산하여 tf_cur에 저장.
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

			// 어셈블러 블록의 끝을 가리키는 레이블.
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */

// 현재 스레드의 상태를 주어진 status로 변경하고, 실제 스케줄링 로직(schedule())을 호출함. + 이전에 파괴가 요청된 스레드들의 메모리를 해제.
static void
do_schedule(int status) {

	// 인터럽트가 꺼진 상태여야 함.
	ASSERT (intr_get_level () == INTR_OFF);

	// 현재 스레드가 실행 중 상태여야 함.
	ASSERT (thread_current()->status == THREAD_RUNNING);

	// destruction_req 리스트(파괴 요청된 스레드 리스트)가 비어있지 않은 동안 반복
	while (!list_empty (&destruction_req)) {

		// 리스트에서 스레드(victim)를 하나 꺼냄.
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);

		// 해당 스레드가 사용하던 페이지(스레드 구조체 및 스택)의 메모리를 해제.
		palloc_free_page(victim);
	}
	
	// 현재 스레드의 상태를 매개변수로 받은 status
	thread_current ()->status = status;

	// 다음 스레드를 선택하고 실행하는 주 스케줄링 함수를 호출.
	schedule ();
}

// 다음에 실행할 스레드를 결정하고, 현재 스레드에서 그 스레드로 컨텍스트 스위칭을 수행.
static void
schedule (void) {

	// 현재 실행 중인 스레드를 가져옴.
	struct thread *curr = running_thread ();

	// 다음에 실행할 스레드를 선택함.
	struct thread *next = next_thread_to_run ();

	// 인터럽트가 꺼져있고, curr 스레드는 더 이상 THREAD_RUNNING이 아니며, next 스레드가 유효함을 확인.
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */

	// 다음에 실행될 스레드의 상태를 THREAD_RUNNING으로 표시함.
	next->status = THREAD_RUNNING;

	/* Start new time slice. */

	// 새 스레드를 위한 타임 슬라이스를 시작하므로, thread_ticks를 0으로 리셋.
	thread_ticks = 0;

// 사용자 프로그램 지원 시, next 스레드의 주소 공간(페이지 테이블 등)을 활성화.
#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	// 현재 스레드와 다음 스레드가 다르면 컨텍스트 스위칭이 필요.
	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */

		// 만약 이전 스레드 curr가 THREAD_DYING 상태이고 초기 스레드가 아니라면,
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);

			// curr 스레드를 destruction_req 리스트에 추가함. 실제 메모리 해제는 다음 do_schedule 호출 시 일어남.
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */

		// next 스레드로 컨텍스트를 전환. 이 함수는 현재 스레드의 상태를 저장하고 next 스레드의 상태를 복원하여 실행을 시작. 이 함수 호출 이후에는 next 스레드가 실행됨.
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */

// 새 스레드를 위한 고유한 TID(Thread Identifier)를 할당.
static tid_t
allocate_tid (void) {

	// 다음에 할당할 TID 값을 저장하는 정적 변수. (프로그램 시작 시 1로 초기화.)
	static tid_t next_tid = 1;
	tid_t tid;

	// next_tid에 대한 동시 접근을 막기 위해 락을 획득.
	lock_acquire (&tid_lock);

	// 현재 next_tid 값을 tid에 할당하고, next_tid를 1 증가 시킴.
	tid = next_tid++;

	// 락을 해제.
	lock_release (&tid_lock);

	// 할당된 tid 반환.
	return tid;
}
