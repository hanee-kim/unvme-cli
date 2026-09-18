---
type: Cython Binding Target
title: unvmed_reset_ctrl 계열 (reset / subsystem reset / FLR / hot reset / link disable)
description: 컨트롤러 리셋과 PCIe 레벨 리셋(NSSR, FLR, Hot Reset, Link Disable)을 수행하는 API 모음.
resource: lib/libunvmed.h::unvmed_reset_ctrl
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
void unvmed_reset_ctrl(struct unvme *u);
void unvmed_reset_ctrl_graceful(struct unvme *u);

/* 하드웨어 레벨 리셋만 수행 (CC.EN=0 → CSTS.RDY=0 대기).
 * 큐 quiesce나 in-flight 커맨드 취소는 하지 않으며, 성공 후 caller가
 * 반드시 unvmed_reset_ctx()를 호출해야 함.
 * Return: 0 성공, -1 실패 + errno
 * (EALREADY: 이미 disabled, EBUSY: 리셋 진행 중,
 *  ENODEV: CSTS.RDY 폴링 중 PCIe link down 감지 (CSTS == 0xffffffff)) */
int unvme_reset_ctrl(struct unvme *u);

/* NVM Subsystem Reset */
int unvmed_subsystem_reset(struct unvme *u);
/* Function Level Reset */
int unvmed_flr(struct unvme *u);
/* Trigger Hot Reset to corresponding downstream port */
int unvmed_hot_reset(struct unvme *u);
/* Trigger Link Disable to corresponding downstream port */
int unvmed_link_disable(struct unvme *u);
```

## 설명

- `unvmed_reset_ctrl()`: CC.EN을 0으로 내리고 CSTS.RDY=0을 대기한다. 모든 SQ를 quiesce하여 상위 계층 제출을 막고, in-flight 커맨드는 "Host Aborted" 상태 코드의 인공 CQ 엔트리로 취소 처리한 뒤 상위 계층이 fetch하도록 재개한다. 마지막으로 컨트롤러에 등록된 네임스페이스 인스턴스를 모두 해제한다.
- `unvmed_reset_ctrl_graceful()`: 먼저 생성된 모든 I/O 큐 페어를 (SQ quiesce 상태에서) Delete 커맨드로 삭제한 후 CC.EN=0 → CSTS.RDY=0 순서로 진행하는 graceful 버전.
- `unvme_reset_ctrl()` (접두사가 `unvme_`임에 주의): 드라이버 컨텍스트 teardown 없이 하드웨어 레벨 리셋만 수행하고 결과를 반환하는 변형. 성공 후 호출자가 반드시 `unvmed_reset_ctx()`로 드라이버 컨텍스트(큐/커맨드/네임스페이스)를 일관된 상태로 되돌린 뒤 컨트롤러를 재활성화해야 한다. EALREADY/EBUSY/ENODEV로 실패 원인을 구분해 준다.
- `unvmed_subsystem_reset()` / `unvmed_flr()` / `unvmed_hot_reset()` / `unvmed_link_disable()`: 각각 NVM Subsystem Reset, PCIe Function Level Reset, 다운스트림 포트 Hot Reset, Link Disable을 트리거한다. 헤더에는 한 줄 요약만 있다.

> TODO: 각 리셋 API 이후 요구되는 후속 절차(레지스터 재구성 등)의 세부 동작은 upstream 구현(`lib/libunvmed.c`)으로 재확인 필요

## ctests에서의 사용 맥락

(추정) 리셋/복구 시나리오 테스트의 핵심 API. 통상 `unvmed_reset_ctrl()` 직후 `unvmed_reset_ctx()` 또는 `unvmed_disable_ctx()`(드라이버 컨텍스트 정리)와 `unvmed_ctx_init()`/`unvmed_ctx_restore()`(상태 스냅샷/복원)를 조합해 "리셋 후 재초기화" fixture를 구성할 것으로 보인다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Controller Reset, NVM Subsystem Reset(NSSR); PCI Express Base Specification의 Function Level Reset, Secondary Bus Reset(Hot Reset), Link Disable
- 관련 컨셉: `unvmed-ctx.md`, `unvmed-cancel.md`, `unvmed-enable-ctrl.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
