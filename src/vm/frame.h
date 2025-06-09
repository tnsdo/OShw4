/* File: src/vm/frame.h */
#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include <stdbool.h>
#include "threads/thread.h"
#include "threads/synch.h"

/* 한 물리 페이지(프레임)에 대응되는 구조체 */
struct frame {
    void *kpage;             /* palloc_get_page(PAL_USER)로 받은 커널 물리 주소 */
    void *uaddr;             /* 매핑된 사용자 가상 주소 (SPT의 uaddr와 일치) */
    struct thread *t;        /* 이 프레임을 소유한 스레드 */
    bool pinned;             /* I/O 중이거나 페이지 폴트 처리 중이면 true (교체 불가) */
    struct list_elem elem;   /* 전역 frame_list에 연결하기 위한 엘리먼트 */
};

/* 프레임 테이블 초기화 (Boot 시 한 번 호출) */
void frame_table_init (void);

/* 새 페이지를 할당할 때 호출
   - uaddr: 이 프레임이 매핑될 사용자 가상 주소
   - 성공하면 struct frame* 반환, 실패 시 NULL */
struct frame *frame_alloc (void *uaddr);

/* 프레임을 완전히 해제 (페이지 파일 혹은 swap에서 교체된 뒤 호출) */
void frame_free (struct frame *f);

/* Clock 알고리즘으로 교체할 victim 프레임을 선택 */
struct frame *select_victim_frame (void);

/* victim 프레임을 스왑으로 내보내고 SPT를 업데이트 */
void evict_frame (struct frame *victim);

struct frame *frame_lookup_by_kpage(const void *kpage);

#endif /* VM_FRAME_H */

