/* File: src/vm/swap.c */
#include "vm/swap.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include <bitmap.h>
#include <debug.h>
#include "threads/vaddr.h"   /* PGSIZE를 위해 필요 */
#include "devices/block.h"
#include "threads/palloc.h"

/* 스왑 블록과 비트맵, 락을 전역으로 선언 */
static struct block *swap_block;
static struct bitmap *swap_bitmap;
static struct lock swap_lock;

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

void
swap_init(void) {
    /* 1) 스왑용 블록 장치 얻어오기 */
    swap_block = block_get_role(BLOCK_SWAP);
    ASSERT(swap_block != NULL);

    /* 2) 블록 크기(섹터 수) / 한 페이지당 섹터 수 = 총 스왑 슬롯 수 */
    size_t nslots = block_size(swap_block) / SECTORS_PER_PAGE;
    swap_bitmap = bitmap_create(nslots);
    ASSERT(swap_bitmap != NULL);

    /* 3) 스왑 영역 비트맵 락 초기화 */
    lock_init(&swap_lock);
}

int
swap_out(void *kpage) {
    /* kpage: 내보낼 물리 주소 (커널 물리 페이지)
       1) 빈 슬롯(bitmap에서 false인 슬롯)을 찾아서
       2) 해당 슬롯 번호(slot) * SECTORS_PER_PAGE 부터 연속으로
          block_write()로 페이지 크기만큼(4096B) 섹터에 저장
       3) bitmap_mark() 해서 슬롯을 차지 상태로 표시
       4) 리턴 slot
     */
    lock_acquire(&swap_lock);

    /* 빈 슬롯 검색 */
    size_t slot = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
    if (slot == BITMAP_ERROR) {
        /* 빈 슬롯 없음 */
        lock_release(&swap_lock);
        return -1;
    }

    /* 실제로 스왑 블록에 기록 */
    size_t sector_ofs = slot * SECTORS_PER_PAGE;
    size_t i;
    for (i = 0; i < SECTORS_PER_PAGE; i++) {
        void *sector_ptr = (uint8_t *)kpage + i * BLOCK_SECTOR_SIZE;
        block_write(swap_block, sector_ofs + i, sector_ptr);
    }

    lock_release(&swap_lock);
    return (int)slot;
}

void
swap_in(int slot, void *kpage) {
    /* slot: 스왑 인덱스 (swap_out에서 리턴된 값)
       kpage: 복원할 페이지 물리 주소
       1) block_read()로 슬롯*SECTORS_PER_PAGE부터 연속으로 읽어와 kpage에 복사
       2) bitmap_reset()으로 해당 슬롯 해제
     */
    lock_acquire(&swap_lock);

    size_t sector_ofs = slot * SECTORS_PER_PAGE;
    size_t i;
    for (i = 0; i < SECTORS_PER_PAGE; i++) {
        void *sector_ptr = (uint8_t *)kpage + i * BLOCK_SECTOR_SIZE;
        block_read(swap_block, sector_ofs + i, sector_ptr);
    }
    bitmap_reset(swap_bitmap, slot);

    lock_release(&swap_lock);
}

void
swap_free(int slot) {
    /* 스왑 슬롯(slot)을 그냥 bitmap에서 해제 */
    lock_acquire(&swap_lock);
    bitmap_reset(swap_bitmap, slot);
    lock_release(&swap_lock);
}

