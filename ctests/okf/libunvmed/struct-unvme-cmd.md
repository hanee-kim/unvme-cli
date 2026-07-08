---
type: Cython Binding Target
title: struct unvme_cmd
description: NVMe 커맨드 1건의 상태·버퍼·SQE/CQE를 담는 커맨드 인스턴스 구조체와 상태/플래그 enum.
resource: lib/libunvmed.h::struct unvme_cmd
tags: [libunvmed, upstream, io]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
enum unvme_cmd_state {
	UNVME_CMD_S_INIT		= 0,
	UNVME_CMD_S_ALLOCATED,
	UNVME_CMD_S_SUBMITTED,
	UNVME_CMD_S_COMPLETED,
	UNVME_CMD_S_TO_BE_COMPLETED,
};

enum unvmed_cmd_flags {
	/* No doorbell update after posting one or more commands */
	UNVMED_CMD_F_NODB	= 1 << 0,
	/* Use SGL in DPTR instead of PRP */
	UNVMED_CMD_F_SGL	= 1 << 1,
	/* To wake up unvmed_cmd_wait(cmd) on CQE being fetched. */
	UNVMED_CMD_F_WAKEUP_ON_CQE = 1 << 2,
	/* Command has been timed out */
	UNVMED_CMD_F_TIMEDOUT	= 1 << 3,
};

struct unvme_buf {
	void *va;
	size_t len;
#define UNVME_CMD_BUF_F_VA_UNMAP	(1 << 0)
#define UNVME_CMD_BUF_F_IOVA_UNMAP	(1 << 1)
	unsigned int flags;
	struct iovec iov;	/* 버퍼 전체를 나타내는 인라인 단일 iovec */
};

struct unvme_cmd {
	struct unvme *u;

	int refcnt;
	enum unvme_cmd_state state;
	unsigned long flags;

	struct unvme_sq *usq;
	uint32_t vcq;		/* != 0 이면 해당 qid의 vcq로 CQE가 push됨 */
	uint16_t cid;

	struct nvme_rq *rq;	/* rq->opaque는 이 구조체를 가리킴 */

	struct unvme_buf buf;
	struct unvme_buf mbuf;	/* DIX 모드에서만 유효한 메타데이터 버퍼 */

	void *opaque;

	bool completed;		/* futex WAIT 기반 완료 통지 (RCQ 모드) */
	struct timespec timeout;

	union nvme_cmd sqe;	/* 이 커맨드로 push된 SQE */
	struct nvme_cqe cqe;	/* RCQ 모드(인터럽트)에서 reap된 CQE */
};
```

## 설명

- 커맨드 1건의 전 생명주기(INIT → ALLOCATED → SUBMITTED → COMPLETED)를 담는 구조체. `unvmed_alloc_cmd()` 계열로 할당하고 `unvmed_cmd_put()`으로 해제한다.
- `buf`/`mbuf`는 DPTR에 매핑될 데이터/메타데이터 버퍼이며, `UNVME_CMD_BUF_F_VA_UNMAP`/`UNVME_CMD_BUF_F_IOVA_UNMAP` 플래그에 따라 `unvmed_cmd_free()` 시점에 버퍼/IOVA 매핑이 해제된다.
- `flags`(enum unvmed_cmd_flags)는 제출 동작을 제어한다: `UNVMED_CMD_F_NODB`(도어벨 갱신 생략, batch 제출용), `UNVMED_CMD_F_SGL`(PRP 대신 SGL 사용), `UNVMED_CMD_F_WAKEUP_ON_CQE`(CQE 수신 시 `unvmed_cmd_wait()` 깨움), `UNVMED_CMD_F_TIMEDOUT`(타임아웃 발생 표시).
- `vcq` 필드가 0이 아니면 이 커맨드의 CQE는 해당 qid의 가상 CQ(vcq)로 push된다 (`unvmed_cmd_get_vcq()` 참고).
- 헬퍼: `unvmed_get_cmd(usq, cid)` — 상태가 INIT이 아니면 `&usq->cmds[cid]` 반환, INIT이면 NULL.

## ctests에서의 사용 맥락

(추정) 모든 커맨드 발행 테스트의 기본 단위. Cython 래퍼에서 Command 클래스가 `struct unvme_cmd *`를 보유하고, `sqe`/`cqe` 필드를 읽어 검증(예: 상태 코드, DW0 결과값 확인)하는 패턴이 예상된다. `opaque`는 파이썬 객체 참조를 매달아 비동기 완료 시 콜백 컨텍스트로 쓸 수 있다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Submission Queue Entry / Completion Queue Entry 포맷, PRP/SGL 데이터 포인터
- 관련 컨셉: `unvmed-cmd-alloc.md`, `unvmed-cmd-submit.md`, `unvmed-cmd-status.md`, `unvmed-prp-sgl.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
