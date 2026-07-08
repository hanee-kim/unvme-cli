---
type: Cython Binding Target
title: unvmed_cqe_status / enum unvmed_cmd_status
description: CQE 상태 필드 해석 헬퍼와 libunvmed 전용 상태 코드(SCT/SC) 정의.
resource: lib/libunvmed.h::unvmed_cqe_status
tags: [libunvmed, upstream, error]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

## 시그니처

```c
enum unvmed_cmd_status {
	NVME_SCT_UNVME	= 6,		/* libunvmed-specific SCT */
	NVME_SC_UNVME_TIMED_OUT	= 1,
};

static inline int unvmed_cqe_status(struct nvme_cqe *cqe)
{
	uint16_t status = le16_to_cpu((cqe)->sfp) >> 1;
	uint8_t sct = status >> 8;
	uint8_t sc = status & 0xff;

	if (sct == NVME_SCT_UNVME && sc == NVME_SC_UNVME_TIMED_OUT)
		return -ETIMEDOUT;

	return status;
}
```

## 설명

- CQE의 Status Field(`sfp`)에서 Phase bit를 제거한 15비트 상태값을 반환한다. 상위 8비트가 SCT(Status Code Type), 하위 8비트가 SC(Status Code)다.
- libunvmed는 스펙에 정의되지 않은 자체 SCT 값 `NVME_SCT_UNVME(6)`을 사용하며, `NVME_SC_UNVME_TIMED_OUT(1)`과 조합되면 커맨드 타임아웃을 의미한다. 이 경우 `unvmed_cqe_status()`는 `-ETIMEDOUT`을 반환한다.
- 그 외에는 (SCT<<8 | SC) 값을 그대로 반환하므로, 0이면 성공(Generic/Successful Completion)이다.
- 참고로 커맨드 발행 계열 API(`unvmed_read`, `unvmed_set_features` 등)의 반환값 규약도 "0: 성공, 그 외: CQE status field"로 이 값과 동일한 인코딩을 쓴다.
- 리셋 경로에서는 in-flight 커맨드에 대해 "Command Aborted By Host"(SCT=PATH(0x3), SC=0x71) 상태의 인공 CQE가 생성된다 (`unvmed_cancel_allocated_state_cmds`, `unvmed_reset_ctrl` 주석 참고).

## ctests에서의 사용 맥락

(추정) 테스트 assertion의 핵심 지점. Cython 바인딩에서 CQE를 받아 `unvmed_cqe_status()`로 (SCT, SC)를 분해해 기대 상태 코드와 비교하는 헬퍼(예: `assert status == 0`, 에러 주입 테스트에서 특정 SC 기대)로 감쌀 가능성이 높다. `-ETIMEDOUT` 특수 케이스를 파이썬 예외로 변환하는 처리도 필요할 것이다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Status Field (SCT/SC 정의), Path Related Status (Command Aborted By Host)
- 관련 컨셉: `struct-unvme-cmd.md`, `unvmed-cancel.md`, `unvmed-reset.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
