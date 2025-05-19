/* 사용자 프로세스가 커널 기능에 접근하려고 할 때마다 시스템 호출을 호출합니다. 이는 뼈대 시스템 호출 핸들러입니다. 
   현재는 메시지를 출력하고 사용자 프로세스를 종료합니다. 이 프로젝트의 2부에서는 시스템 호출에 
   필요한 다른 모든 작업을 수행하는 코드를 추가하겠습니다. */

#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* 시스템 콜.
 *
 * 이전에는 시스템 호출 서비스가 인터럽트 핸들러(예: 리눅스의 int 0x80)에 의해 처리되었습니다.
 * 그러나 x86-64에서는 제조사가 시스템 호출을 요청하는 효율적인 경로인 `syscall` 명령어를 제공합니다.
 *
 * syscall 명령어는 모델 특정 레지스터(MSR)에서 값을 읽어서 작동합니다.
 * 디테일한 사항은 메뉴얼을 참고하세요. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void 
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* 인터럽트 서비스 루틴은 syscall_entry가 사용자 영역 스택을 
	 * 커널 모드 스택으로 전환할 때까지 인터럽트를 처리해서는 안 됩니다.
	 * 따라서 FLAG_FL을 마스크했습니다. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 주요 시스템 호출 인터페이스 */
void 
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: 여기에 구현하면 됩니다.
	printf ("system call!\n");
	thread_exit ();
}
