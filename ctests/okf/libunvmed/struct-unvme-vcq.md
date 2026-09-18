---
type: Cython Binding Target
title: struct unvme_vcq / unvmed_vcq_*
description: 하드웨어 CQ에서 reap한 CQE를 애플리케이션(스레드)별로 분배하는 가상 완료 큐(Virtual CQ) 구조체와 API.
resource: lib/libunvmed.h::struct unvme_vcq
tags: [libunvmed, upstream, io]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/*
 * struct unvme_vcqe - Virtual CQ entry wrapping an NVMe CQE
 * 32바이트(2의 거듭제곱)로 맞춰 64바이트 캐시라인에 2개 엔트리가
 * 정확히 들어가도록 설계됨 (캐시라인 split 방지)
 */
struct unvme_vcqe {
	struct nvme_cqe cqe;	/* 표준 NVMe CQE (16바이트) */
	uint32_t bdf;		/* (domain<<16)|(bus<<8)|(device<<3)|func */
	uint8_t rsvd[12];
};

struct unvme_vcq {
	uint32_t qid;
	int qsize;
	uint16_t head;
	uint16_t tail;
	pthread_spinlock_t tail_lock;
	struct unvme_vcqe *vcqe;	/* qsize 만큼의 VCQE 배열 */
};

struct unvme_vcq *unvmed_vcq_get(uint32_t qid);
int unvmed_vcq_init(struct unvme_vcq *vcq, uint32_t qsize, uint32_t *qid);
void unvmed_vcq_free(struct unvme_vcq *vcq);
static inline struct unvme_vcq *unvmed_cmd_get_vcq(struct unvme_cmd *cmd);
int unvmed_vcq_pop(struct unvme_vcq *q, struct unvme_vcqe *vcqes);
void unvmed_vcq_drain(struct unvme_vcq *vcq);
int unvmed_vcq_run_n(struct unvme *u, struct unvme_vcq *vcq,
		     struct unvme_vcqe *vcqes, int min, int max);

/* @cmd에 연결된 vcq로 push. cmd->state는 변경하지 않음.
 * Return: 0 성공, 그 외 errno */
int unvmed_vcq_push(struct unvme_cmd *cmd, struct nvme_cqe *cqe);

/* (sqid, cid)로 커맨드를 찾아 그 vcq로 push (다른 스레드의 커맨드용).
 * cmd->state를 SUBMITTED → TO_BE_COMPLETED로 원자적으로 전이한 후 push.
 * CQ head 도어벨 갱신 전에 호출해야 하며 @cqe는 CQ 안의 포인터를
 * 복사 없이 그대로 전달해야 함. state가 SUBMITTED가 아니면 실패.
 * Return: 0 성공, 그 외 errno */
int unvmed_vcq_push_to_other(struct unvme *u, struct nvme_cqe *cqe);
```

## 설명

- 가상 CQ(vcq)는 실제 하드웨어 CQ와 별개로, reap된 CQE를 커맨드 소유자(예: 제출 스레드)에게 전달하기 위한 소프트웨어 큐다. 각 SQ는 자체 `vcq`를 내장하며(`usq->vcq`), 애플리케이션이 `unvmed_vcq_init()`으로 자체 vcq를 등록할 수도 있다.
- 헤더 주석 기준 동작:
  - `unvmed_vcq_init(vcq, qsize, &qid)` — 애플리케이션용 vcq 초기화, 할당된 qid를 출력. 성공 0, 실패 -1 (errno 설정).
  - `unvmed_vcq_get(qid)` — qid로 vcq 조회, 없으면 NULL.
  - `unvmed_cmd_get_vcq(cmd)` — `cmd->vcq`가 설정돼 있으면 그 qid의 vcq, 아니면 `&cmd->usq->vcq` 반환.
  - `unvmed_vcq_push(cmd, cqe)` — `cmd->vcq`에 연결된 vcq로 CQE를 push. CQ reaping 경로가 제출 스레드에 완료를 전달할 때 내부적으로 사용하며, `cmd->state`는 변경하지 않는다 (상태 전이는 호출자 책임). thread-safe.
  - `unvmed_vcq_push_to_other(u, cqe)` — (sqid, cid)가 일치하는 다른 스레드의 커맨드를 찾아 그 vcq로 push. push 전에 `cmd->state`를 SUBMITTED → TO_BE_COMPLETED로 원자적으로 전이하며, state가 SUBMITTED가 아니면 에러를 반환한다. CQ head 도어벨 갱신 전에, CQ 내부의 CQE 포인터를 복사 없이 그대로 넘겨 호출해야 한다. thread-safe.
  - `unvmed_vcq_pop(q, vcqes)` — vcqe 1개 pop. 소비자는 단일 스레드 전제(head 쪽 락 불필요). 비어 있으면 `-ENOENT`.
  - `unvmed_vcq_run_n(u, vcq, vcqes, min, max)` — 최소 min(필수)~최대 max(best effort)개 fetch, 개수 반환.
  - `unvmed_vcq_drain(vcq)` — 모든 엔트리가 소비될 때까지 busy-wait.
- CQE가 vcq로 push되는 주체는 irq 모드에 따라 다르다: irq 활성 시 rcq 스레드, 비활성 시 owner 스레드 또는 먼저 reap한 다른 스레드 (`struct unvme_cmd`의 `vcq` 필드 주석 참고).

## ctests에서의 사용 맥락

(추정) 멀티스레드/비동기 I/O 테스트에서 스레드별 완료 수신 채널로 사용될 수 있다. 단순 동기 테스트에서는 `unvmed_cmd_issue_and_wait()` 경로가 내부적으로 이 메커니즘을 사용하므로 직접 바인딩하지 않아도 될 수 있다.

> TODO: ctests가 vcq를 직접 노출할지, issue_and_wait 경로만 쓸지는 바인딩 설계 시 upstream 코드로 재확인 필요

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Completion Queue Entry (vcqe가 표준 CQE를 그대로 포함)
- 관련 컨셉: `unvmed-cq-reap.md`, `struct-unvme-cmd.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
