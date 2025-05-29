/* 사용자 프로세스가 커널 기능에 접근하려고 할 때마다 시스템 호출을 호출합니다. 이는 뼈대 시스템 호출 핸들러입니다. 
   현재는 메시지를 출력하고 사용자 프로세스를 종료합니다. 이 프로젝트의 2부에서는 시스템 호출에 
   필요한 다른 모든 작업을 수행하는 코드를 추가하겠습니다. */

#include "userprog/syscall.h"
#include "lib/user/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/file.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "threads/palloc.h"
#include "lib/string.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void halt(void);
pid_t fork(const char *thread_name);
int exec(const char *cmd_line);
int wait(pid_t pid);
bool create(const char *file, unsigned initial_size);
void exit(int status);
int open(const char *file);
bool remove(const char *file);
int filesize (int fd);
int read(int fd, void *buffer, unsigned size);
int write(int fd, const void *buffer, unsigned size);
void seek (int fd, unsigned position);
// unsigned tell (int fd);
void close (int fd);

struct lock filesys_lock;

void is_user_memory(void * addr);

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

	lock_init(&filesys_lock);
	/* 인터럽트 서비스 루틴은 syscall_entry가 사용자 영역 스택을 
	 * 커널 모드 스택으로 전환할 때까지 인터럽트를 처리해서는 안 됩니다.
	 * 따라서 FLAG_FL을 마스크했습니다. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 주요 시스템 호출 인터페이스 */
void 
syscall_handler (struct intr_frame *f) {

	int sc_number =f -> R.rax;
	// printf("%d\n", sc_number);
	switch(sc_number){
		case SYS_HALT:		// 0
			halt();
			break;
		case SYS_EXIT:		// 1
			exit(f->R.rdi);
			break;
		case SYS_FORK:		// 2
			f->R.rax = process_fork(f->R.rdi, f);
			break;
		case SYS_EXEC:	// 3
			f->R.rax = exec(f->R.rdi);
			break;
		case SYS_WAIT:		// 4
			f->R.rax = wait(f->R.rdi);
			break;
		case SYS_CREATE:	// 5
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;
		case SYS_REMOVE:	// 6
			break;
		case SYS_OPEN:		// 7
			f->R.rax = open(f->R.rdi);
			break;
		case SYS_FILESIZE:	// 8
			f->R.rax = filesize(f->R.rdi);
			break;
		case SYS_READ:		// 9
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:		// 10
			f->R.rax = write((int)f->R.rdi, (void *)f->R.rsi, (unsigned)f -> R.rdx);
			break;
		case SYS_SEEK:		// 11
			seek(f->R.rdi, f->R.rsi); 
			break;	
		// case SYS_TELL:	// 12
		// 	break;
		case SYS_CLOSE:		// 13
			close(f->R.rdi);
			break;
		default:
			exit(f->R.rdi);
			break;
	}

}

void halt(void){
	power_off();
}

void exit(int status){
	printf("%s: exit(%d)\n",thread_current() ->name, status);
	thread_current()->exit_status = status;
	/* rox 용 */
	if(thread_current()->running_file){
		file_allow_write(thread_current()->running_file);
		file_close(thread_current()->running_file);

	}
	thread_exit();
}

pid_t fork(const char *thread_name){
	struct thread * curr = thread_current();
	
	return process_fork(thread_name, &curr -> tf);
}

int exec(const char *cmd_line){
	is_user_memory(cmd_line);
	char *fn = palloc_get_page(PAL_USER);

	int size = strlen(cmd_line) + 1;
	if(fn == NULL)
		return -1;
	strlcpy(fn, cmd_line, size);

	if(process_exec(fn) == -1){
		exit(-1);
	}

	return 0;
}

int wait(pid_t pid){
	int status = process_wait(pid);
	return status;
}

bool create(const char *file, unsigned initial_size){
	is_user_memory(file);
	return filesys_create(file, initial_size);
}

int open(const char *file){
	is_user_memory(file);
	struct thread *curr = thread_current();
	struct file *opened_file = filesys_open(file);
	// file_deny_write(opened_file);
	int a = file_to_fd(opened_file);
	return a;
}

bool remove(const char *file){
	if(!filesys_remove(file)){
		return false;
	}
	return true; 
}

int file_to_fd(struct file *file){
	struct thread *curr = thread_current();
	struct file **fdt = curr -> fdt;
	if(file == NULL)
		return -1;
	for (int fd = 3; fd < 64; fd++){
		if(fdt[fd] == NULL){
			fdt[fd] = file;
			return fd;
		}
	}
}

int filesize (int fd){
	struct thread *curr = thread_current();
	if(fd < 0 || fd >= 64)
		return NULL;
	struct file *f = curr -> fdt[fd];
	int a = file_length(f);
	return a;
}

int read(int fd, void *buffer, unsigned size){
	is_user_memory(buffer);
	struct thread *curr = thread_current();
	if(fd < 0 || fd >= 64)
		return NULL;
	struct file *readed_file = curr-> fdt[fd];
	if(readed_file == NULL){
		return -1; 
	}
	int bytes = 0;
	if(fd == 1) exit(-1);
	if(fd == 0){
		for(int i = 0; i < size; i++){	
			char c = input_getc();
			((char *) buffer)[i] = c;
			if( c == '\n'){
				bytes = i;
				break;
			}
		}
		return bytes;
	}
	else{
		// file_deny_write(readed_file);
		lock_acquire(&filesys_lock);
		bytes = file_read (readed_file, buffer, size);
		lock_release(&filesys_lock);
	}
	return bytes;
}

int write(int fd, const void *buffer, unsigned size){
	is_user_memory(buffer);
	int ret;
	if(fd == 1){
		putbuf(buffer, size);
		ret = size;
	}
	else if(fd > 1 && fd < 64 && thread_current() -> fdt[fd]){
		lock_acquire(&filesys_lock);
		
		ret = file_write (thread_current() -> fdt[fd], buffer, size);
		lock_release(&filesys_lock);
	}
	else
		ret = -1;
	return ret;
}

void seek (int fd, unsigned position){
	struct thread *curr = thread_current();
	struct file *seeked_file = curr-> fdt[fd];

	file_seek(seeked_file, position);
}

unsigned tell (int fd){

}

void close (int fd){
	if(fd < 0 || fd >= 64){
		return;
	}
	struct file *f = thread_current()->fdt[fd];
	if(f == NULL){
		return;
	} 
}

void is_user_memory(void * addr){
	struct thread *cur = thread_current();
	if(addr == NULL || !is_user_vaddr(addr) || pml4_get_page(cur->pml4, addr) == NULL){
		exit(-1);
	}
}

struct file *fd_to_file(int fd){
	struct thread *curr = thread_current();
	if(fd < 0 || fd >= 64)
		return NULL;
	struct file *f = curr-> fdt[fd];

	return f;
}