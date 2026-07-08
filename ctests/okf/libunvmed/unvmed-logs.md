---
type: Cython Binding Target
title: unvmed_log_* (로깅 API)
description: 로그 레벨 기반 로깅 매크로와 커맨드 post/completion 트레이스 로깅 함수.
resource: lib/libunvmed-logs.h::unvmed_log_err
tags: [libunvmed, upstream, logging]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

## 시그니처

```c
enum {
	UNVME_LOG_ERR,
	UNVME_LOG_INFO,
	UNVME_LOG_DEBUG,
	UNVME_LOG_LAST = UNVME_LOG_DEBUG,
};

/* 로그 레벨 필터링 + 타임스탬프/함수명/라인 포함 포맷 매크로 */
#define unvmed_log_err(fmt, ...)
#define unvmed_log_info(fmt, ...)
#define unvmed_log_debug(fmt, ...)

/* libunvmed-logs.c — 커맨드 트레이스 로깅 */
void unvmed_log_cmd_post(const char *bdf, uint32_t sqid, union nvme_cmd *sqe);
void unvmed_log_cmd_cmpl(const char *bdf, struct nvme_cqe *cqe);
void unvmed_log_cmd_vcq_push(struct nvme_cqe *cqe);
void unvmed_log_cmd_vcq_pop(struct nvme_cqe *cqe);
```

## 설명

- 로그 출력은 `unvmed_init(logfile, log_level)`로 설정된 파일 디스크립터(`__unvmed_logfd`)와 레벨(`__log_level`)에 따라 필터링된다. logfd가 0이면 아무것도 기록하지 않는다.
- 각 로그 라인은 `레벨 | 날짜시간(usec 포함) | 함수명: 라인: 메시지` 형식이다.
- `unvmed_log_cmd_post/cmpl/vcq_push/vcq_pop()`은 커맨드 제출·완료·vcq push/pop 시점의 SQE/CQE 내용을 로깅하는 트레이스 함수다 (구현은 `lib/libunvmed-logs.c`).

## ctests에서의 사용 맥락

(추정) 테스트가 직접 호출할 일은 적고, 실패 분석 시 로그 파일을 아티팩트로 수집하는 용도가 주가 될 것이다. `unvmed_init()`의 log_level 인자를 pytest 옵션(예: -v 수준)과 연동하는 정도의 바인딩이면 충분해 보인다.

## 관련

- 관련 NVMe 스펙 조항: 해당 없음 (호스트 측 로깅 유틸리티)
- 관련 컨셉: `unvmed-init.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
