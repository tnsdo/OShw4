/* File: src/vm/swap.h */
#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdbool.h>
#include <stddef.h>

/* 스왑 영역을 초기화. 부팅 시 한 번만 호출 */
void swap_init(void);

/* 물리 페이지(kpage)를 스왑 공간에 내보내고, 할당된 스왑 슬롯 번호를 반환.
   실패 시 -1 리턴 */
int swap_out(void *kpage);

/* 스왑 슬롯(slot)에서 페이지를 읽어와 kpage에 복사.
   swap_in 후 해당 슬롯은 반드시 bitmap에서 해제 */
void swap_in(int slot, void *kpage);

/* 스왑 슬롯(slot)을 해제(비트맵에서 mark 비우기) */
void swap_free(int slot);

#endif /* VM_SWAP_H */

