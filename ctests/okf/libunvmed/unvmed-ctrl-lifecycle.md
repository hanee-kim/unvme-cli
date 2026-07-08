---
type: Cython Binding Target
title: unvmed_init_ctrl / unvmed_get / unvmed_free_ctrl
description: PCI NVMe 컨트롤러 인스턴스(struct unvme)의 생성·조회·해제 API.
resource: lib/libunvmed.h::unvmed_init_ctrl
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/**
 * @bdf: bus-device-function address of PCI NVMe controller
 * @max_nr_ioqs: maximum number of I/O queue identifier (QID) to be supported
 * Return: &struct unvme, otherwise NULL on error with errno set.
 */
struct unvme *unvmed_init_ctrl(const char *bdf, uint32_t max_nr_ioqs);

/* Free all the resources related to the given @u. */
void unvmed_free_ctrl(struct unvme *u);

/* Free all the resources of all the controller instances
 * in the current process context. */
void unvmed_free_ctrl_all(void);

/* @bdf에 해당하는 인스턴스를 찾고 refcnt를 증가시킴.
 * 사용 후 반드시 unvmed_put() 호출 필요.
 * Return: &struct unvme, otherwise NULL. */
struct unvme *unvmed_get(const char *bdf);

/* @u->refcnt 감소. 0에 도달하면 @u가 해제되며 이후 접근 금지.
 * Return: 감소된 refcnt 값 */
int unvmed_put(struct unvme *u);

/* Return: true on controller is enabled, otherwise false. */
bool unvmed_ctrl_enabled(struct unvme *u);

/* nvme_ctrl_init() 성공 시마다 증가하는 epoch 카운터 반환.
 * open 시점에 스냅샷해 두고 close/cleanup 시점과 비교하면
 * stale ctrl 참조에 대한 동작을 건너뛸 수 있음 */
uint32_t unvmed_get_epoch(struct unvme *u);
```

## 설명

- `unvmed_init_ctrl()`은 주어진 BDF의 PCI 디바이스를 NVMe 컨트롤러 인스턴스(`struct unvme`)로 초기화한다. `max_nr_ioqs`는 지원할 최대 I/O QID의 상한이다.
- `unvmed_get()`은 이미 초기화된 인스턴스를 BDF로 조회하며 **refcnt를 증가**시킨다. 사용이 끝나면 `unvmed_put()`으로 반납해야 하고, refcnt가 0에 도달하면 인스턴스가 해제된다.
- `unvmed_free_ctrl()`은 해당 인스턴스의 모든 리소스를 해제하고, `unvmed_free_ctrl_all()`은 프로세스 내 모든 컨트롤러 인스턴스를 해제한다.
- `unvmed_ctrl_enabled()`는 컨트롤러 enabled 여부를 반환한다.
- `unvmed_get_epoch()`은 컨트롤러 재초기화 감지용 epoch 카운터를 반환한다. 헤더 주석 기준으로 `nvme_ctrl_init()` 성공 시마다 증가하며, 스레드가 open 시점 값과 cleanup 시점 값을 비교해 stale 참조 여부를 판단하는 용도다.

## ctests에서의 사용 맥락

(추정) 테스트 세션/모듈 단위 fixture에서 `unvmed_init_ctrl()`로 디바이스를 attach하고, teardown에서 `unvmed_free_ctrl()`을 호출하는 컨트롤러 생명주기 관리의 뼈대가 될 API. `unvmed_get()`은 여러 fixture가 같은 컨트롤러 핸들을 공유할 때 유용하며, refcnt 규약상 `unvmed_get()`/`unvmed_put()` 쌍을 파이썬 객체 수명(`__dealloc__`)이나 컨텍스트 매니저에 연결하는 바인딩이 자연스럽다. `unvmed_get_epoch()`은 리셋/재초기화가 끼어드는 테스트에서 핸들 유효성 검사에 쓸 수 있다.

## 관련

- 관련 NVMe 스펙 조항: 해당 없음 (디바이스 attach는 vfio/IOMMU 영역 — libvfn 기반)
- 관련 컨셉: `struct-unvme.md`, `unvmed-enable-ctrl.md`, `unvmed-adminq.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
