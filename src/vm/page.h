/* File: src/vm/page.h */
#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdbool.h>
#include <list.h>
#include <filesys/file.h>
#include <threads/thread.h>
#include "userprog/exception.c"


/* 페이지 종류 구분 */
enum page_type {
    PAGE_FILE,   /* 실행 파일이나 mmap 파일에서 읽을 때 */
    PAGE_ZERO,   /* 스택 확장 등 제로 페이지 */
    PAGE_SWAP,   /* 스왑에서 불러올 때 */
    PAGE_MMAP    /* mmap된 파일 페이지 */
};

/* Supplemental Page Table 엔트리 구조체 */
struct sup_page {
    void *uaddr;             /* 페이지 정렬된 사용자 가상 주소 */
    enum page_type type;     /* 이 페이지가 어디서 온 건지 */
    struct file *file;       /* type == PAGE_FILE 또는 PAGE_MMAP 시 사용 */
    off_t ofs;               /* 파일 오프셋 */
    size_t read_bytes;       /* 파일에서 읽어야 할 바이트 수 */
    size_t zero_bytes;       /* 그 외 나머지를 0으로 채울 바이트 수 */
    bool writable;           /* 쓰기 가능 여부 */
    int swap_slot;           /* 스왑 슬롯 (없으면 -1) */
    struct list_elem elem;   /* 프로세스별 리스트 연결용 */
};

/* 보조 페이지 테이블 관련 함수들 */

/* 프로세스의 supplemental page table 리스트 초기화 */
void sup_page_table_init(struct thread *t);

/* 새로운 supplemental page 엔트리 추가
   - t: 대상 프로세스 스레드
   - uaddr: 페이지 정렬된 유저 가상 주소
   - type: PAGE_FILE / PAGE_ZERO / PAGE_SWAP / PAGE_MMAP
   - file, ofs, read_bytes, zero_bytes, writable: PAGE_FILE 또는 PAGE_MMAP에서 사용
   Returns true on success, false on failure. */
bool sup_page_install(struct thread *t, void *uaddr,
                      enum page_type type,
                      struct file *file, off_t ofs,
                      size_t read_bytes, size_t zero_bytes,
                      bool writable);

/* 특정 가상 주소에 대응하는 supplemental page 엔트리 조회
   - t: 대상 프로세스 스레드
   - uaddr: 조회할 가상 주소 (페이지 내 어느 위치여도 괜찮음)
   Returns pointer to sup_page if found, or NULL otherwise. */
struct sup_page *sup_page_lookup(struct thread *t, void *uaddr);

/* 페이지 폴트 시 supplemental page 엔트리에 따라 실제 페이지 할당 및 매핑 수행
   - t: 대상 프로세스 스레드
   - sp: supplemental page 엔트리
   Returns true on successful load & mapping, false on failure. */
bool sup_page_load(struct thread *t, struct sup_page *sp);

/* 스왑 아웃 이후 supplemental page 엔트리에 swap_slot 정보 업데이트
   - t: 대상 프로세스 스레드
   - uaddr: 해당 페이지 가상 주소
   - slot: 할당된 스왑 슬롯 번호 */
void sup_page_update_swap(struct thread *t, void *uaddr, int slot);

/* 특정 supplemental page 엔트리 제거
   - t: 대상 프로세스 스레드
   - sp: 제거할 supplemental page 엔트리 */
void sup_page_remove(struct thread *t, struct sup_page *sp);

/* 프로세스 종료 시 supplemental page 테이블 전체 해제
   - t: 대상 프로세스 스레드
   Releases any swap slots and frees all supplemental page entries. */
void sup_page_destroy(struct thread *t);


/* stack growth */
struct intr_frame;
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write, bool not_present);

void vm_stack_growth(void *addr);
#endif /* VM_PAGE_H */

