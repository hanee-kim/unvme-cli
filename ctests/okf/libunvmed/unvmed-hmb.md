---
type: Cython Binding Target
title: unvmed_hmb_* / struct unvme_hmb (Host Memory Buffer)
description: HMB 영역 할당·IOMMU 매핑·해제와 descriptor 관리 구조체 (Set Features 발행은 별도).
resource: lib/libunvmed.h::unvmed_hmb_init
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
struct unvme_hmb {
	struct {
		__le64 badd;	/* buffer address */
		__le32 bsize;	/* buffer size */
		uint32_t rsvd;
	} *descs;		/* HMB descriptor list */
	uint64_t *descs_vaddr;
	uint64_t descs_iova;
	size_t descs_size;
	int nr_descs;
	uint32_t hsize;
};

bool unvmed_hmb_allocated(struct unvme *u);
struct unvme_hmb *unvmed_hmb(struct unvme *u);

/**
 * @bsize: buffer size array pointer (CC.MPS 단위), @nr_bsize: 배열 엔트리 수
 * Set Features admin 커맨드는 발행하지 않고, IOMMU 매핑된 HMB만 초기화
 */
int unvmed_hmb_init(struct unvme *u, uint32_t *bsize, int nr_bsize);
int unvmed_hmb_free(struct unvme *u);
```

## 설명

- Host Memory Buffer(호스트 메모리를 컨트롤러가 사용하도록 제공하는 기능)의 호스트 측 자원 관리 API다.
- `unvmed_hmb_init()`은 CC.MPS 단위 크기 배열대로 버퍼들을 할당하고 IOMMU에 매핑하며 descriptor list를 구성하지만, **Set Features 커맨드는 발행하지 않는다**. 실제 컨트롤러에 HMB를 알리는 것은 `unvmed_set_features_hmb()`가 담당하며, 인자(hsize, descs_addr, nr_descs)는 `unvmed_hmb()`로 얻은 `struct unvme_hmb`의 값으로 구성한다 (헤더 주석 기준).
- `unvmed_hmb_allocated()`는 HMB가 이미 할당됐는지 확인, `unvmed_hmb_free()`는 해제.

## ctests에서의 사용 맥락

(추정) HMB feature 테스트에서 hmb_init → set_features_hmb(enable) → 동작 검증 → set_features_hmb(memory return) → hmb_free의 시퀀스로 사용될 것이다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Host Memory Buffer (Set Features FID 0Dh, HSIZE/HMDLLA/HMDLUA/HMDLEC 필드, Host Memory Descriptor 포맷)
- 관련 컨셉: `unvmed-admin-features.md`, `unvmed-dma.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
