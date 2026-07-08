---
type: Cython Binding Target
title: unvmed_init / unvmed_parse_bdf
description: 프로세스 컨텍스트에서 libunvmed 라이브러리를 초기화하고 PCI BDF 문자열을 정규화하는 진입점 API.
resource: lib/libunvmed.h::unvmed_init
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/**
 * @logfile: logfile path, NULL if no-log mode
 * @log_level: log level (0: ERROR, 1: INFO, 2: DEBUG)
 */
void unvmed_init(const char *logfile, int log_level);

/**
 * @input: input string to be parsed (e.g., "0:1")
 * @bdf: output string which is fully formed as a PCI BDF format
 *       (e.g. "0000:01:00.0")
 * Return: 0 on success, otherwise -1 with errno set.
 */
int unvmed_parse_bdf(const char *input, char *bdf);
```

## 설명

- `unvmed_init()`은 현재 프로세스 컨텍스트에서 libunvmed를 초기화한다. `logfile`이 NULL이면 로그를 남기지 않으며, `log_level`은 0(ERROR)/1(INFO)/2(DEBUG)이다 (`lib/libunvmed-logs.h`의 `UNVME_LOG_*` enum과 대응).
- `unvmed_parse_bdf()`는 축약형 입력(예: `"0:1"`)을 완전한 PCI BDF 형식(예: `"0000:01:00.0"`)으로 변환한다.

## ctests에서의 사용 맥락

(추정) pytest 세션 시작 시(예: session-scoped fixture) 한 번 `unvmed_init()`을 호출해 로그 파일을 설정하고, 사용자가 넘긴 디바이스 지정 문자열을 `unvmed_parse_bdf()`로 정규화한 뒤 `unvmed_init_ctrl()`에 전달하는 흐름이 예상된다.

## 관련

- 관련 NVMe 스펙 조항: 해당 없음 (라이브러리/PCI 주소 유틸리티)
- 관련 컨셉: `unvmed-ctrl-lifecycle.md`, `unvmed-logs.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
