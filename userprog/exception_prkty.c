/* 사용자 프로세스가 권한이 있거나 금지된 작업을 수행하면 커널에 exception또는 . 로 트랩됩니다. 
   이 파일들은 예외를 처리합니다. 현재 모든 예외는 단순히 메시지를 출력하고 프로세스를 종료합니다. 
   프로젝트 2에 대한 일부 해결책은 이 파일을 fault수정해야 하지만, 전부는 아닙니다 .page_fault() */

#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"

/* 처리된 페이지 오류 수. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* 사용자 프로그램으로 인해 발생할 수 있는 인터럽트에 대한 핸들러를 등록합니다.

   실제 유닉스 계열 OS에서는 이러한 인터럽트의 대부분이 [SV-386] 3-24 및 3-25에 
   설명된 대로 시그널 형태로 사용자 프로세스에 전달되지만, 우리는 시그널을 구현하지 않습니다.
   대신, 시그널이 사용자 프로세스를 종료하도록 할 것입니다.

   페이지 오류는 예외입니다. 여기서는 다른 예외와 동일한 방식으로 처리되지만, 
   가상 메모리를 구현하려면 이를 변경해야 합니다.

   각 예외에 대한 설명은 [IA32-v3a] 섹션 5.15 "예외 및 인터럽트 참조"를 참조하세요. */
void
exception_init (void) {
	/* 이러한 예외는 사용자 프로그램에서 명시적으로 발생할 수 있습니다. (예: INT, INT3, INTO, BOUND 명령어)
	  따라서 DPL==3으로 설정하면 사용자 프로그램에서 밑에와 같은 명령어를 통해 예외를 호출할 수 있습니다. */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
			"#BR BOUND Range Exceeded Exception");

	/* 이러한 예외는 DPL==0으로 설정되어 사용자 프로세스가 INT 명령을 통해 해당 예외를 호출할 수 없습니다.
	   간접적으로 발생할 수도 있습니다. 예를 들어, 0으로 나누면 #DE 오류가 발생할 수 있습니다.  */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
			"#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
			"#XF SIMD Floating-Point Exception");

	/* 대부분의 예외는 인터럽트를 켜면 처리할 수 있습니다.
	   페이지 폴트 발생 시 인터럽트를 비활성화해야 하는데, 
	   폴트 주소가 CR2에 저장되어 있고 이를 보존해야 하기 때문입니다. */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* 사용자 프로세스로 인해 발생한 예외(아마도)에 대한 핸들러입니다. */
static void
kill (struct intr_frame *f) {
	/* 이 인터럽트는 (아마도) 사용자 프로세스에 의해 발생합니다.
	   예를 들어, 프로세스가 매핑되지 않은 가상 메모리에 접근하려고 시도했을 수 있습니다(페이지 폴트).
	   지금은 단순히 사용자 프로세스를 종료합니다.
	   나중에 커널에서 페이지 폴트를 처리해야 합니다.
	   실제 유닉스 계열 운영 체제는 대부분의 예외를 시그널을 통해 프로세스로 전달하지만, 
	   우리는 이를 구현하지 않습니다. */

	/* 인터럽트 프레임의 코드 세그먼트 값은 예외가 발생한 위치를 알려줍니다. */
	switch (f->cs) {
		case SEL_UCSEG:
			/* 사용자 코드 세그먼트이므로 예상대로 사용자 예외가 발생합니다.  
			   유제 프로세스를 Kill 합니다.  */
			printf ("%s: dying due to interrupt %#04llx (%s).\n",
					thread_name (), f->vec_no, intr_name (f->vec_no));
			intr_dump_frame (f);
			thread_exit ();

		case SEL_KCSEG:
			/* 커널 버그를 나타내는 커널 코드 세그먼트입니다. 
			   커널 코드는 예외를 발생시켜서는 안 됩니다.
			   (페이지 폴트가 커널 예외를 유발할 수는 있지만, 여기에 발생해서는 안 됩니다.) 
			   커널을 패닉 상태로 만들어서 요점을 명확히 전달합니다. */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:
			/* 다른 코드의 세그먼트인가요? 발생해서는 안 됩니다. 커널을 패닉 상태로 만듭니다. */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
					f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/* 페이지 폴트 처리기입니다. 
   이는 가상 메모리를 구현하기 위해 반드시 작성해야 하는 기본 골격입니다. 
   프로젝트 2에 대한 일부 솔루션에서도 이 코드를 수정해야 할 수 있습니다.

   진입 시, 오류가 발생한 주소는 CR2(제어 레지스터 2)에 있고, 
   exception.h의 PF_* 매크로에 설명된 대로 형식화된 오류 정보는 F의 error_code 멤버에 있습니다.
   이 예제 코드는 해당 정보를 구문 분석하는 방법을 보여줍니다.
   이 두 가지에 대한 자세한 내용은 [IA32-v3a] 섹션 5.15 
   "예외 및 인터럽트 참조"의 "인터럽트 14--페이지 오류 예외(#PF)" 설명을 참조하십시오. */
static void
page_fault (struct intr_frame *f) {
	bool not_present;  /* True: not-present page, false: writing r/o page. */
	bool write;        /* True: access was write, false: access was read. */
	bool user;         /* True: access by user, false: access by kernel. */
	void *fault_addr;  /* Fault address. */

	/* 오류 발생 주소, 즉 오류를 발생시킨 가상 주소를 구합니다.
	   이 주소는 코드나 데이터를 가리킬 수 있습니다.
	   오류를 발생시킨 명령어(즉, f->rip)의 주소일 필요는 없습니다. */

	fault_addr = (void *) rcr2();

	/* 인터럽트를 다시 켜세요(CR2가 변경되기 전에 읽을 수 있도록 하기 위해 인터럽트를 꺼두었습니다). */
	intr_enable ();


	/* 원인을 파악하세요. */
	not_present = (f->error_code & PF_P) == 0;
	write = (f->error_code & PF_W) != 0;
	user = (f->error_code & PF_U) != 0;

#ifdef VM
	/* For project 3 and later. */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif

	/* 페이지 폴트를 계산합니다. */
	page_fault_cnt++;

	/* 오류가 실제 오류인 경우 정보를 표시하고 종료합니다. */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
			fault_addr,
			not_present ? "not present" : "rights violation",
			write ? "writing" : "reading",
			user ? "user" : "kernel");
	kill (f);
}

