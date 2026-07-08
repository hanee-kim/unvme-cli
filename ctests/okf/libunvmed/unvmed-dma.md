---
type: Cython Binding Target
title: unvmed_map_vaddr / unvmed_mem_alloc 계열 (DMA/IOMMU 메모리 관리)
description: 가상주소↔IOVA 변환, IOMMU 매핑/해제, DMA 버퍼 할당과 MPS 정렬 버퍼 헬퍼 API.
resource: lib/libunvmed.h::unvmed_map_vaddr
tags: [libunvmed, upstream, memory, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

## 시그니처

```c
/* 주소 변환. 모두 thread-safe */
int unvmed_to_iova(struct unvme *u, void *buf, uint64_t *iova);
ssize_t unvmed_to_vaddr(struct unvme *u, uint64_t iova, void **vaddr);

/* IOMMU 매핑/해제. @flags는 libvfn의 enum iommu_map_flags */
int unvmed_map_vaddr(struct unvme *u, void *buf, size_t len, uint64_t *iova,
		     unsigned long flags);
int unvmed_unmap_vaddr(struct unvme *u, void *buf);

/* 물리적으로 연속된 DMA 버퍼 할당 + IOMMU 매핑. thread-safe */
int __unvmed_mem_alloc(struct unvme *u, size_t size,
		       struct iommu_dmabuf *buf, size_t pagesize);
int unvmed_mem_alloc(struct unvme *u, size_t size, struct iommu_dmabuf *buf,
		     size_t pagesize);
struct iommu_dmabuf *unvmed_mem_get(struct unvme *u, uint64_t iova);
int unvmed_mem_free(struct unvme *u, uint64_t iova);

/* MPS 기반 페이지 크기 및 정렬 버퍼 헬퍼 (IOMMU 매핑은 하지 않음) */
size_t unvmed_pagesize(struct unvme *u);
static inline ssize_t unvmed_pgmap_aligned(struct unvme *u, void **mem,
					   size_t sz, size_t pagesize);
static inline ssize_t unvmed_pgmap(struct unvme *u, void **mem, size_t sz);
static inline void unvmed_pgunmap(void *mem);
```

## 설명

- **주소 변환**: `unvmed_to_iova()`/`unvmed_to_vaddr()`는 IOMMU 변환 테이블 기준으로 가상주소↔IOVA를 변환한다. `unvmed_to_vaddr()`는 접근 가능한 크기를 반환한다.
- **매핑**: `unvmed_map_vaddr()`/`unvmed_unmap_vaddr()`는 임의의 사용자 버퍼를 IOMMU 테이블에 매핑/해제한다.
- **DMA 버퍼**: `unvmed_mem_alloc()`은 물리적으로 연속인 DMA 버퍼를 할당해 IOMMU에 매핑하고 `struct iommu_dmabuf`(libvfn)로 돌려준다. `unvmed_mem_get()`은 IOVA가 속한 버퍼를 검색, `unvmed_mem_free()`는 IOVA로 찾아 해제한다.
- **정렬 버퍼 헬퍼**: `unvmed_pgmap[_aligned]()`는 `posix_memalign()` 기반으로 페이지 정렬 버퍼를 할당하고 0으로 채운다 (page-fault 유도 목적, 헤더 주석 기준). `unvmed_pagesize()`는 CC.MPS에 구성된 페이지 크기를 반환한다. 이 헬퍼들은 IOMMU 매핑을 수행하지 않는다.

> TODO: `__unvmed_mem_alloc()`과 `unvmed_mem_alloc()`의 차이는 헤더 주석이 동일해 구분이 불명확 — upstream 구현으로 재확인 필요

## ctests에서의 사용 맥락

(추정) 파이썬에서 만든 버퍼(numpy 배열, bytearray 등)를 DMA에 쓰려면 `unvmed_map_vaddr()`로 명시적 매핑이 필요하다. PRP list/SGL segment처럼 호출자가 직접 매핑해야 하는 보조 페이지(`__unvmed_mapv_prp_list`, `__unvmed_mapv_sgl_seg`의 전제조건) 준비에도 사용된다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Memory Page Size(CC.MPS), PRP/SGL의 메모리 페이지 정렬 요구사항
- 관련 컨셉: `unvmed-prp-sgl.md`, `unvmed-cmd-alloc.md`, `unvmed-hmb.md`, `unvmed-cmb.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
