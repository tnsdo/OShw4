/* File: src/vm/page.c */

#include "vm/page.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"

/* pagedir_* 함수들을 쓰기 위해 */
#include "userprog/pagedir.h"

/* frame_alloc, frame_free, struct frame 사용을 위해 */
#include "vm/frame.h"

/* swap_in, swap_out, swap_free 함수 원형이 필요하면 */
#include "vm/swap.h"

#include <string.h>    /* memset */
#include <stdlib.h>    /* malloc, free */
#include <threads/synch.h>
#include <debug.h>

#include "userprog/syscall.h"
#include <stdbool.h>
#include <stdio.h>

#define STACK_LIMIT (1 << 20)

/* 전역 락: supplemental page table 조작 시 사용 */
static struct lock page_table_lock;
static bool page_table_lock_inited = false;

/* supplemental page table 초기화
   - t->sup_page_list를 빈 리스트로 설정
   - page_table_lock이 초기화되지 않았으면 초기화 */
void
sup_page_table_init (struct thread *t) {
    if (!page_table_lock_inited) {
        lock_init (&page_table_lock);
        page_table_lock_inited = true;
    }
    list_init (&t->sup_page_list);
}

/* supplemental page 엔트리 추가
   - uaddr: 페이지 내 어느 위치여도 상관없음; 내부에서 page 경계로 정렬
   - file/ofs/read_bytes/zero_bytes/writable 은 PAGE_FILE 또는 PAGE_MMAP 시 사용
   - PAGE_ZERO/PAGE_SWAP인 경우 file은 NULL, ofs/read_bytes/zero_bytes 는 0
   Returns true on success, false on OOM */
bool
sup_page_install (struct thread *t, void *uaddr,
                  enum page_type type,
                  struct file *file, off_t ofs,
                  size_t read_bytes, size_t zero_bytes,
                  bool writable) 
{
    struct sup_page *sp = malloc (sizeof *sp);
    if (sp == NULL)
        return false;

    sp->uaddr = pg_round_down (uaddr);
    sp->type = type;
    sp->file = file;
    sp->ofs = ofs;
    sp->read_bytes = read_bytes;
    sp->zero_bytes = zero_bytes;
    sp->writable = writable;
    sp->swap_slot = -1;

    lock_acquire (&page_table_lock);
    list_push_back (&t->sup_page_list, &sp->elem);
    lock_release (&page_table_lock);

    return true;
}

/* supplemental page 테이블에서 uaddr에 해당하는 엔트리 조회
   - uaddr가 속한 페이지 경계로 정렬하여 비교
   Returns pointer on found, NULL otherwise */
struct sup_page *
sup_page_lookup (struct thread *t, void *uaddr) {
    void *page_u = pg_round_down (uaddr);
    struct list_elem *e;

    lock_acquire (&page_table_lock);
    for (e = list_begin (&t->sup_page_list); e != list_end (&t->sup_page_list); e = list_next (e)) {
        struct sup_page *sp = list_entry (e, struct sup_page, elem);
        if (sp->uaddr == page_u) {
            lock_release (&page_table_lock);
            return sp;
        }
    }
    lock_release (&page_table_lock);
    return NULL;
}

/* 페이지 폴트 시 supplemental page 엔트리를 실제로 로드 및 매핑
   Returns true on success, false on failure */
bool
sup_page_load (struct thread *t, struct sup_page *sp) {
    struct frame *f;
    void *kpage;

    switch (sp->type) {
        case PAGE_FILE: {
            /* 1) 프레임 할당 */
            f = frame_alloc (sp->uaddr);
            if (f == NULL)
                return false;
            f->pinned = true;

            /* 2) 파일에서 read_bytes 만큼 읽고, zero_bytes 만큼 0으로 채움 */
            kpage = f->kpage;
            file_seek (sp->file, sp->ofs);
            if (file_read (sp->file, kpage, sp->read_bytes) != (int) sp->read_bytes) {
                frame_free (f);
                return false;
            }
            memset (kpage + sp->read_bytes, 0, sp->zero_bytes);

            /* 3) pagedir에 매핑 */
            if (!pagedir_set_page (t->pagedir, sp->uaddr, kpage, sp->writable)) {
                frame_free (f);
                return false;
            }

            /* 4) 프레임 구조체 정보 갱신 후 unpin */
            f->uaddr = sp->uaddr;
            f->t = t;
            f->pinned = false;
            return true;
        }

        case PAGE_ZERO: {
            /* 1) 프레임 할당 */
            f = frame_alloc (sp->uaddr);
            if (f == NULL)
                return false;
            kpage = f->kpage;

            /* 2) 페이지 전체를 0으로 채움 */
            memset (kpage, 0, PGSIZE);

            /* 3) pagedir에 매핑 */
            if (!pagedir_set_page (t->pagedir, sp->uaddr, kpage, sp->writable)) {
                frame_free (f);
                return false;
            }

            f->uaddr = sp->uaddr;
            f->t = t;
            f->pinned = false;
            return true;
        }

        case PAGE_SWAP: {
            /* 1) 프레임 할당 */
            f = frame_alloc (sp->uaddr);
            if (f == NULL)
                return false;
            kpage = f->kpage;

            /* 2) swap 슬롯에서 페이지 읽어옴 */
            swap_in (sp->swap_slot, kpage);
            sp->swap_slot = -1;

            /* 3) pagedir에 매핑 */
            if (!pagedir_set_page (t->pagedir, sp->uaddr, kpage, sp->writable)) {
                frame_free (f);
                return false;
            }

            f->uaddr = sp->uaddr;
            f->t = t;
            f->pinned = false;
            return true;
        }

        case PAGE_MMAP: {
            /* 1) 프레임 할당 */
            f = frame_alloc(sp->uaddr);
            if (f == NULL)
                return false;
            f->pinned = true;
            kpage = f->kpage;

            /* 2) 파일에서 read_bytes만큼 읽고, zero_bytes만큼 0으로 채움 */
            file_seek(sp->file, sp->ofs);
            if (file_read(sp->file, kpage, sp->read_bytes) != (int) sp->read_bytes) {
                frame_free(f);
                return false;
            }
            memset(kpage + sp->read_bytes, 0, sp->zero_bytes);

            /* 3) 페이지 디렉토리에 매핑 */
            if (!pagedir_set_page(t->pagedir, sp->uaddr, kpage, sp->writable)) {
                frame_free(f);
                return false;
            }

            /* 4) 프레임 구조체 정보 갱신 후 unpin */
            f->uaddr = sp->uaddr;
            f->t = t;
            f->pinned = false;
            return true;
        }
    }

    /* 해당하지 않으면 실패 */
    return false;
}

/* 스왑 아웃 이후 supplemental page 엔트리에 swap_slot 정보 업데이트 */
void
sup_page_update_swap (struct thread *t, void *uaddr, int slot) {
    struct sup_page *sp = sup_page_lookup (t, uaddr);
    if (sp) {
        sp->type = PAGE_SWAP;
        sp->swap_slot = slot;
    }
}

/* 특정 supplemental page 엔트리만 제거
   - swap 슬롯 반환 로직은 sup_page_destroy에서 처리하도록 함 */
void
sup_page_remove (struct thread *t, struct sup_page *sp) {
    lock_acquire (&page_table_lock);
    list_remove (&sp->elem);
    lock_release (&page_table_lock);
    free (sp);
}

/* 프로세스 종료 시 supplemental page 테이블 전체 정리
   - swap 슬롯이 남아 있으면 free하고, 리스트에서 제거 후 해제 */
void
sup_page_destroy (struct thread *t) {
    struct list_elem *e = list_begin (&t->sup_page_list);
    while (e != list_end (&t->sup_page_list)) {
        struct sup_page *sp = list_entry (e, struct sup_page, elem);
        e = list_next (e);

        if (sp->type == PAGE_SWAP && sp->swap_slot != -1) {
            swap_free (sp->swap_slot);
        }

        list_remove (&sp->elem);
        free (sp);
    }
}


/* stack growth heuristic + allocation */
bool
vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write, bool not_present)
{
  struct thread *t = thread_current();

  if (user && not_present) {
    void *esp = (void*) t->saved_esp;

    if ((uint8_t*)addr >= (uint8_t*)esp - 32 &&
        (uint8_t*)addr >= (uint8_t*)(PHYS_BASE - STACK_LIMIT) &&
        (uint8_t*)addr < PHYS_BASE) {
      vm_stack_growth(addr);
      return true;
    }
  }

  return false;
}

void
vm_stack_growth(void *addr)
{
  void *page_addr = pg_round_down(addr);

  if ((uint8_t*)page_addr < (uint8_t*)(PHYS_BASE - STACK_LIMIT))
    return;  // beyond limit

  sup_page_install(thread_current(),
                   page_addr,
                   PAGE_ZERO,
                   NULL, 0, 0, PGSIZE,
                   true);
}

