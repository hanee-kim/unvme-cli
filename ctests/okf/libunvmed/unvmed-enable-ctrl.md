---
type: Cython Binding Target
title: unvmed_enable_ctrl
description: CC 레지스터를 구성하고 CC.EN=1을 설정한 뒤 CSTS.RDY=1을 대기하여 컨트롤러를 활성화하는 API.
resource: lib/libunvmed.h::unvmed_enable_ctrl
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/**
 * @iosqes: I/O Submission Queue Entry Size (specified as 2^n)
 * @iocqes: I/O Completion Queue Entry Size (specified as 2^n)
 * @mps: Memory Page Size (specified as (2 ^ (12 + n)))
 * @ams: Arbitration Mechanism Selected
 * @css: I/O Command Set Selected
 * @timeout: timeout in seconds (0: disabled)
 * Return: 0 on success, otherwise -1 with errno set.
 */
int unvmed_enable_ctrl(struct unvme *u, uint8_t iosqes, uint8_t iocqes,
		       uint8_t mps, uint8_t ams, uint8_t css, int timeout);
```

## 설명

- Controller Configuration(CC) 레지스터에 주어진 IOSQES/IOCQES/MPS/AMS/CSS 값을 쓰고 CC.EN을 1로 설정한 후, CSTS.RDY가 1이 될 때까지 대기하여 컨트롤러가 ready 상태가 되었음을 보장한다.
- 파라미터 인코딩은 CC 레지스터 필드 정의를 그대로 따른다: entry size는 2^n 바이트, MPS는 2^(12+n) 바이트.
- `timeout`은 초 단위이며 0이면 비활성화.
- 관련 헬퍼 `unvmed_pagesize(u)`는 CC에 구성된 MPS 기반 페이지 크기(바이트)를 반환한다.

## ctests에서의 사용 맥락

(추정) 컨트롤러 초기화 fixture의 핵심 단계. 일반적으로 iosqes=6(64B), iocqes=4(16B), mps=0(4KiB) 같은 표준 값을 기본 인자로 감싼 파이썬 헬퍼를 만들고, CC 필드 조합에 따른 동작을 검증하는 컨트롤러 레지스터 테스트에서는 인자를 직접 조작할 것이다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Controller Configuration(CC) 레지스터 (EN, CSS, MPS, AMS, IOSQES, IOCQES 필드), Controller Status(CSTS.RDY)
- 관련 컨셉: `unvmed-adminq.md`, `unvmed-reset.md`, `struct-unvme.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
