/* ELF 바이너리를 로드하고 프로세스를 시작합니다. */

#include "userprog/process.h"
#include "userprog/syscall.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
struct thread* pid_to_thread(tid_t child_tid);

/* initd 및 기타 프로세스를 위한 일반 프로세스 초기화 프로그램입니다. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* FILE_NAME에서 로드된 첫 번째 사용자 영역 프로그램인 "initd"를 시작합니다.
 * 새 스레드는 process_create_initd()가 반환되기 전에 스케줄링될 수 있으며, 종료될 수도 있습니다.
 * initd의 스레드 ID를 반환하거나, 스레드를 생성할 수 없는 경우 TID_ERROR를 반환합니다.
 * 이 함수는 한 번만 호출해야 합니다. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* FILE_NAME의 복사본을 만듭니다.
	 * 그렇지 않으면 호출자와 load() 사이에 경쟁이 발생합니다. */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	/* fn_copy에 file_name을 PGSIZE(4kb) 만큼 복사한다.*/
	strlcpy (fn_copy, file_name, PGSIZE);

	/* FILE_NAME을 실행하기 위해 새로운 스레드를 만든다. 이때 . */

	/* 이름 추가 */
	char *save_ptr;
	strtok_r(file_name, " ", &save_ptr);
	
	/*  */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);

	
	return tid;
}

/* 첫 번째 사용자 프로세스를 시작하는 스레드 함수입니다. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* 현재 프로세스를 `name`으로 복제합니다. 새 프로세스의 스레드 ID를 반환하거나,
 * 스레드를 생성할 수 없는 경우 TID_ERROR를 반환합니다. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	/* 현재 스레드를 새 스레드로 복제합니다.*/
	enum intr_level old_level;
	old_level = intr_disable();
	struct thread *curr = thread_current();

	memcpy(&curr->parent_if, if_, sizeof(struct intr_frame));
	tid_t child_id = thread_create (name, PRI_DEFAULT, __do_fork, curr);

	//printf("자식 쓰레드 ID : %d\n", &child_thread -> tid);
	if(child_id == NULL){
		return TID_ERROR;
	}
	
	struct thread *child_thread = pid_to_thread(child_id);
	child_thread -> parent_thread = curr;
	// printf("Current_Thread PID : %d\n", curr -> tid);
	// printf("fork_Child_Thread PID : %d\n", child_thread -> tid);
	// printf("fork_child_Thread status : %d\n", child_thread -> status);
	// printf("Before fork_sema : DOWN \n");
	intr_set_level(old_level);
	sema_down(&child_thread->fork_sema);
	// printf("After fork_sema : DOWN \n");

	if(child_thread->exit_status == -1)
		return TID_ERROR;
	//printf("After fork_sema : DOWN \n");
	return child_id;
}

#ifndef VM
/* 이 함수를 pml4_for_each에 전달하여 부모 주소 공간을 복제합니다.
 * 이는 프로젝트 2에만 적용됩니다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;
	
	/* 1. TODO: parent_page가 커널 페이지인 경우 즉시 반환합니다. */
	if (!is_user_vaddr(va)) {
    return true;
	}
	/* 2. 부모 페이지 맵 레벨 4에서 VA 해결합니다. */
	
	parent_page = pml4_get_page (parent->pml4, va); /* */
	/* → parent 프로세스의 가상주소 va가 가리키는 “실제 물리 페이지”를
	커널 가상 주소로 변환한 뒤 parent_page에 담거나,
	매핑이 없으면 NULL을 담는다. */

	if(parent_page == NULL){
		printf("[fork-duplicate] fail. parent.\n");
		return false;
	}
		
	/* 3. TODO: 자식 페이지에 새로운 PAL_USER 페이지를 할당하고 결과를 NEWPAGE로 설정합니다. */
	newpage = palloc_get_page(PAL_USER);
	if(newpage == NULL){
		printf("[fork-duplicate] fail. new page.\n");
		return false;
	}
	/* 4. TODO: 부모 페이지를 새 페이지에 복제하고 부모 페이지가 
	 * 쓰기 가능한지 확인합니다(결과에 따라 WRITABLE로 설정합니다). */
	writable = is_writable(pte);
	memcpy(newpage, parent_page, PGSIZE);
	/* 5. WRITABLE 권한을 가지고 VA 주소의 자식 페이지 테이블에 새 페이지를 추가합니다. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: 페이지 삽입에 실패하면 오류 처리를 수행합니다. */
		printf("[fork-duplicate] fail. page insert\n");
		return false;
	}
	return true;
}
#endif

/* 부모의 실행 컨텍스트를 복사하는 스레드 함수입니다.
 * 힌트) parent->tf는 프로세스의 사용자 영역 컨텍스트를 유지하지 않습니다.
 *       즉, process_fork의 두 번째 인수를 이 함수에 전달해야 합니다. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: 어떻게든 parent_if를 전달합니다. (즉, process_fork()의 if_) */
	struct intr_frame *parent_if = &parent -> parent_if;
	/* TODO */
	bool succ = true;
	
	/* 1. CPU 컨텍스트를 로컬 스택으로 읽습니다. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0;
	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;
	
	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: 코드를 여기에 입력하세요.
	 * TODO: 힌트) 파일 객체를 복제하려면 include/filesys/file.h에서 `file_duplicate`를 사용하세요.
	 * TODO:       이 함수가 부모의 리소스를 성공적으로 복제할 때까지
	 * TODO:       부모는 fork()에서 반환되어서는 안 됩니다. */
	
	for(int i = 3; i < 64; i++){
		if(parent->fdt[i] != NULL){
			current -> fdt[i] = file_duplicate(parent -> fdt[i]);
		}
	}

	/* TODO */
	// process_init ();
	// printf("Before fork_sema : UP \n");
	sema_up(&current->fork_sema);
	// printf("After fork_sema : UP \n");
	/* 마지막으로 새로 만든 프로세스로 전환합니다. */
	if (succ)
		do_iret (&if_);
error:
	thread_exit ();
}

/* 현재 실행 컨텍스트를 f_name으로 전환합니다.
 * 실패하면 -1을 반환합니다. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* 스레드 구조체에서는 intr_frame을 사용할 수 없습니다.
     * 현재 스레드가 재스케줄링될 때 실행 정보가 멤버에 저장되기 때문입니다. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 우리는 먼저 현재 컨텍스트를 죽입니다 */
	process_cleanup ();

	/* 그리고 바이너리를 로드합니다 */
	success = load (file_name, &_if);
	/* 로드에 실패하면 종료합니다. */
	palloc_free_page (file_name);
	if (!success){
		return -1;
	}
	
	// 디버깅용
	// hex_dump(_if.rsp, _if.rsp, USER_STACK - _if.rsp, true);
	// 전환 프로세스를 시작합니다.

	
	do_iret (&_if);
	NOT_REACHED ();
}


/* 스레드 TID가 종료될 때까지 기다리고 종료 상태를 반환합니다.
 * 커널에 의해 종료된 경우(예: 예외로 인해 종료된 경우) -1을 반환합니다.
 * TID가 유효하지 않거나 호출 프로세스의 자식 프로세스가 아니거나,
 * 주어진 TID에 대해 process_wait()가 이미 성공적으로 호출된 경우,
 * 대기하지 않고 즉시 -1을 반환합니다.
 *
 * 이 함수는 문제 2-2에서 구현될 것입니다.
 * 현재로서는 아무 작업도 수행하지 않습니다. */
int
process_wait (tid_t child_tid) {
	struct thread *curr = thread_current();				// 부모 쓰레드
	struct thread *child_thread = NULL;					// 자식 쓰레드
	struct list_elem* e;
	// printf("부모 쓰레드의 PID : %d\n", curr -> tid);
	for(e = list_begin(&curr->child_list); e != list_end(&curr->child_list); e = list_next(e)){
		struct thread *t = list_entry(e, struct thread, fork_elem);
		if(t-> tid == child_tid){
			child_thread = t;
			// printf("포크가 됬다.\n");
			break; 
		}
	}
	if(child_thread == NULL){
		// printf("child_thread = NULL\n");
		return -1;
	}
	
	// printf("자식 쓰레드의 PID : %d\n", child_thread -> tid);
	// printf("Before PID : %d, waiting_sema : DOWN \n", curr->tid);
	sema_down(&child_thread->waiting_sema);
	// printf("After PID : %d, waiting_sema : DOWN \n", curr->tid);

	/* XXX: Hint) pintos는 process_wait(initd)가 발생하면 종료되므로,
	 * XXX:       process_wait를 구현하기 전에 여기에 무한 루프를 추가하는 것이 좋습니다. */

	int status = child_thread -> exit_status;
	// printf("Child status : %d\n", child_thread -> exit_status);
	list_remove(&child_thread -> fork_elem);
	sema_up(&child_thread -> free_sema);
	return status;
}

/* 프로세스를 종료합니다. 이 함수는 thread_exit()에 의해 호출됩니다. */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: 여기에 코드를 입력하세요.
	 * TODO: 프로세스 종료 메시지를 구현하세요(project2/process_termination.html 참조).
	 * TODO: 여기에 프로세스 리소스 정리를 구현하는 것이 좋습니다. */

	//printf("Before waiting_sema : UP \n");
	
	// printf("after waiting_sema : UP \n");
	// sema_down(&curr->free_sema);
	process_cleanup ();

	//printf("Before waiting_sema : UP \n");
	sema_up(&curr->waiting_sema);
	// printf("after waiting_sema : UP \n");
	sema_down(&curr->free_sema);
}

/* 현재 프로세스의 리소스를 해제합니다. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* 현재 프로세스의 페이지 디렉터리를 삭제하고
     * 커널 전용 페이지 디렉터리로 다시 전환합니다. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* 여기서 올바른 순서가 중요합니다. 페이지 디렉터리를 전환하기 전에 
		 * cur->pagedir을 NULL로 설정해야 타이머 인터럽트가 프로세스 페이지 디렉터리로 다시 전환할 수 없습니다.
		 * 프로세스의 페이지 디렉터리를 삭제하기 전에 기본 페이지 디렉터리를 활성화해야 합니다. 
		 * 그렇지 않으면 활성 페이지 디렉터리가 해제(및 삭제)된 디렉터리가 됩니다. */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* 중첩 스레드에서 사용자 코드를 실행하기 위한 CPU를 설정합니다.
 * 이 함수는 모든 컨텍스트 전환 시 호출됩니다. */
void
process_activate (struct thread *next) {
	/* 스레드의 페이지 테이블을 활성화합니다. */
	pml4_activate (next->pml4);

	/* 인터럽트 처리에 사용할 스레드의 커널 스택을 설정합니다 */
	tss_update (next);
}

/* ELF 바이너리를 로드합니다. 다음 정의는 ELF 사양 [ELF1]에서 거의 그대로 가져왔습니다.  */

/* ELF 타입들.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* 무시함 */
#define PT_LOAD    1            /* 로드 가능한 세그먼트 */
#define PT_DYNAMIC 2            /* 동적 링크 정보 */
#define PT_INTERP  3            /* 동적 로더의 이름 */
#define PT_NOTE    4            /* 보조 정보 */
#define PT_SHLIB   5            /* 리버스 */
#define PT_PHDR    6            /* 프로그램 헤더 테이블 */
#define PT_STACK   0x6474e551   /* 스택 세그먼트 */

#define PF_X 1          /* 실행 가능 */
#define PF_W 2          /* 쓰기 가능 */
#define PF_R 4          /* 읽기 가능 */

/* 실행 파일 헤더입니다. [ELF1] 1-4부터 1-8까지를 참조하세요.
 * ELF 바이너리의 맨 처음에 나타납니다. */
struct ELF64_hdr {
    unsigned char e_ident[EI_NIDENT]; /* ELF 식별 정보 (매직 넘버, 클래스, 데이터 인코딩, 버전 등)를 담는 배열 */
    uint16_t      e_type;             /* 오브젝트 파일의 타입 (예: 실행 파일, 재배치 가능 파일, 공유 오브젝트 등) */
    uint16_t      e_machine;          /* 필요한 아키텍처 (예: x86, x86-64, ARM 등) */
    uint32_t      e_version;          /* 오브젝트 파일의 버전 */
    uint64_t      e_entry;            /* 프로그램 실행 시작점의 가상 주소 (실행 파일인 경우) */
    uint64_t      e_phoff;            /* 프로그램 헤더 테이블의 파일 내 오프셋 (바이트 단위) */
    uint64_t      e_shoff;            /* 섹션 헤더 테이블의 파일 내 오프셋 (바이트 단위) */
    uint32_t      e_flags;            /* 프로세서별 플래그 */
    uint16_t      e_ehsize;           /* ELF 헤더의 크기 (바이트 단위) */
    uint16_t      e_phentsize;        /* 프로그램 헤더 테이블의 한 항목(entry)의 크기 (바이트 단위) */
    uint16_t      e_phnum;            /* 프로그램 헤더 테이블의 항목(entry) 개수 */
    uint16_t      e_shentsize;        /* 섹션 헤더 테이블의 한 항목(entry)의 크기 (바이트 단위) */
    uint16_t      e_shnum;            /* 섹션 헤더 테이블의 항목(entry) 개수 */
    uint16_t      e_shstrndx;         /* 섹션 이름 문자열 테이블(section name string table)이 있는 섹션 헤더 테이블의 인덱스 */
};

struct ELF64_PHDR {
    uint32_t p_type;    /* 이 프로그램 헤더 항목이 설명하는 세그먼트의 타입 */
    uint32_t p_flags;   /* 세그먼트 관련 플래그 (예: 읽기/쓰기/실행 권한) */
    uint64_t p_offset;  /* 파일의 시작부터 이 세그먼트의 첫 바이트까지의 오프셋 (파일 내 위치) */
    uint64_t p_vaddr;   /* 메모리에서 이 세그먼트의 첫 바이트가 위치할 가상 주소 */
    uint64_t p_paddr;   /* 물리 메모리 주소 (물리 주소 지정이 관련된 시스템에서 사용, 보통 무시됨) */
    uint64_t p_filesz;  /* 파일 이미지에서 이 세그먼트가 차지하는 크기 (바이트 단위) */
    uint64_t p_memsz;   /* 메모리에서 이 세그먼트가 차지하는 크기 (바이트 단위) */
    uint64_t p_align;   /* 세그먼트가 메모리와 파일에서 정렬되어야 하는 방식 (2의 거듭제곱 값) */
};

/* 약어 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* FILE_NAME에서 ELF 실행 파일을 현재 스레드로 로드합니다.
 * 실행 파일의 진입점을 *RIP에 저장하고
 * 초기 스택 포인터를 *RSP에 저장합니다.
 * 성공하면 true를 반환하고, 그렇지 않으면 false를 반환합니다. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	// 문자 저장할 문자열
	char *argv[64], *save_ptr, *token;
	int argc = 0;
	// printf("filename: %s\n", file_name);
	for(argv[argc] = strtok_r(file_name, " ", &save_ptr); argv[argc] != NULL; 
		argv[argc] = strtok_r(NULL, " ", &save_ptr)){
		// printf("argv[%d]: %s\n", argc, argv[argc]);
		argc++; 
	}

	file_name = argv[0];
	// printf("argv[%d]: %s\n", argc, argv[argc]);
	/* 페이지 디렉토리를 할당하고 활성화합니다. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* 실행 파일을 엽니다. 1312312312*/
	file = filesys_open (argv[0]);

	if (file == NULL) {
		printf ("load: %s: open failed\n", argv[0]);
		goto done;
	}

	/* rox 할 때 */
	t->running_file = file;
	file_deny_write(file);
	/* rox 할 때 */
	/* 실행 가능한 헤더를 읽고 검증합니다. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* 프로그램 헤더를 읽습니다. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* 이 세그먼트를 무시합니다. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* 보통의 세그먼트
						 * 디스크에서 초기 부분을 읽고 나머지는 0으로 채웁니다. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* 단지 0.
						 * 디스크에서 아무것도 읽지 않습니다. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}
	
	/* 스택 설정. */
	if (!setup_stack (if_))
		goto done;
	
	/* 시작 주소. */
	if_->rip = ehdr.e_entry;

	
	/* 스택에 인자들 저장 */
	argument_to_stack(if_, argc, &argv);
	success = true;
done:
	/* 우리는 화물이 성공적으로 도착하든 실패하든 여기에 도착합니다. */
	// file_close (file);
	// printf("success : %d\n",success);
	return success;
}

void argument_to_stack(struct intr_frame *if_, int argc, char ** argv){
	uintptr_t addrlist[64];
	for(int j = argc - 1; j >= 0; j--){
		if_ -> rsp -= strlen(argv[j]) + 1;
		memcpy(if_->rsp, argv[j], strlen(argv[j]) + 1);
		addrlist[j] = (char *)if_->rsp;
	}
	
	// printf("%d\n", if_-> rsp);
	uintptr_t padding = if_->rsp % 16;
	if_->rsp -= padding;
	memset((char *)if_->rsp, 0, padding);

	argv[argc] = 0;
	addrlist[argc] = NULL;
	for(int j = argc; j>= 0; j--){
		if_ -> rsp -= sizeof(char *);
		memcpy(if_->rsp, &addrlist[j], sizeof(char *));
	}
	/* %rsi에 argv[0]의 주소, %rdi에 argc값 저장*/
	if_ -> R.rdi = argc;
	if_ -> R.rsi = if_->rsp;
	if_->rsp -= sizeof(void *);
	memset((void *)if_->rsp, 0, sizeof(void *));
	// printf("rsp = %p\n", (void *) if_->rsp);
}


/* PHDR이 FILE에 유효하고 로드 가능한 세그먼트를 설명하는지 확인하고, 
 * 그렇다면 true를 반환하고, 그렇지 않으면 false를 반환합니다. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset과 p_vaddr은 동일한 페이지 오프셋을 가져야 합니다. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset은 FILE 내부를 가리켜야 합니다. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz는 최소한 p_filesz만큼 커야 합니다. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* 세그먼트는 비어 있을 수 없습니다. */
	if (phdr->p_memsz == 0)
		return false;

	/* 가상 메모리 영역은 사용자 주소 공간 범위 내에서 시작하고 끝나야 합니다. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* 해당 영역은 커널 가상 주소 공간을 "둘러싸고" 있을 수 없습니다. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* 매핑 페이지 0을 허용하지 않습니다.
	   페이지 0을 매핑하는 것은 나쁜 생각일 뿐만 아니라, 이를 허용한다면 시스템 호출에
	   널 포인터를 전달하는 사용자 코드가 memcpy() 등의 널 포인터 어설션을 통해
	   커널에 패닉을 일으킬 가능성이 매우 높습니다. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* 이 블록의 코드는 프로젝트 2에서만 사용됩니다.
 * 프로젝트 2 전체에 함수를 구현하려면 #ifndef 매크로 외부에서 구현하세요. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* OFS 오프셋에서 시작하는 세그먼트를 FILE의 UPAGE 주소에 로드합니다. 
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 다음과 같이 초기화됩니다.
 *
 * - UPAGE의 READ_BYTES 바이트는 오프셋 OFS에서 시작하여 FILE에서 읽어야 합니다.
 *
 * - UPAGE + READ_BYTES에서 ZERO_BYTES 바이트는 0으로 설정해야 합니다.
 *
 * 이 함수로 초기화된 페이지는 WRITABLE이 true인 경우 사용자 프로세스에서 쓰기 가능해야 하고, 
 * 그렇지 않은 경우 읽기 전용이어야 합니다.
 *
 * 성공하면 true를 반환하고, 메모리 할당 오류나 디스크 읽기 오류가 발생하면 false를 반환합니다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산해 보세요.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고 
		 * 마지막 PAGE_ZERO_BYTES 바이트를 0으로 설정합니다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* 메모리 한 페이지를 얻습니다. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* 페이지를 읽어들입니다. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* 프로세스의 주소 공간에 페이지를 추가합니다. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK에 0으로 설정된 페이지를 매핑하여 최소 스택을 생성합니다. */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* 사용자 가상 주소 UPAGE에서 커널 가상 주소 KPAGE로의 매핑을 페이지 테이블에 추가합니다.
 * WRITABLE이 참이면 사용자 프로세스가 페이지를 수정할 수 있습니다. 그렇지 않으면 읽기 전용입니다.
 * UPAGE는 이미 매핑되어 있으면 안 됩니다.
 * KPAGE는 palloc_get_page()를 사용하여 사용자 풀에서 가져온 페이지여야 합니다.
 * 성공 시 참을 반환하고, UPAGE가 이미 매핑되었거나 메모리 할당에 실패하면 거짓을 반환합니다. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}

struct thread* pid_to_thread(tid_t child_tid){
	struct thread *curr = thread_current();				// 부모 쓰레드
	struct list_elem* e;
	for(e = list_begin(&curr->child_list); e != list_end(&curr->child_list); e = list_next(e)){
		struct thread *t = list_entry(e, struct thread, fork_elem);
		if(t-> tid == child_tid){
			// printf("여기서 t->tid : %d\n", t->tid);
			return t;
		}
	}
}

#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
