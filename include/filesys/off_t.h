#ifndef FILESYS_OFF_T_H
#define FILESYS_OFF_T_H

#include <stdint.h>

/*  파일 내에서의 오프셋(상대적인 위치). 
    여러 헤더 파일에서 이 정의를 원하지만 다른 정의는 원하지 않기 때문에 
    이것은 별도의 헤더 파일로 존재합니다.*/
typedef int32_t off_t;

/* Format specifier for printf(), e.g.:
 * printf ("offset=%"PROTd"\n", offset); */
#define PROTd PRId32

#endif /* filesys/off_t.h */
