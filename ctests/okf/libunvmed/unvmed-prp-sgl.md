---
type: Cython Binding Target
title: unvmed_mapv_prp / unvmed_mapv_sgl 계열 (DPTR 데이터 구조 매핑)
description: iovec 목록을 PRP 또는 SGL 데이터 구조로 만들어 SQE의 DPTR에 매핑하는 API.
resource: lib/libunvmed.h::unvmed_mapv_prp
tags: [libunvmed, upstream, io]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/* PRP */
int __unvmed_mapv_prp(struct unvme_cmd *cmd, union nvme_cmd *sqe,
		      struct iovec *iov, int nr_iov);
/* @prplist(PRP2에 쓰일 PRP list 페이지)는 caller가 IOMMU에 미리 매핑해야 함 */
int __unvmed_mapv_prp_list(struct unvme_cmd *cmd, union nvme_cmd *sqe,
			   void *prplist, struct iovec *iov, int nr_iov);
/* cmd->buf.iov (인라인 단일 iovec)를 사용:
 * __unvmed_mapv_prp(cmd, sqe, &cmd->buf.iov, 1)과 동일 */
int unvmed_mapv_prp(struct unvme_cmd *cmd, union nvme_cmd *sqe);

/* SGL */
int __unvmed_mapv_sgl(struct unvme_cmd *cmd, union nvme_cmd *sqe,
		      struct iovec *iov, int nr_iov);
/* @seg(SGL segment 페이지)는 caller가 IOMMU에 미리 매핑해야 함 */
int __unvmed_mapv_sgl_seg(struct unvme_cmd *cmd, union nvme_cmd *sqe,
			  struct nvme_sgld *seg, struct iovec *iov, int nr_iov);
/* cmd->buf.iov 사용: __unvmed_mapv_sgl(cmd, sqe, &cmd->buf.iov, 1)과 동일 */
int unvmed_mapv_sgl(struct unvme_cmd *cmd, union nvme_cmd *sqe);
```

## 설명

- iovec 배열을 PRP 또는 SGL 데이터 구조로 구성해 SQE의 데이터 포인터(DPTR)에 매핑한다. 실제 데이터 구조 준비는 libvfn이 수행한다 (헤더 주석 기준).
- `__unvmed_mapv_prp_list()`와 `__unvmed_mapv_sgl_seg()`는 PRP list 페이지/SGL segment 페이지를 호출자가 직접 제공하는 변형으로, **해당 페이지를 미리 IOMMU 페이지 테이블에 매핑해 두어야 한다**.
- 밑줄 없는 `unvmed_mapv_prp()`/`unvmed_mapv_sgl()`은 커맨드에 내장된 단일 iovec(`cmd->buf.iov`)을 사용하는 단축형이다.
- SGL 사용 여부는 제출 플래그 `UNVMED_CMD_F_SGL`과도 연관된다 (`struct-unvme-cmd.md` 참고).
- 모든 함수는 0 성공 / -1 실패 (errno 설정).

## ctests에서의 사용 맥락

(추정) PRP/SGL 경계 조건 테스트(페이지 비정렬 오프셋, 다중 iovec, PRP list가 필요한 크기, SGL segment/last segment descriptor 구성 등)에서 직접 사용될 가능성이 높다. 일반 read/write 테스트는 `unvmed_cmd_prep_read/write()`가 내부에서 매핑을 처리하므로 직접 호출이 필요 없을 것이다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Physical Region Page(PRP) Entry/List, Scatter Gather List(SGL) descriptor
- 관련 컨셉: `unvmed-cmd-submit.md`, `unvmed-dma.md`, `struct-unvme-cmd.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
