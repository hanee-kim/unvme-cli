---
type: Cython Binding Target
title: unvmed_set_features / unvmed_get_features (Features admin 커맨드)
description: Set/Get Features 커맨드 발행 및 prep API (HMB 전용 Set Features 포함).
resource: lib/libunvmed.h::unvmed_set_features
tags: [libunvmed, upstream, admin]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

## 시그니처

```c
/* Set Features
 * @fid: feature identifier, @save: 속성 영속화(SV), @cdw11/@cdw12: 원시 dword */
int unvmed_cmd_prep_set_features(struct unvme_cmd *cmd, uint32_t nsid,
				 uint8_t fid, bool save, uint32_t cdw11,
				 uint32_t cdw12, struct iovec *iov, int nr_iov);
int unvmed_set_features(struct unvme_cmd *cmd, uint32_t nsid, uint8_t fid,
			bool save, uint32_t cdw11, uint32_t cdw12,
			struct iovec *iov, int nr_iov);

/* Set Features - Host Memory Buffer (FID 0Dh)
 * @hsize: HMB 크기 (CC.MPS 단위), @descs_addr: descriptor list 주소
 * @nr_descs: descriptor 수, @mr: Memory Return, @enable: EHM (CDW11) */
int unvmed_cmd_prep_set_features_hmb(struct unvme_cmd *cmd, uint32_t hsize,
				     uint64_t descs_addr, uint32_t nr_descs,
				     bool mr, bool enable);
int unvmed_set_features_hmb(struct unvme_cmd *cmd, uint32_t hsize,
			    uint64_t descs_addr, uint32_t nr_descs,
			    bool mr, bool enable);

/* Get Features
 * @sel: 반환 데이터의 속성 선택 (current/default/saved 등) */
int unvmed_cmd_prep_get_features(struct unvme_cmd *cmd, uint32_t nsid,
				 uint8_t fid, uint8_t sel, uint32_t cdw11,
				 uint32_t cdw14, struct iovec *iov, int nr_iov);
int unvmed_get_features(struct unvme_cmd *cmd, uint32_t nsid, uint8_t fid,
			uint8_t sel, uint32_t cdw11, uint32_t cdw14,
			struct iovec *iov, int nr_iov);
```

## 설명

- Set/Get Features 커맨드를 FID와 원시 CDW 값(cdw11/cdw12/cdw14)으로 구성하는 얇은 래퍼로, 특정 feature에 대한 해석은 하지 않는다 (HMB 제외).
- prep 버전은 SQE 구성만 하고(0/-1 반환), 발행 버전은 완료까지 수행한다 (0 성공, -1 에러, 그 외 CQE status field).
- HMB 전용 변형은 `unvmed_hmb_init()`으로 HMB 영역을 초기화한 뒤 그 결과값(descriptor list 주소·개수 등)으로 커맨드를 구성해야 한다고 헤더 주석에 명시돼 있다.
- 모두 thread-safe.

## ctests에서의 사용 맥락

(추정) Number of Queues(FID 07h), Arbitration, Power Management 등 각종 feature 테스트에서 FID·CDW를 직접 지정해 사용. Get Features의 결과값은 CQE DW0로 반환되는 경우가 많아 완료 CQE 접근(`cmd->cqe`)과 함께 바인딩되어야 한다.

> TODO: CQE DW0 반환값을 어떻게 노출하는지 upstream 코드(`lib/libunvmed-cmds.c`)로 재확인 필요

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Set Features / Get Features 커맨드, Feature Identifiers, Host Memory Buffer (FID 0Dh)
- 관련 컨셉: `unvmed-hmb.md`, `unvmed-cmd-alloc.md`, `unvmed-cmd-status.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
