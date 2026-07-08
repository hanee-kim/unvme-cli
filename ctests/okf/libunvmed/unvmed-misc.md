---
type: Cython Binding Target
title: unvmed_nr_cmds / unvmed_nr_irqs / unvmed_to_json / MMIO 헬퍼 (기타 조회 API)
description: in-flight 커맨드 수, IRQ 수, 컨트롤러 상태 JSON 덤프, PCI MMIO 접근 매크로.
resource: lib/libunvmed.h::unvmed_to_json
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

## 시그니처

```c
/* in-flight(발행됐지만 미완료) 커맨드 수 */
int unvmed_nr_cmds(struct unvme *u);

/* vfio-pci가 보고한 최대 IRQ 수 */
int unvmed_nr_irqs(struct unvme *u);

/**
 * 큐, 네임스페이스, HMB, CMB, shared memory 등 컨트롤러 상태를
 * json_object로 반환. caller가 json_object_put() 책임.
 * Return: json_object pointer, NULL on failure with errno set.
 */
struct json_object *unvmed_to_json(struct unvme *u);

/* PCI MMIO 접근 헬퍼 (little-endian 변환 포함) */
#define unvmed_read32(u, offset)
#define unvmed_read64(u, offset)
#define unvmed_write32(u, offset, value)
#define unvmed_write64(u, offset, value)
```

## 설명

- `unvmed_nr_cmds()`: SQ에 발행됐지만 아직 CQ로 완료되지 않은 커맨드 수를 반환한다.
- `unvmed_nr_irqs()`: vfio-pci가 보고한 해당 컨트롤러의 최대 인터럽트 벡터 수를 반환한다.
- `unvmed_to_json()`: 컨트롤러의 전체 상태(큐, 네임스페이스, HMB, CMB 등)를 json-c의 `json_object`로 덤프한다.
- MMIO 매크로는 `unvmed_reg(u)` 베이스에 오프셋을 더해 32/64비트 레지스터를 읽고 쓴다. NVMe 레지스터(CAP, CC, CSTS, 도어벨 등)에 대한 raw 접근 수단이다.

## ctests에서의 사용 맥락

(추정) `unvmed_to_json()`은 테스트 실패 시 컨트롤러 상태 스냅샷을 로그로 남기는 디버깅 헬퍼로 유용하다 (JSON 문자열화 후 파이썬 dict로 파싱 가능). MMIO 매크로는 레지스터 직접 검증 테스트(CAP 필드 확인, CSTS 폴링, 비정상 도어벨 쓰기 등)를 위해 Cython 인라인 함수로 노출할 가치가 있다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Controller Registers (레지스터 오프셋 맵)
- 관련 컨셉: `struct-unvme.md`, `unvmed-io-queues.md`, `unvmed-hmb.md`, `unvmed-cmb.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
