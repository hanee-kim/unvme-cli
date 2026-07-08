---
type: Cython Binding Target
title: struct unvme
description: libunvmed가 관리하는 NVMe 컨트롤러 인스턴스를 나타내는 불투명(opaque) 핸들과 컨트롤러 상태 enum.
resource: lib/libunvmed.h::struct unvme
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

## 시그니처

```c
/* 공개 헤더에는 전방 선언만 존재. 실제 정의는 lib/libunvmed-private.h (비공개) */
struct unvme;

enum unvme_state {
	UNVME_DISABLED	= 0,	/* 컨트롤러 비활성화 상태 (초기 상태) */
	UNVME_ENABLING,		/* 활성화 진행 중, 이후 UNVME_ENABLED */
	UNVME_ENABLED,		/* 컨트롤러 활성화(alive) 상태 */
	UNVME_RESETTING,	/* 리셋 진행 중, 이후 UNVME_DISABLED */
	UNVME_TEARDOWN,		/* 인스턴스 해제 직전의 최종 상태 */
};

static inline const char *unvmed_state_str(enum unvme_state state);

/* `struct unvme`는 `struct nvme_ctrl`(libvfn)로 시작하므로 캐스팅 가능 */
#define __unvmed_ctrl(u)	((struct nvme_ctrl *)(u))
#define unvmed_bdf(u)		(__unvmed_ctrl(u)->pci.bdf)
#define unvmed_reg(u)		(__unvmed_ctrl(u)->regs)
```

## 설명

- `struct unvme`는 libunvmed의 거의 모든 API가 첫 인자로 받는 컨트롤러 인스턴스 핸들이다. 공개 헤더(`lib/libunvmed.h`)에서는 전방 선언만 제공되며 내부 필드는 `lib/libunvmed-private.h`에 숨겨져 있다.
- 헤더 주석에 따르면 `struct unvme`는 libvfn의 `struct nvme_ctrl`로 시작하도록 배치되어 있어 `__unvmed_ctrl()` 매크로로 쉽게 변환할 수 있고, `unvmed_bdf()`/`unvmed_reg()`로 PCI BDF 문자열과 MMIO 레지스터 베이스에 접근한다.
- `enum unvme_state`는 컨트롤러 인스턴스의 생명주기 상태(DISABLED → ENABLING → ENABLED → RESETTING → TEARDOWN)를 나타내며, `unvmed_state_str()`이 문자열 표현을 반환한다.
- MMIO 접근 헬퍼 매크로 `unvmed_read32/read64/write32/write64(u, offset[, value])`도 이 핸들 기반으로 동작한다 (little-endian 변환 포함).

## ctests에서의 사용 맥락

(추정) Cython 바인딩에서는 `struct unvme *`를 불투명 포인터(`cdef struct unvme` 전방 선언)로 유지하면서 파이썬 측 Controller 객체가 이 포인터를 소유하는 형태가 자연스럽다. 내부 필드에 직접 접근하지 않고 `unvmed_*` 접근자 함수/매크로를 통해서만 조작해야 한다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Controller Configuration(CC), Controller Status(CSTS) 레지스터 (상태 전이가 CC.EN/CSTS.RDY와 연동됨 — `unvmed_enable_ctrl`, `unvmed_reset_ctrl` 참고)
- 관련 컨셉: `unvmed-ctrl-lifecycle.md`, `unvmed-enable-ctrl.md`, `unvmed-reset.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
