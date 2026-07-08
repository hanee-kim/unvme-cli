---
type: Cython Binding Target
title: unvmed_ctx_* / quiesce / 드라이버 컨텍스트 관리
description: 드라이버 컨텍스트(admin/IO 큐, 네임스페이스) 스냅샷·복원·정리와 SQ quiesce API.
resource: lib/libunvmed.h::unvmed_ctx_init
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/* 현재 드라이버 컨텍스트(컨트롤러 레지스터 + admin 큐, ns, I/O 큐) 스냅샷 */
int unvmed_ctx_init(struct unvme *u);
/* 스냅샷 상태로 복원 (admin/IO 큐, ns 인스턴스 revive) */
int unvmed_ctx_restore(struct unvme *u);
/* 스냅샷 데이터 해제 (하드웨어 상태에는 영향 없음) */
void unvmed_ctx_free(struct unvme *u);

/* 리셋 직후 usq/ucq/ns 컨텍스트를 disabled 상태로 전환 */
void unvmed_disable_ctx(struct unvme *u);
/* 리셋 직후 모든 컨텍스트 초기화(clear) 및 in-flight 커맨드 취소 */
void unvmed_reset_ctx(struct unvme *u);
/* 컨트롤러의 모든 컨텍스트 리소스 일괄 해제 */
void unvmed_free_ctx(struct unvme *u);

void unvmed_quiesce_sq_all(struct unvme *u);
void unvmed_unquiesce_sq_all(struct unvme *u);
int unvmed_quiesce_sq(struct unvme *u, uint16_t qid);
int unvmed_unquiesce_sq(struct unvme *u, uint16_t qid);
```

## 설명

- **스냅샷/복원**: `unvmed_ctx_init()`은 컨트롤러 레지스터 상태(admin 큐 포함), 네임스페이스 인스턴스, I/O 큐를 스냅샷으로 저장하고, `unvmed_ctx_restore()`가 리셋 후 그 상태로 재초기화한다. `unvmed_ctx_free()`는 스냅샷 자료구조만 해제한다.
- **리셋 후 정리**: 헤더 주석에 따르면 `unvmed_disable_ctx()`와 `unvmed_reset_ctx()`는 리셋 API 직후 호출해야 하며, 각각 애플리케이션의 추가 SQE push/CQE reap을 막기 위한 disable 처리, 컨텍스트 인스턴스 clear 및 in-flight 커맨드 취소를 수행한다. `unvmed_free_ctx()`는 리소스를 일괄 해제한다 (자체적으로 리소스를 추적하지 않는 애플리케이션용).
- **Quiesce**: `unvmed_quiesce_sq[_all]()`은 SQ를 잠가(lock) 스레드들의 추가 제출을 차단하고, `unvmed_unquiesce_sq[_all]()`이 해제한다.

## ctests에서의 사용 맥락

(추정) 리셋 계열 테스트에서 "리셋 전 상태 저장 → 리셋 → 복원 → I/O 재검증" 시나리오를 만들 때 사용. pytest fixture의 teardown에서 `unvmed_free_ctx()`로 잔여 리소스를 일괄 정리하는 용도로도 쓸 수 있다.

## 관련

- 관련 NVMe 스펙 조항: 직접 대응 조항 없음 (호스트 드라이버 상태 관리; 복원 과정은 Create I/O SQ/CQ, Identify 등 표준 커맨드를 재실행하는 것으로 추정)
- 관련 컨셉: `unvmed-reset.md`, `unvmed-io-queues.md`, `unvmed-ns-driver.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
