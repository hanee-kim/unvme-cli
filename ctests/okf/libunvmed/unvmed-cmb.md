---
type: Cython Binding Target
title: unvmed_cmb_* / struct unvme_cmb (Controller Memory Buffer)
description: 컨트롤러 메모리 버퍼(CMB)의 초기화·해제·영역 조회 API와 구조체.
resource: lib/libunvmed.h::unvmed_cmb_init
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
struct unvme_cmb {
	int bar;	/* CMB가 위치한 PCI BAR */
	void *vaddr;
	uint64_t iova;
	size_t size;
};

int unvmed_cmb_init(struct unvme *u);
void unvmed_cmb_free(struct unvme *u);
struct unvme_cmb *unvmed_cmb(struct unvme *u);

/**
 * @vaddr: 매핑된 CMB 영역 출력
 * Return: CMB 영역 크기(양수), 실패 시 에러 (errno 설정)
 */
ssize_t unvmed_cmb_get_region(struct unvme *u, void **vaddr);
```

## 설명

- Controller Memory Buffer(컨트롤러가 제공하는 메모리 영역을 호스트가 큐/데이터 배치에 사용하는 기능)의 접근 API다.
- `unvmed_cmb_init()`으로 초기화하고 `unvmed_cmb_get_region()`으로 매핑된 가상주소와 크기를 얻는다. `unvmed_cmb()`는 bar/vaddr/iova/size를 담은 구조체를 반환한다.
- 헤더에는 각 함수의 한 줄 요약만 있으며 CMBLOC/CMBSZ 레지스터 처리 등 세부 동작은 명시돼 있지 않다.

> TODO: CMB 활성화 절차(CMBMSC 등 레지스터 조작 여부)는 upstream 구현(`lib/libunvmed.c`)으로 재확인 필요

## ctests에서의 사용 맥락

(추정) CMB 상에 I/O 큐를 배치하는 테스트에서 `unvmed_cmb_get_region()` → IOVA 계산 → `unvmed_init_sq_iova()`/`unvmed_init_cq_iova()` 순으로 사용될 가능성이 높다 (iova 변형 API가 "pre-mapped 버퍼"를 요구하는 것과 부합).

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Controller Memory Buffer (CMBLOC, CMBSZ, CMBMSC, CMBSTS 레지스터)
- 관련 컨셉: `unvmed-io-queues.md` (`unvmed_init_sq_iova`/`unvmed_init_cq_iova`), `unvmed-dma.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
