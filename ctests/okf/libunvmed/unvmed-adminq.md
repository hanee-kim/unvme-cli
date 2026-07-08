---
type: Cython Binding Target
title: unvmed_create_adminq / unvmed_configure_adminq
description: Admin SQ/CQ를 생성하고 컨트롤러 레지스터(AQA/ASQ/ACQ)에 구성하는 API.
resource: lib/libunvmed.h::unvmed_create_adminq
tags: [libunvmed, upstream, admin, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

## 시그니처

```c
/**
 * @usq, @ucq: NULL 가능. NULL이면 no-op, 아니면 컨트롤러 레지스터 구성
 */
int unvmed_configure_adminq(struct unvme *u, struct unvme_sq *usq,
			    struct unvme_cq *ucq);

/**
 * @sq_size / @cq_size: number of queue entries
 * @irq: whether to initialize IRQ
 * Return: 0 on success, otherwise -1 with errno set.
 */
int unvmed_create_adminq(struct unvme *u, uint32_t sq_size,
			 uint32_t cq_size, bool irq);
```

## 설명

- `unvmed_create_adminq()`는 libvfn에 admin SQ/CQ 인스턴스를 생성하고, 그에 맞게 컨트롤러 attribute 레지스터의 admin queue 레지스터를 구성한다. 헤더 주석이 명시하듯 **컨트롤러를 enable하지는 않으며**, 이후 `unvmed_enable_ctrl()`을 명시적으로 호출해야 한다.
- `unvmed_configure_adminq()`는 이미 만들어진 `usq`/`ucq` 인스턴스를 컨트롤러 레지스터에 반영한다. 각각 NULL이면 해당 큐에 대해서는 아무 동작도 하지 않는다.

## ctests에서의 사용 맥락

(추정) 컨트롤러 초기화 fixture의 표준 순서 — `unvmed_init_ctrl()` → `unvmed_create_adminq()` → `unvmed_enable_ctrl()` → (identify, I/O 큐 생성) — 의 두 번째 단계로 사용될 것이다. admin 큐 레지스터를 비정상 값으로 구성하는 negative 테스트에는 `unvmed_configure_adminq()`가 쓰일 수 있다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Admin Queue Attributes(AQA), Admin Submission Queue Base Address(ASQ), Admin Completion Queue Base Address(ACQ) 레지스터
- 관련 컨셉: `unvmed-enable-ctrl.md`, `unvmed-io-queues.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
