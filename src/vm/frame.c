/* File: src/vm/frame.c */
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include <debug.h>

static struct list frame_list;            /* 모든 프레임을 연결 */
static struct list_elem *clock_hand;      /* Clock 알고리즘용 손잡이 */
static struct lock frame_lock;            /* frame_list 보호용 락 */

void
frame_table_init (void) {
    list_init (&frame_list);
    lock_init (&frame_lock);
    clock_hand = NULL;

    swap_init();
}

/* 내부: 빈 프레임을 palloc_get_page()로 얻어옴 */
static void *
get_new_kpage (void) {
    return palloc_get_page (PAL_USER);
}

/* frame_free: 프레임 해제 */
void
frame_free (struct frame *f) {
    ASSERT (f != NULL);

    lock_acquire (&frame_lock);
    list_remove (&f->elem);
    lock_release (&frame_lock);

    palloc_free_page (f->kpage);
    free (f);
}

/* evict_frame: 교체 대상인 victim을 실제로 스왑/매핑 해제 후 해제 */
void
evict_frame (struct frame *victim) {
    ASSERT (victim != NULL);
    ASSERT (victim->pinned == false);

    uint32_t *pd = victim->t->pagedir;
    void *uaddr = victim->uaddr;

    /* 1) dirty 여부 검사 */
    if (pagedir_is_dirty (pd, uaddr)) {
        /* Dirty면 swap으로 내보냄 */
        int slot = swap_out (victim->kpage);
        /* SPT 업데이트: 이제 이 가상 페이지는 swap에 있음 */
        sup_page_update_swap (victim->t, uaddr, slot);
    }
    /* 2) 유저 페이지 디렉토리 엔트리 해제 (present 비트 0) */
    pagedir_clear_page (pd, uaddr);

    /* 3) frame_list에서 victim 엘리먼트 제거, 물리 페이지 그대로 재사용 */
    lock_acquire (&frame_lock);
    list_remove (&victim->elem);
    lock_release (&frame_lock);
    /* victim 구조체는 해제하지 않고, 새로운 페이지 할당 시 재사용할 kpage만 남김 */
}

/* select_victim_frame: Clock(Second-Chance) 알고리즘 */
struct frame *
select_victim_frame (void) {
    struct frame *victim = NULL;

    lock_acquire (&frame_lock);

    /* 처음 호출 시 clock_hand 설정 */
    if (clock_hand == NULL) {
        if (list_empty (&frame_list)) {
            lock_release (&frame_lock);
            return NULL;
        }
        clock_hand = list_begin (&frame_list);
    }

    /* frame_list를 한 바퀴 돌면서 victim 찾기 */
    struct list_elem *start = clock_hand;
    do {
        struct frame *f = list_entry (clock_hand, struct frame, elem);

        /* pinned된 프레임은 건너뜀 */
        if (!f->pinned) {
            uint32_t *pd = f->t->pagedir;
            void *uaddr = f->uaddr;

            /* Accessed 비트 검사 */
            if (pagedir_is_accessed (pd, uaddr)) {
                /* Accessed 비트가 1: second chance */
                pagedir_set_accessed (pd, uaddr, false);
            } else {
                /* Accessed 비트가 0: 이걸 victim으로 선택 */
                victim = f;
                break;
            }
        }

        /* 다음 엘리먼트로 이동 (리스트 끝이면 처음으로) */
        clock_hand = list_next (clock_hand);
        if (clock_hand == list_end (&frame_list))
            clock_hand = list_begin (&frame_list);

    } while (clock_hand != start);

    /* victim이 없다면(모두 pinned or 모두 accessed==true인 경우), 
       방금 순회 중 Accessed 비트를 모두 clear했다면 다시 한 번 순회 */
    if (victim == NULL) {
        /* Accessed 비트 모두 clear 후, 첫 번째 비Pinned 찾기 */
        struct list_elem *elem = clock_hand;
        do {
            struct frame *f = list_entry (elem, struct frame, elem);
            if (!f->pinned) {
                victim = f;
                break;
            }
            elem = list_next (elem);
            if (elem == list_end (&frame_list))
                elem = list_begin (&frame_list);
        } while (elem != clock_hand);
    }

    /* clock_hand를 victim 다음으로 옮겨 둠 (다음 호출 시 이어서 순회) */
    if (victim != NULL) {
        struct list_elem *next = list_next (&victim->elem);
        if (next == list_end (&frame_list))
            clock_hand = list_begin (&frame_list);
        else
            clock_hand = next;
    }

    lock_release (&frame_lock);
    return victim;
}

/* frame_alloc: 새 프레임 요청 */
struct frame *
frame_alloc (void *uaddr) {
    void *kpage = get_new_kpage ();
    if (kpage == NULL) {
        /* 물리 메모리 부족: eviction */
        struct frame *victim = select_victim_frame ();
        if (victim == NULL)
            return NULL;

        /* victim을 스왑/매핑 해제 */
        evict_frame (victim);

        /* evict_frame 후 사용 가능한 물리 페이지는 victim->kpage */
        kpage = victim->kpage;
        /* victim 구조체 자체를 재활용 */
        victim->uaddr = uaddr;
        victim->t = thread_current ();
        victim->pinned = false;

        /* 새로운 위치(현재 스레드)에 붙이기 위해 리스트에서 다시 삽입 */
        lock_acquire (&frame_lock);
        list_push_back (&frame_list, &victim->elem);
        lock_release (&frame_lock);

        return victim;
    }

    /* 정상적으로 새 페이지를 할당받은 경우 */
    struct frame *f = malloc (sizeof *f);
    if (f == NULL) {
        palloc_free_page (kpage);
        return NULL;
    }

    f->kpage = kpage;
    f->uaddr = uaddr;
    f->t = thread_current ();
    f->pinned = false;

    lock_acquire (&frame_lock);
    list_push_back (&frame_list, &f->elem);
    lock_release (&frame_lock);

    /* clock_hand이 NULL이면 첫 할당 시 설정 */
    if (clock_hand == NULL)
        clock_hand = list_begin (&frame_list);

    return f;
}


struct frame *
frame_lookup_by_kpage (const void *kpage) {
    struct list_elem *e;
    for (e = list_begin(&frame_list); e != list_end(&frame_list); e = list_next(e)) {
        struct frame *f = list_entry(e, struct frame, elem);
        if (f->kpage == kpage)
            return f;
    }
    return NULL;
}

