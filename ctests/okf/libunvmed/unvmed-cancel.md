---
type: Cython Binding Target
title: unvmed_cancel_cmd / unvmed_cancel_allocated_state_cmds (커맨드 취소)
description: 리셋/에러 경로에서 미완료 커맨드를 인공 CQE로 취소 처리하는 API.
resource: lib/libunvmed.h::unvmed_cancel_cmd
tags: [libunvmed, upstream, io, error]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

## 시그니처

```c
/**
 * @usq->ucq를 drain하고, SQ를 cancel한 뒤, 제출 스레드들이 자신의 vcq에서
 * 모든 커맨드를 fetch할 때까지 대기
 */
void unvmed_cancel_cmd(struct unvme *u, struct unvme_sq *usq);

/**
 * ALLOCATED 상태(할당됐지만 아직 제출 안 됨)인 모든 커맨드 취소.
 * 각 커맨드에 대해 "Command Aborted By Host"(SCT=PATH(0x3), SC=0x71)
 * fake CQE를 생성해 일반 완료 경로로 전달.
 *
 * 반드시 unvmed_quiesce_sq_all()로 모든 SQ를 quiesce(lock)한 상태에서
 * 호출해야 함 (동시 SQE 발행 방지)
 */
void unvmed_cancel_allocated_state_cmds(struct unvme *u);
```

## 설명

- `unvmed_cancel_cmd()`는 특정 SQ에 대해 연결된 CQ를 drain하고 SQ를 취소 처리한 뒤, 각 제출 스레드가 자기 vcq에서 (취소된) 완료를 모두 가져갈 때까지 기다린다.
- `unvmed_cancel_allocated_state_cmds()`는 `UNVME_CMD_S_ALLOCATED` 상태의 커맨드들에 대해 "Command Aborted By Host" 상태의 가짜 CQE를 만들어 정상 완료 경로로 흘려보낸다. 호출 전 `unvmed_quiesce_sq_all()`이 선행되어야 한다는 제약이 헤더에 명시돼 있다.
- 이 메커니즘은 `unvmed_reset_ctrl()`의 in-flight 커맨드 취소 동작과 같은 계열이다.

## ctests에서의 사용 맥락

(추정) 테스트 코드가 직접 호출하기보다는 리셋/타임아웃 시나리오에서 라이브러리가 내부적으로 사용하는 것을 관찰하는 대상일 가능성이 높다. 다만 "리셋 중 대기 커맨드가 Host Aborted로 완료되는가"를 검증하는 테스트에서는 취소된 CQE의 status(SCT=0x3, SC=0x71)를 assertion하게 된다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Path Related Status — Command Aborted By Host (SCT 3h, SC 71h)
- 관련 컨셉: `unvmed-reset.md`, `unvmed-ctx.md`, `unvmed-cmd-status.md`, `struct-unvme-vcq.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
