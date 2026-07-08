---
type: Cython Binding Target
title: unvmed_cmd_prep_create_sq / delete_sq / create_cq / delete_cq (I/O 큐 admin 커맨드 prep)
description: Create/Delete I/O SQ·CQ admin 커맨드의 SQE를 준비하는 저수준 prep API.
resource: lib/libunvmed.h::unvmed_cmd_prep_create_sq
tags: [libunvmed, upstream, admin, io]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/* 모두 0 성공 / -1 실패 (errno 설정) */
int unvmed_cmd_prep_create_sq(struct unvme_cmd *cmd, struct unvme *u,
			      uint32_t qid, uint32_t qsize, uint32_t cqid,
			      uint32_t qprio, uint32_t pc, uint32_t nvmsetid);

int unvmed_cmd_prep_delete_sq(struct unvme_cmd *cmd, uint32_t qid);

int unvmed_cmd_prep_create_cq(struct unvme_cmd *cmd, struct unvme *u,
			      uint32_t qid, uint32_t qsize, int vector, uint32_t pc);

int unvmed_cmd_prep_delete_cq(struct unvme_cmd *cmd, uint32_t qid);
```

## 설명

- admin SQ에서 발행할 Create/Delete I/O SQ·CQ 커맨드의 SQE를 준비(prep)만 하는 API다. 실제 발행은 `unvmed_cmd_issue_and_wait()` 등 제출 경로로 수행한다.
- Create 계열은 앞서 `unvmed_init_sq()`/`unvmed_init_cq()`로 큐 메모리와 드라이버 인스턴스를 준비해 둔 상태를 전제로 하며 (헤더의 "This separates libvfn operations from command prep" 주석 참고), 커맨드 성공 후 `unvmed_enable_sq()`/`unvmed_enable_cq()`로 활성화, 실패 시 `unvmed_free_sq()`/`unvmed_free_cq()`로 정리하는 흐름이다.
- 파라미터는 스펙의 커맨드 필드와 대응: `qid`/`qsize`, SQ의 `cqid`(연결 CQ)·`qprio`(우선순위)·`nvmsetid`, CQ의 `vector`(인터럽트 벡터), 공통 `pc`(Physically Contiguous).
- 원샷 헬퍼 `unvmed_create_sq()`/`unvmed_create_cq()`와 달리 이 prep API들은 커맨드 발행 시점·순서를 테스트가 직접 제어할 수 있게 한다.

## ctests에서의 사용 맥락

(추정) Create/Delete I/O Queue 커맨드 자체를 검증하는 테스트(비정상 qid, qsize 경계, PC=0, 잘못된 cqid 참조 등 negative 케이스 포함)에서 prep → 발행 → CQE status 검증의 형태로 사용될 것이다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Create I/O Submission Queue, Delete I/O Submission Queue, Create I/O Completion Queue, Delete I/O Completion Queue admin 커맨드
- 관련 컨셉: `unvmed-io-queues.md`, `unvmed-cmd-submit.md`, `unvmed-cmd-status.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
