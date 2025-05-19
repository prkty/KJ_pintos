#ifndef THREADS_VADDR_H
#define THREADS_VADDR_H

#include <debug.h>
#include <stdint.h>
#include <stdbool.h>

#include "threads/loader.h"

/* Functions and macros for working with virtual addresses.
 *
 * See pte.h for functions and macros specifically for x86
 * hardware page tables. */

#define BITMASK(SHIFT, CNT) (((1ul << (CNT)) - 1) << (SHIFT))

/* Page offset (bits 0:12). */
/* 첫번째 오프셋 비트의 주소 */
#define PGSHIFT 0 
/* 오프셋 비트의 수를 정의 */
#define PGBITS  12                      
/* 페이지 크기 */   
#define PGSIZE  (1 << PGBITS)             
/* 페이지 오프셋만 1로 나머지는 0으로 마스크 */ 
#define PGMASK  BITMASK(PGSHIFT, PGBITS)  

/* Offset within a page. */
/* va에서 페이지 오프셋(하위 12비트) 추출 */
#define pg_ofs(va) ((uint64_t) (va) & PGMASK) 
/* 페이지 번호 추출(va >> PGBITS)*/
#define pg_no(va) ((uint64_t) (va) >> PGBITS) /* vas*/

/* va를 다음 페이지 경계로 올김*/
#define pg_round_up(va) ((void *) (((uint64_t) (va) + PGSIZE - 1) & ~PGMASK))

/* va가 속한 페이지의 시작 주소로 내림 */
#define pg_round_down(va) (void *) ((uint64_t) (va) & ~PGMASK)

/* 커널 가상 메모리 시작 주소 */
#define KERN_BASE LOADER_KERN_BASE

/* 사용자 프로세스의 스택이 시작되는 가상주소를 나타낸다. */
#define USER_STACK 0x47480000

/* vaddr이 사용자 공간이면 true. */
#define is_user_vaddr(vaddr) (!is_kernel_vaddr((vaddr)))

/* vaddr이 커널 공간이면 true. */
#define is_kernel_vaddr(vaddr) ((uint64_t)(vaddr) >= KERN_BASE)

// FIXME: add checking
/* Returns kernel virtual address at which physical address PADDR
 *  is mapped. */
/*물리주소에 매핑되는 커널 가상주소 반환*/
#define ptov(paddr) ((void *) (((uint64_t) paddr) + KERN_BASE))

/* Returns physical address at which kernel virtual address VADDR
 * is mapped. */
/* 커널 가상주소에 매핑되는 물리메모리 반환 */
#define vtop(vaddr) \
({ \
	ASSERT(is_kernel_vaddr(vaddr)); \
	((uint64_t) (vaddr) - (uint64_t) KERN_BASE);\
})

#endif /* threads/vaddr.h */
