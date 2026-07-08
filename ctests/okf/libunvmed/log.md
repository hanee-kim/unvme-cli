---
type: Log
title: libunvmed OKF 문서 버전 추적 로그
description: 문서 작성 시 참조한 upstream 커밋과 스냅샷 날짜 기록.
tags: [libunvmed, upstream, log]
---

# 버전 추적 로그

| 날짜 (스냅샷) | upstream 커밋 | 커밋 날짜 | 비고 |
|---|---|---|---|
| 2026-07-08 | `26f62dc5c3793497b541635d50230949ff704ce7` | 2026-05-14 (KST) | 최초 작성. 컨셉 문서 29건 + index. 소스 범위: `lib/libunvmed.h`, `lib/libunvmed-logs.h` (공개 헤더 기준) |

## 참조 방법 메모

- 이번 스냅샷은 `hanee-kim/unvme-cli` fork의 clone(HEAD)에서 읽었다. 커밋 메시지: "libunvmed: add UNVME_CMD_S_ALLOCATED state".
  - > TODO: 위 커밋 해시가 upstream `SamsungDS/unvme-cli`에 동일하게 존재하는지 재확인 필요 (작성 환경에서 upstream 저장소 접근이 제한되어 fork 기준으로 기록함)
- 재검토 절차: upstream 최신 커밋과 위 해시 사이에 `lib/` 디렉토리 변경이 있으면 (`git log <hash>..HEAD -- lib/`), 변경된 심볼이 속한 문서를 골라 재검토하고 이 표에 새 행을 추가한다.
