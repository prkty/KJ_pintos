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
#include "include/threads/init.h"
#include "include/lib/user/syscall.h"
#include "include/filesys/file.h"
#include "include/lib/kernel/console.h"
#include "include/threads/thread.h"
#include "include/devices/input.h"

struct lock filesys_lock;

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

	lock_init(&filesys_lock);
}

/* 주요 시스템 호출 인터페이스 */
void 
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: 여기에 구현하면 됩니다.

	/*
	
	왜 f->R.rax, f->R.rdi, f->R.rsi, f->R.rdx로 하는가??

	답: Pintos의 시스템 콜과 x86-64 시스템 콜 호출 규약 (Calling Convention) 때문

	rdi: 첫 번째 인자

	rsi: 두 번째 인자

	rdx: 세 번째 인자

	rax: 시스템 콜의 리턴값 저장 위치

	그래서 왜 왜 f->R.rdi, f->R.rsi, f->R.rdx 라고 씀?
	답: 시스템 콜을 호출하면, 유저 프로그램에서 rdi, rsi, rdx 등의 레지스터에 인자를 넣고 
	   인터럽트를 통해 커널에 전달한다.

	인자: f->R.rdi, f->R.rsi -> 사용자 레지스터에서 받은 값.f
	리턴: f->R.rax -> 결과값 저장
	
	*/

	int sys_num = f->R.rax;

	switch (sys_num) {
		case SYS_HALT:
			halt();
			break;
		case SYS_EXIT:
			exit(f->R.rdi);
			break;                   
		// case SYS_FORK:
		// 	break;                  
		// case SYS_EXEC:
		// 	break;                   
		case SYS_WAIT:
			f->R.rax = process_wait(f->R.rdi);
			break;                   
		case SYS_CREATE:
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;                
		case SYS_REMOVE:
			f->R.rax = remove(f->R.rdi);
			break;                 
		case SYS_OPEN:
			f->R.rax = open(f->R.rdi);
			break;                  
		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
			break;               
		case SYS_READ:
			f->R.rax = read((int)f->R.rdi, (void *)f->R.rsi, (unsigned)f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;                  
		case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;                  
		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;                   
		case SYS_CLOSE:
			close(f->R.rdi);
			break;
		default:
			exit(-1);
	}
}

// 문자열의 끝까지 접근 가능한지 확인 안하는 문제 발생! -> check_sting 함수 구현
void check_address(void* addr) {
	if (is_kernel_vaddr(addr) || addr == NULL || pml4_get_page(thread_current()->pml4, addr) == NULL)
        exit(-1);
}

struct file * process_get_file(int fd) {
	struct thread *curr = thread_current();

	if(fd < 2 || fd >= FDCOUNT_LIMIT) return NULL;

	return curr->fdt[fd];
}

int process_add_file(struct file *file) {
	struct thread *curr = thread_current();
	struct file **fdt = curr->fdt;

	for(int fd = 2; fd < FD_MAX; fd++) {
		if(curr->fdt[fd] == NULL) {
			curr->fdt[fd] = file;
			return fd;
		}
	}

	return -1;
}

int process_close_file(int fd) {
	struct thread *curr = thread_current();

    if (fd >= FDCOUNT_LIMIT)
        return -1;

    curr->fdt[fd] = NULL;
    return 0;
}

void
halt (void) {
	power_off();
}

void
exit (int status) {
	struct thread *t = thread_current();
	t->exit_status = status;
	printf("%s: exit(%d)\n", t->name, t->exit_status);
	thread_exit();
}

// pid_t
// fork (const char *thread_name){
	
// }

// int
// exec (const char *file) {

// }

int
wait (pid_t pid) {
	for(int i = 0; i < 500000000; i ++);

	return -1;
}

bool
create (const char *file, unsigned initial_size) {
	
	check_address(file);

	return filesys_create(file, initial_size);
}

bool
remove (const char *file) {
	
	check_address(file);

	if(file == NULL) return -1;

	return filesys_remove(file);
}

int
open (const char *file) {
	/*
	
	1단계:	시스템 콜 번호 처리 추가, syscall_handler()에 SYS_OPEN 케이스 만들기

	2단계:	open() 함수 만들기, 주소 확인, 파일 열기, 파일 디스크립터 할당

	3단계:	파일 테이블 관리, 스레드 내 file descriptor table 사용

	4단계:	filesys_open(), 호출 실제로 파일을 열도록 Pintos의 함수 사용

	5단계:	fd 리턴	성공이면 fd, 실패면 -1 리턴
	
	*/

	// check_address(file);
	// struct file *f = filesys_open(file);

	// if(file == NULL) exit(-1);

	// lock_acquire(&filesys_lock);
	

	// if(f == NULL) {
	// 	lock_release(&filesys_lock);
	// 	return -1;
	// }

	// int fd = process_add_file(f);
	// lock_release(&filesys_lock);
	// return fd;

	check_address(file);  // 주소 유효성 검사 (필수)

    lock_acquire(&filesys_lock);
    struct file *f = filesys_open(file);
    lock_release(&filesys_lock);

    if (f == NULL)
        return -1;

    struct thread *t = thread_current();
    int fd = t->next_fd++;
    t->fdt[fd] = f; // 또는 list_push_back(&t->file_list, ...)
    return fd;
}

// 파일 디스크립터를 이용해 열린 파일의 크기를 반환
int
filesize (int fd) {
	struct file *f = process_get_file(fd);
	if(f == NULL) return -1;

	lock_acquire(&filesys_lock);
    off_t length = file_length(f);
    lock_release(&filesys_lock);
    return length;
}

int
read (int fd, void *buffer, unsigned size) {

	check_address(buffer); // 버퍼의 시작 주소 검사

    // 버퍼의 끝 주소까지 검사
    for (unsigned i = 0; i < size; i++) {
        check_address((uint8_t *)buffer + i);
    }

    if(fd == 0) { // stdin
        unsigned char c;
        unsigned int bytes_read = 0;
        while (bytes_read < size) {
            c = input_getc();
            *((char *)buffer + bytes_read) = c;
            bytes_read++;
            if (c == '\n') break; // 엔터가 들어오면 읽기 중단 (일반적인 콘솔 입력 처리)
        }
        return bytes_read;
    }

    struct file *f = process_get_file(fd);
    if(f == NULL) return -1;

    lock_acquire(&filesys_lock); // 파일 시스템 접근 전에 락 획득
    off_t bytes_read = file_read(f, buffer, size);
    lock_release(&filesys_lock); // 파일 시스템 접근 후에 락 해제
    return bytes_read;
}

int
write (int fd, const void *buffer, unsigned size) {
	// 1단계: buffer 주소 검사
	check_address(buffer);

	for(unsigned i = 0; i < size; i++) check_address((uint8_t *)buffer + i);

	// 2단계: 파일 디스크립터가 stdout인지 확인
	if(fd == 1) {
		putbuf(buffer, size);
		return size;
	}

	// 3단계: 현재 스레드의 파일 디스트립터 테이블에서 fd에 해당하는 파일 찾기
	struct file *f = process_get_file(fd);
	if(f == NULL) return -1;

	// 4단계: 파일에 쓰기
	lock_acquire(&filesys_lock); // 파일 시스템 접근 전에 락 획득
    off_t bytes_written = file_write(f, buffer, size);
    lock_release(&filesys_lock); // 파일 시스템 접근 후에 락 해제
    return bytes_written;
}

// 주어진 fd(file descriptor)에 대해 파일 내 읽기/쓰기 위치를 position으로 옮기는 역할
void
seek (int fd, unsigned position) {
	struct file *f = process_get_file(fd);
	if(f == NULL) return;

	lock_acquire(&filesys_lock);
    file_seek(f, (off_t)position);
    lock_release(&filesys_lock);
}

// seek()과 짝을 이루는 시스템 콜로, 현재 파일 포인터가 어디에 있는지 반환
unsigned
tell (int fd) {
	struct file *f = process_get_file(fd);

	if(f == NULL) return -1;

	lock_acquire(&filesys_lock);
    off_t position = file_tell(f);
    lock_release(&filesys_lock);
    return position;
}

//  열려 있는 파일을 닫고, 해당 **파일 디스크립터(fd)**를 
// 파일 디스크립터 테이블에서 제거하여 자원을 회수하는 시스템 콜
void
close (int fd) {
	struct file *file = process_get_file(fd);

    if (fd < 3 || file == NULL)
        return;

    process_close_file(fd);

    file_close(file);
}