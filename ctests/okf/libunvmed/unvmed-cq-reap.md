---
type: Cython Binding Target
title: unvmed_cq_run 계열 (CQ reaping)
description: 완료 큐에서 CQE를 reap하는 API 모음 (전량/개수 지정/nowait).
resource: lib/libunvmed.h::unvmed_cq_run
tags: [libunvmed, upstream, io]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/**
 * @nowait: true면 CQ가 비었을 때 @nr_cqes를 못 채워도 즉시 반환
 * Return: Number of cq entries fetched.
 */
int __unvmed_cq_run_n(struct unvme *u, struct unvme_sq *usq, struct unvme_cq *ucq,
		      struct unvme_vcq *vcq, struct nvme_cqe *cqes, int nr_cqes,
		      bool nowait);

/* @ucq가 빌 때까지 가능한 모든 CQE reap */
int unvmed_cq_run(struct unvme *u, struct unvme_sq *usq, struct unvme_cq *ucq,
		  struct nvme_cqe *cqes);

/* @min(필수) ~ @max(best effort)개 reap */
int unvmed_cq_run_n(struct unvme *u, struct unvme_sq *usq, struct unvme_cq *ucq,
		    struct unvme_vcq *vcq, struct nvme_cqe *cqes, int min, int max);
```

## 설명

- `unvmed_cq_run()`은 CQ가 빌 때까지 모든 엔트리를, `unvmed_cq_run_n()`은 최소 `min`개(필수)에서 최대 `max`개(best effort)까지 reap한다. `__unvmed_cq_run_n()`은 `nowait` 여부를 직접 제어하는 저수준 버전이다.
- 결과 CQE들은 호출자가 준비한 `cqes` 배열로 복사되며 반환값은 실제 fetch된 개수다.
- `vcq` 인자를 받는 변형은 가상 CQ 분배 경로와 연동된다 (`struct-unvme-vcq.md` 참고).

> TODO: `usq`/`vcq` 인자의 정확한 역할(어떤 경우 NULL 허용인지 등)은 헤더 주석에 명시가 없어 upstream 구현(`lib/libunvmed.c`)으로 재확인 필요

## ctests에서의 사용 맥락

(추정) 비동기 제출 테스트에서 명시적으로 CQE를 회수·검증할 때 사용. 예를 들어 NODB로 N개 post → doorbell 일괄 갱신 → `unvmed_cq_run_n(min=N)`으로 N개 완료 회수 → 각 CQE의 status/cid 검증하는 시나리오. 동기 테스트에서는 `unvmed_cmd_issue_and_wait()` 내부에서 처리되므로 직접 쓰지 않을 수 있다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 완료 처리 모델 (Phase Tag, Completion Queue Head Doorbell)
- 관련 컨셉: `struct-unvme-vcq.md`, `unvmed-cmd-submit.md`, `unvmed-cmd-status.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
