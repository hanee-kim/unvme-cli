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
| 2026-07-08 | `e555bb7e976c96584a28ccffe12c72c5ba6ba597` | 2026-06-26 (KST) | fork 업데이트 반영 재검토. `lib/` 변경 커밋 15건, 공개 헤더 변경: `UNVME_FATAL` 상태 추가, `unvmed_ctrl_get/set_state()` 공개화, `unvmed_get()` refcnt 동작 변경 + `unvmed_put()` 신설, `unvmed_get_epoch()` 신설, `unvmed_vcq_push()` 시그니처 변경 + `unvmed_vcq_push_to_other()` 신설, `unvme_reset_ctrl()` 신설, `unvmed_del_sq()` 신설, `unvmed_[un]map_vaddr()` ENODEV 문서화. 갱신 문서: struct-unvme, unvmed-ctrl-lifecycle, struct-unvme-vcq, unvmed-reset, unvmed-io-queues, unvmed-dma. 나머지 문서는 헤더 diff 상 무변경 확인 후 커밋 해시만 갱신 |

## 참조 방법 메모

- 이 스냅샷들은 `hanee-kim/unvme-cli` fork의 clone에서 읽었다.
  - > TODO: 위 커밋 해시가 upstream `SamsungDS/unvme-cli`에 동일하게 존재하는지 재확인 필요 (작성 환경에서 upstream 저장소 접근이 제한되어 fork 기준으로 기록함)
- 재검토 절차: upstream 최신 커밋과 최신 행의 해시 사이에 `lib/` 디렉토리 변경이 있으면 (`git log <hash>..HEAD -- lib/`), 공개 헤더 diff(`git diff <hash>..HEAD -- lib/libunvmed.h lib/libunvmed-logs.h`)로 변경 심볼을 확인해 해당 문서를 재검토하고 이 표에 새 행을 추가한다.
