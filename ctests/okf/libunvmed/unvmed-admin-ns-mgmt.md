---
type: Cython Binding Target
title: unvmed_create_ns / attach_ns / detach_ns / delete_ns / format / virt_mgmt
description: Namespace Management/Attachment, Format NVM, Virtualization Management admin 커맨드 API.
resource: lib/libunvmed.h::unvmed_create_ns
tags: [libunvmed, upstream, admin]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/* Namespace Management - Create
 * (NSZE, NCAP, FLBAS, DPS, NMIC, ANAGRPID, NVMSETID, ENDGID, CSI,
 *  LBSTM, NPHNDLS, PHNDLS 필드 지정) */
int unvmed_cmd_prep_create_ns(struct unvme_cmd *cmd, uint64_t nsze,
			      uint64_t ncap, uint8_t flbas, uint8_t dps,
			      uint8_t nmic, uint32_t anagrp_id,
			      uint16_t nvmset_id, uint16_t endg_id,
			      uint8_t csi, uint64_t lbstm, uint16_t nphndls,
			      uint16_t *phndls, struct iovec *iov, int nr_iov);
int unvmed_create_ns(struct unvme_cmd *cmd, uint64_t nsze, uint64_t ncap,
		     uint8_t flbas, uint8_t dps, uint8_t nmic,
		     uint32_t anagrp_id, uint16_t nvmset_id, uint16_t endg_id,
		     uint8_t csi, uint64_t lbstm, uint16_t nphndls,
		     uint16_t *phndls, struct iovec *iov, int nr_iov);

/* Namespace Management - Delete */
int unvmed_cmd_prep_delete_ns(struct unvme_cmd *cmd, uint32_t nsid);
int unvmed_delete_ns(struct unvme_cmd *cmd, uint32_t nsid);

/* Namespace Attachment - Attach/Detach (controller list를 iov로 전달) */
int unvmed_cmd_prep_attach_ns(struct unvme_cmd *cmd, uint32_t nsid,
			      int nr_ctrlids, uint16_t *ctrlids,
			      struct iovec *iov, int nr_iov);
int unvmed_attach_ns(struct unvme_cmd *cmd, uint32_t nsid,
		     int nr_ctrlids, uint16_t *ctrlids,
		     struct iovec *iov, int nr_iov);
int unvmed_cmd_prep_detach_ns(struct unvme_cmd *cmd, uint32_t nsid,
			      int nr_ctrlids, uint16_t *ctrlids,
			      struct iovec *iov, int nr_iov);
int unvmed_detach_ns(struct unvme_cmd *cmd, uint32_t nsid,
		     int nr_ctrlids, uint16_t *ctrlids,
		     struct iovec *iov, int nr_iov);

/* Format NVM
 * @lbaf: LBA format index, @ses: secure erase settings,
 * @pil: PI location, @pi: protection information, @mset: metadata settings */
int unvmed_cmd_prep_format(struct unvme_cmd *cmd, uint32_t nsid, uint8_t lbaf,
			   uint8_t ses, uint8_t pil, uint8_t pi, uint8_t mset);
int unvmed_format(struct unvme_cmd *cmd, uint32_t nsid, uint8_t lbaf,
		  uint8_t ses, uint8_t pil, uint8_t pi, uint8_t mset);

/* Virtualization Management
 * @rt: resource type (VI/VQ Resource), @act: action, @nr: 리소스 수 */
int unvmed_cmd_prep_virt_mgmt(struct unvme_cmd *cmd, uint32_t cntlid,
			      uint32_t rt, uint32_t act, uint32_t nr);
int unvmed_virt_mgmt(struct unvme_cmd *cmd, uint32_t cntlid, uint32_t rt,
		     uint32_t act, uint32_t nr);
```

## 설명

- **Namespace Management**: `unvmed_create_ns()`는 Identify Namespace 데이터 구조의 주요 필드(NSZE/NCAP/FLBAS/DPS/NMIC/ANAGRPID/NVMSETID/ENDGID/CSI/LBSTM/placement handle)를 인자로 받아 Create 동작을 수행한다. 헤더 주석 기준 발행 버전은 "command result on success (>= 0) or a negative error" 반환. `unvmed_delete_ns()`는 nsid로 삭제.
- **Namespace Attachment**: attach/detach 모두 controller ID 배열(`ctrlids`)과 데이터 버퍼 iov를 받는다.
- **Format NVM**: LBAF/SES/PIL/PI/MSET 필드로 포맷을 수행한다. prep 버전 외 발행 버전 주석에는 반환값 명시가 없다.
- **Virtualization Management**: flexible resource 관리나 secondary controller online/offline 설정에 사용 (헤더 주석 기준).
- prep 버전은 모두 "0 성공 / -1 실패, 호출자가 명시적으로 제출" 규약이다.

## ctests에서의 사용 맥락

(추정) 네임스페이스 생성/attach/포맷은 테스트 전제 조건을 만드는 fixture(예: 특정 LBA 포맷·PI 설정으로 재포맷 후 테스트)에서 사용된다. Format 후에는 `unvmed_init_ns()`/`unvmed_init_meta_ns()`로 드라이버 컨텍스트를 갱신해야 할 것이다. virt_mgmt는 SR-IOV 테스트 전용.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Namespace Management, Namespace Attachment, Virtualization Management 커맨드; NVM Command Set Specification의 Format NVM 커맨드
- 관련 컨셉: `unvmed-ns-driver.md`, `unvmed-admin-identify.md` (CNS 14h/15h), `struct-unvme-ns.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
