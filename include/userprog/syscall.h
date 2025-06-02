#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include "include/lib/user/syscall.h"

void syscall_init (void);

void halt(void);
void exit(int status);
int wait (pid_t pid);
int write (int fd, const void *buffer, unsigned size);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int filesize (int fd);
void seek (int fd, unsigned position);
unsigned tell (int fd);
int read (int fd, void *buffer, unsigned size);
void close (int fd);
pid_t fork(const char *thread_name);
int exec (const char *file);

void check_address(void* addr);

#endif /* userprog/syscall.h */
