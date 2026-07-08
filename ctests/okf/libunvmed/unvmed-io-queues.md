---
type: Cython Binding Target
title: unvmed_create_cq / unvmed_create_sq 계열 (I/O 큐 생성·관리)
description: I/O SQ/CQ의 생성·초기화·enable/disable·해제·조회 API 모음.
resource: lib/libunvmed.h::unvmed_create_sq
tags: [libunvmed, upstream, io]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

## 시그니처

```c
/* 원샷 생성 (커맨드 발행 포함). thread-safe */
int unvmed_create_cq(struct unvme *u, uint32_t qid, uint32_t qsize, int vector,
		     uint32_t pc);
int unvmed_create_sq(struct unvme *u, uint32_t qid, uint32_t qsize,
		     uint32_t cqid, uint32_t qprio, uint32_t pc, uint32_t nvmsetid);

/* libvfn 구성 + usq/ucq 초기화 (커맨드 prep과 분리된 단계) */
struct unvme_sq *unvmed_init_sq(struct unvme *u, uint32_t qid, uint32_t qsize,
				uint32_t cqid, int qprio, int pc, int nvmsetid);
struct unvme_sq *unvmed_init_sq_iova(struct unvme *u, uint32_t qid, uint32_t qsize,
				     uint32_t cqid, int qprio, int pc, int nvmsetid,
				     uint64_t iova);	/* pre-mapped 버퍼 사용 */
struct unvme_cq *unvmed_init_cq(struct unvme *u, uint32_t qid, uint32_t qsize,
				int vector, int pc);
struct unvme_cq *unvmed_init_cq_iova(struct unvme *u, uint32_t qid, uint32_t qsize,
				     int vector, int pc, uint64_t iova);

/* Create 커맨드 성공 후 최종 enable / atomic disable */
void unvmed_enable_sq(struct unvme_sq *usq);
void unvmed_disable_sq(struct unvme_sq *usq);
void unvmed_enable_cq(struct unvme_cq *ucq);
void unvmed_disable_cq(struct unvme_cq *ucq);

/* 커맨드 실패 시 usq/ucq + libvfn 큐 정리 */
int unvmed_free_sq(struct unvme *u, uint16_t qid);
int unvmed_free_cq(struct unvme *u, uint16_t qid);

/* 조회 */
int unvmed_get_sqs(struct unvme *u, struct unvme_sq ***sqs);   /* caller가 free */
int unvmed_get_cqs(struct unvme *u, struct unvme_cq ***cqs);   /* caller가 free */
struct unvme_sq *unvmed_sq_get(struct unvme *u, uint32_t qid); /* refcnt++ */
struct unvme_cq *unvmed_cq_get(struct unvme *u, uint32_t qid); /* refcnt++ */
int unvmed_sq_put(struct unvme *u, struct unvme_sq *usq);      /* refcnt--, 0이면 해제 */
int unvmed_cq_put(struct unvme *u, struct unvme_cq *ucq);
struct unvme_sq *unvmed_sq_find(struct unvme *u, uint32_t qid); /* refcnt 무변경 */
struct unvme_cq *unvmed_cq_find(struct unvme *u, uint32_t qid);
```

## 설명

- **원샷 경로**: `unvmed_create_cq()`/`unvmed_create_sq()`는 I/O CQ/SQ를 생성하는 상위 레벨 API다 (0 성공 / -1 실패, errno 설정). CQ의 `vector`에 -1을 주면 인터럽트 비활성화.
- **분리 경로**: `unvmed_init_sq/cq[_iova]()`는 libvfn 큐 구성과 usq/ucq 초기화를 커맨드 준비(prep)와 분리해 수행한다. `_iova` 변형은 미리 IOMMU에 매핑된 버퍼(IOVA)를 큐 메모리로 사용한다 (CMB 상의 큐 등). Create I/O SQ/CQ 커맨드가 성공하면 `unvmed_enable_sq()/unvmed_enable_cq()`로 최종 활성화하고, 커맨드가 실패하면 `unvmed_free_sq()/unvmed_free_cq()`로 정리한다.
  - 헤더의 XXX 주석: `unvmed_enable_sq()`는 원래 라이브러리 내부에서만 호출되어야 하나 현재 애플리케이션 영역에서 호출되고 있어 수정 예정이라고 명시됨.
- **조회**: `unvmed_get_sqs()/unvmed_get_cqs()`는 생성된 큐 목록을 할당해 반환하므로 호출자가 free해야 한다. `*_get()/*_put()`은 refcnt 기반 소유, `*_find()`는 refcnt 변경 없는 단순 조회.

## ctests에서의 사용 맥락

(추정) I/O 테스트 fixture의 표준 준비 단계: CQ 생성 → SQ 생성(cqid 연결) → 테스트 후 teardown. 단순 테스트는 원샷 `unvmed_create_*`를, Create/Delete 커맨드 자체를 검증하는 테스트는 분리 경로(`unvmed_init_*` + `unvmed_cmd_prep_create_*` + enable/free)를 쓸 것으로 보인다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Create/Delete I/O Submission Queue, Create/Delete I/O Completion Queue admin 커맨드 (QID, QSIZE, PC, QPRIO, IV/IEN, NVMSETID 필드)
- 관련 컨셉: `struct-unvme-sq-cq.md`, `unvmed-admin-queue-cmds.md`, `unvmed-cmd-submit.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
