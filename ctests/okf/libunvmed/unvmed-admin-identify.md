---
type: Cython Binding Target
title: unvmed_id_ctrl / unvmed_id_ns 계열 (Identify admin 커맨드)
description: Identify Controller/Namespace/Active NS List/NVM Identify NS/Primary·Secondary Controller 커맨드 발행 및 prep API.
resource: lib/libunvmed.h::unvmed_id_ctrl
tags: [libunvmed, upstream, admin]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/* 각 커맨드는 prep(준비만)과 발행(완료 대기 포함) 두 형태 제공.
 * prep: 0 성공 / -1 실패(errno). 발행: 0 성공 / 그 외 CQE status field.
 * 모두 thread-safe */

/* Identify Namespace (CNS 0h) */
int unvmed_cmd_prep_id_ns(struct unvme_cmd *cmd, uint32_t nsid,
			  struct iovec *iov, int nr_iov);
int unvmed_id_ns(struct unvme_cmd *cmd, uint32_t nsid,
		 struct iovec *iov, int nr_iov);

/* Identify Controller (CNS 1h) */
int unvmed_cmd_prep_id_ctrl(struct unvme_cmd *cmd, struct iovec *iov, int nr_iov);
int unvmed_id_ctrl(struct unvme_cmd *cmd, struct iovec *iov, int nr_iov);

/* Identify Active Namespace ID List (CNS 2h) */
int unvmed_cmd_prep_id_active_nslist(struct unvme_cmd *cmd, uint32_t nsid,
				     struct iovec *iov, int nr_iov);
int unvmed_id_active_nslist(struct unvme_cmd *cmd, uint32_t nsid,
			    struct iovec *iov, int nr_iov);

/* I/O Command Set specific Identify Namespace, NVM (CNS 5h) */
int unvmed_cmd_prep_nvm_id_ns(struct unvme_cmd *cmd, uint32_t nsid,
			      struct iovec *iov, int nr_iov);
int unvmed_nvm_id_ns(struct unvme_cmd *cmd, uint32_t nsid, struct iovec *iov,
		     int nr_iov);

/* Identify Primary Controller Capabilities (CNS 14h) */
int unvmed_cmd_prep_id_primary_ctrl_caps(struct unvme_cmd *cmd,
					 struct iovec *iov, int nr_iov,
					 uint32_t cntlid);
int unvmed_id_primary_ctrl_caps(struct unvme_cmd *cmd, struct iovec *iov,
				int nr_iov, uint32_t cntlid);

/* Identify Secondary Controller List (CNS 15h)
 * @cntlid: lowest controller identifier to display */
int unvmed_cmd_prep_id_secondary_ctrl_list(struct unvme_cmd *cmd,
					   struct iovec *iov, int nr_iov,
					   uint32_t cntlid);
int unvmed_id_secondary_ctrl_list(struct unvme_cmd *cmd, struct iovec *iov,
				  int nr_iov, uint32_t cntlid);
```

## 설명

- 각 Identify 변형(CNS 값은 헤더 주석에 명시: 0h/1h/2h/5h/14h/15h)에 대해 두 층의 API가 있다:
  - `unvmed_cmd_prep_*()`: SQE를 구성하고 iov를 매핑만 함. 이후 `unvmed_cmd_issue_and_wait()` 등으로 발행.
  - prep 없는 버전: 발행 및 완료까지 수행. 반환값 0은 성공, 그 외에는 CQE status field 값.
- 결과 데이터(4KiB Identify 데이터 구조 등)는 호출자가 준비한 `iov` 버퍼로 수신된다.
- CNS 14h/15h는 SR-IOV 가상화 관련 커맨드로 `unvmed_virt_mgmt()`와 함께 사용된다 (`unvmed-admin-ns-mgmt.md`의 Virtualization Management 항목 참고).

## ctests에서의 사용 맥락

(추정) 거의 모든 테스트 세션 초기화에서 사용되는 기본 admin 커맨드. Identify 결과 파싱은 파이썬 측(예: ctypes/struct 언패킹 또는 별도 파서)에서 수행하고, 여기서는 raw 버퍼만 주고받는 형태의 바인딩이 예상된다. `unvmed_init_ns()`/`unvmed_init_id_ctrl()`과 조합해 드라이버 컨텍스트 등록까지 이어지는 흐름도 함께 래핑될 수 있다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Identify 커맨드 및 CNS 값 정의 (0h, 1h, 2h, 14h, 15h); NVM Command Set Specification의 CNS 5h
- 관련 컨셉: `unvmed-ns-driver.md`, `unvmed-cmd-alloc.md`, `unvmed-admin-ns-mgmt.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
