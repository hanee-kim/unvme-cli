---
type: Cython Binding Target
title: struct unvme_sq / struct unvme_cq
description: libvfn 큐를 감싸는 libunvmed의 제출 큐(SQ)/완료 큐(CQ) 인스턴스 구조체와 접근자 매크로.
resource: lib/libunvmed.h::unvme_declare_sq
tags: [libunvmed, upstream, io]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
#define unvme_declare_sq(name)	\
struct name {			\
	int id;			\
	int qsize;		\
	int qprio;		\
	int pc;			\
	int nvmsetid;		\
	unsigned int flags;	\
				\
	struct nvme_sq *q;	\
	struct unvme_cq *ucq;	\
	struct unvme_cmd *cmds; \
	struct unvme_timer timer;	\
	struct unvme_vcq vcq;	\
	struct unvme_bitmap cids;\
	int nr_cmds;		\
	uint64_t cmd_count[CMD_COUNT_RANGE];	\
	pthread_spinlock_t lock;\
	bool enabled;		\
	int refcnt;		\
}

#define unvme_declare_cq(name)	\
struct name {			\
	int id;			\
	int qsize;		\
	int vector;		\
	int pc;			\
				\
	struct unvme *u;	\
	struct nvme_cq *q;	\
	struct unvme_sq *usq;	\
	pthread_spinlock_t lock;\
	bool enabled;		\
	int refcnt;		\
}

enum unvme_sq_flags {
	/* SQ is frozen due to command timeout */
	UNVMED_SQ_F_FROZEN	= 1 << 0,
};

/* 접근자 매크로 */
#define unvmed_cq_id(ucq)	((ucq)->id)
#define unvmed_cq_size(ucq)	((ucq)->qsize)
#define unvmed_cq_iv(ucq)	((ucq)->vector)
#define unvmed_sq_id(usq)	((usq)->id)
#define unvmed_sq_size(usq)	((usq)->qsize)
#define unvmed_sq_cqid(usq)	(unvmed_cq_id((usq)->ucq))
```

## 설명

- `struct unvme_sq`는 libvfn의 `struct nvme_sq`(`q` 필드)를 감싸는 인스턴스로, 연결된 CQ(`ucq`), 커맨드 인스턴스 배열(`cmds`, `nr_cmds`), opcode별 커맨드 카운트(`cmd_count`, 범위는 `CMD_COUNT_RANGE`=255), CID 할당용 비트맵(`cids`), 스핀락(`lock`), 활성화 플래그(`enabled`), 참조 카운트(`refcnt`)를 가진다.
- `struct unvme_cq`는 libvfn의 `struct nvme_cq`를 감싸며, 컨트롤러(`u`), 인터럽트 벡터(`vector`), 연결된 SQ(`usq`) 등을 가진다.
- `UNVMED_SQ_F_FROZEN`: 커맨드 타임아웃 발생 시 SQ가 frozen 상태임을 표시.
- 락/상태 관련 인라인 헬퍼:
  - `unvmed_sq_enter()/unvmed_sq_try_enter()/unvmed_sq_exit()` — SQ 스핀락 획득/시도/해제
  - `unvmed_cq_enter()/unvmed_cq_exit()` — CQ 스핀락 획득/해제
  - `unvmed_sq_ready()` — enabled이고 FROZEN이 아니면 true
  - `unvmed_sq_entry(usq, db)` — 도어벨 위치의 SQ 엔트리 포인터 반환
  - `unvmed_sq_for_each_entry(usq, id, entry)` — pending 엔트리 순회 매크로 (thread-safe 아님, `unvmed_sq_enter()`로 잠근 상태에서 사용)
  - `unvmed_cq_enabled(u, qid)` — 해당 qid CQ가 live인지 확인
  - `unvmed_cq_irq_enabled(ucq)` — vector가 0 이상이면 인터럽트 지원 (-1은 인터럽트 미지원 의미)

## ctests에서의 사용 맥락

(추정) 테스트가 I/O 큐 페어를 만들고 커맨드를 발행할 때 핵심이 되는 핸들. Cython에서는 포인터를 보관하고 `unvmed_sq_id()` 같은 매크로를 인라인 함수로 재노출하는 형태가 될 것이다. 커맨드 발행 전후 `unvmed_sq_enter()`/`unvmed_sq_exit()` 락 규약을 파이썬 레벨 컨텍스트 매니저로 감싸는 패턴이 유용할 것으로 보인다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Submission Queue / Completion Queue 정의, 도어벨 레지스터
- 관련 컨셉: `unvmed-io-queues.md`, `unvmed-cmd-submit.md`, `unvmed-cq-reap.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
