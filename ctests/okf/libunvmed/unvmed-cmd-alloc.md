---
type: Cython Binding Target
title: unvmed_alloc_cmd 계열 (커맨드 인스턴스 할당·참조 관리)
description: 데이터/메타데이터 버퍼 자동 매핑을 포함한 NVMe 커맨드 인스턴스 할당과 refcnt 기반 해제 API.
resource: lib/libunvmed.h::unvmed_alloc_cmd
tags: [libunvmed, upstream, io]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/**
 * @cid: command identifier (NULL이면 자동 할당)
 * Return: &struct unvme_cmd, NULL on error with errno set
 *         (EBUSY: full, EEXIST: @cid already exists)
 */
struct unvme_cmd *unvmed_alloc_cmd(struct unvme *u, struct unvme_sq *usq,
				   uint16_t *cid, void *buf, size_t len);

struct unvme_cmd *unvmed_alloc_cmd_nodata(struct unvme *u,
					  struct unvme_sq *usq, uint16_t *cid);

struct unvme_cmd *unvmed_alloc_cmd_meta(struct unvme *u, struct unvme_sq *usq,
					uint16_t *cid, void *buf, size_t len,
					void *mbuf, size_t mlen);

/* refcnt 기반 획득/반납. 모두 thread-safe */
struct unvme_cmd *unvmed_cmd_get(struct unvme_sq *usq, uint16_t cid);
int unvmed_cmd_put(struct unvme_cmd *cmd);
```

## 설명

- `unvmed_alloc_cmd()`의 버퍼 처리 규칙 (헤더 주석 기준):
  - `buf != NULL && len > 0`: 해당 버퍼가 IOMMU에 매핑돼 있는지 확인하고, 아니면 자동 매핑 (해제는 `unvmed_cmd_free()`에서).
  - `buf == NULL && len > 0`: 지정 크기의 버퍼를 새로 할당하고 IOMMU에 매핑 (버퍼·매핑 모두 `unvmed_cmd_free()`에서 해제).
  - `buf == NULL && len == 0`: 전송할 데이터 버퍼를 준비하지 않음.
- `unvmed_alloc_cmd_nodata()`는 데이터 버퍼 없는 커맨드용, `unvmed_alloc_cmd_meta()`는 별도 메타데이터 버퍼(`mbuf`/`mlen`)까지 준비하는 버전이다. extended LBA(메타데이터가 데이터에 인라인) 포맷이면 mbuf/mlen이 필요 없다.
- `unvmed_cmd_put()`은 refcnt를 감소시키고 0이 되면 커맨드와 연관 버퍼를 모두 해제한다. 최초 할당 시 refcnt=1이므로 추가 참조가 없다면 첫 put에서 해제된다.
- 모든 API는 thread-safe.

## ctests에서의 사용 맥락

(추정) Cython 바인딩의 Command 객체 생성자에 대응. 파이썬 bytes/버퍼 객체를 `buf`로 넘기거나 len만 줘서 라이브러리가 DMA 버퍼를 할당하게 하는 두 가지 패턴 모두 가능하다. 파이썬 객체의 `__dealloc__`에서 `unvmed_cmd_put()`을 호출해 수명을 연결하는 구조가 자연스럽다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Command Identifier(CID) — SQ 내 유일성 요구
- 관련 컨셉: `struct-unvme-cmd.md`, `unvmed-cmd-submit.md`, `unvmed-dma.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
