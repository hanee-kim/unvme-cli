---
type: Cython Binding Target
title: unvmed_cmd_prep / unvmed_cmd_post / unvmed_cmd_issue_and_wait (커맨드 제출)
description: SQE 준비, SQ posting, 도어벨 갱신, 동기 발행·대기, passthru까지 커맨드 제출 경로 API.
resource: lib/libunvmed.h::unvmed_cmd_post
tags: [libunvmed, upstream, io]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

## 시그니처

```c
/* @sqe를 cmd->sqe에 복사하고 @iov를 PRP로 매핑. thread-safe */
int unvmed_cmd_prep(struct unvme_cmd *cmd, union nvme_cmd *sqe,
		    struct iovec *iov, int nr_iov);

/* @sqe를 cmd->usq에 post. UNVMED_CMD_F_NODB면 도어벨 갱신 생략.
 * post 후 cmd->state = UNVME_CMD_S_SUBMITTED.
 * thread-safe 아님 — unvmed_sq_enter()로 잠근 상태에서 호출.
 * Return: queue entry index written */
uint16_t unvmed_cmd_post(struct unvme_cmd *cmd, union nvme_cmd *sqe,
			 unsigned long flags);

/* NODB로 post해둔 엔트리들의 tail 도어벨 일괄 갱신.
 * thread-safe 아님. Return: 발행된 SQ 엔트리 수 (0이면 없음) */
int unvmed_sq_update_tail(struct unvme *u, struct unvme_sq *usq);

/* prep된 cmd를 발행하고 완료까지 대기.
 * thread-safe 아님 — caller가 usq 락 확보 필요.
 * Return: 0 성공, -1 에러, 그 외 CQE status field */
int unvmed_cmd_issue_and_wait(struct unvme_cmd *cmd);

/* cmd->cqe와 함께 완료될 때까지 대기. Return: 0 / -1 (errno) */
int unvmed_cmd_wait(struct unvme_cmd *cmd);

/* 사용자 정의 커맨드 발행 (caller가 sqe 구성). thread-safe.
 * Return: 0 성공, 그 외 CQE status field */
int unvmed_passthru(struct unvme_cmd *cmd, union nvme_cmd *sqe,
		    struct iovec *iov, int nr_iov);
```

## 설명

- 제출 경로는 두 층으로 나뉜다:
  1. **prep + issue**: `unvmed_cmd_prep()`으로 SQE와 데이터 매핑을 준비한 뒤 `unvmed_cmd_issue_and_wait()`로 발행·완료 대기. 이는 각 커맨드별 `unvmed_cmd_prep_*()` 헬퍼들(identify, read/write 등)과 동일한 패턴이다.
  2. **저수준 post**: `unvmed_cmd_post()`로 SQE를 큐에 쓰고, `UNVMED_CMD_F_NODB` 플래그로 도어벨 갱신을 미룬 뒤 `unvmed_sq_update_tail()`로 여러 커맨드의 도어벨을 한 번에 울릴 수 있다 (batch 제출).
- 락 규약에 주의: `unvmed_cmd_post()`/`unvmed_sq_update_tail()`/`unvmed_cmd_issue_and_wait()`는 thread-safe가 아니므로 호출 전 해당 SQ의 락(`unvmed_sq_enter()`)을 잡아야 한다.
- `unvmed_passthru()`는 호출자가 직접 구성한 임의의 SQE를 발행하는 범용 API다.

## ctests에서의 사용 맥락

(추정) 대부분의 테스트는 커맨드별 헬퍼(`unvmed_read` 등)를 쓰겠지만, 도어벨 타이밍 제어 테스트(NODB로 여러 개 post 후 일괄 doorbell), 스펙에 없는/비정상 opcode 테스트(`unvmed_passthru`), 비동기 완료 테스트(post 후 별도 reap)에는 이 저수준 API들이 직접 바인딩되어야 한다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 커맨드 제출/완료 모델 (Submission Queue Tail Doorbell), 커맨드 포맷 (opcode, PSDT, DPTR)
- 관련 컨셉: `unvmed-cmd-alloc.md`, `unvmed-cq-reap.md`, `unvmed-prp-sgl.md`, `struct-unvme-sq-cq.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
