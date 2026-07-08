---
type: Index
title: libunvmed OKF 컨셉 문서 목차
description: 외부 저장소 SamsungDS/unvme-cli의 lib/(libunvmed) 공개 API에 대한 OKF 컨셉 문서 인덱스.
tags: [libunvmed, upstream, index]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: 26f62dc5c3793497b541635d50230949ff704ce7
timestamp: 2026-07-08
---

# libunvmed OKF 컨셉 문서

> **출처 고지**: 이 카테고리는 [SamsungDS/unvme-cli](https://github.com/SamsungDS/unvme-cli)의 `lib/`(libunvmed) 소스를 기반으로 함. 원 라이선스는 `lib/COPYING`(LGPL-2.1-or-later) 및 `lib/LICENSE`(MIT) 참고 — `lib/` 소스는 LGPL-2.1-or-later / MIT 듀얼 라이선스이며, unvme-cli 프로젝트 전체 내에서는 GPL-2.0-only로 취급됨. 이 문서들은 upstream 코드를 요약·재작성한 설명임.

> **버전 스냅샷**: 이 문서들은 upstream 커밋 `26f62dc5c3793497b541635d50230949ff704ce7` 기준의 스냅샷이다. upstream이 갱신되면 각 문서 frontmatter의 `upstream_commit`과 최신 커밋을 비교해 재검토 대상을 선별할 것. 버전 추적 기록은 [`log.md`](log.md) 참고.

## 구조체 / 타입

| 문서 | 심볼 | 분류 |
|---|---|---|
| [struct-unvme.md](struct-unvme.md) | `struct unvme`, `enum unvme_state` | controller |
| [struct-unvme-ns.md](struct-unvme-ns.md) | `struct unvme_ns` (`unvme_declare_ns`) | controller |
| [struct-unvme-sq-cq.md](struct-unvme-sq-cq.md) | `struct unvme_sq`, `struct unvme_cq`, 락/접근자 헬퍼 | io |
| [struct-unvme-cmd.md](struct-unvme-cmd.md) | `struct unvme_cmd`, `enum unvme_cmd_state`, `enum unvmed_cmd_flags`, `struct unvme_buf` | io |
| [struct-unvme-vcq.md](struct-unvme-vcq.md) | `struct unvme_vcq`, `struct unvme_vcqe`, `unvmed_vcq_*` | io |
| [unvmed-cmd-status.md](unvmed-cmd-status.md) | `unvmed_cqe_status()`, `enum unvmed_cmd_status` | error |

## 컨트롤러 생명주기 / 상태 관리

| 문서 | 심볼 | 분류 |
|---|---|---|
| [unvmed-init.md](unvmed-init.md) | `unvmed_init`, `unvmed_parse_bdf` | controller |
| [unvmed-ctrl-lifecycle.md](unvmed-ctrl-lifecycle.md) | `unvmed_init_ctrl`, `unvmed_get`, `unvmed_free_ctrl[_all]`, `unvmed_ctrl_enabled` | controller |
| [unvmed-adminq.md](unvmed-adminq.md) | `unvmed_create_adminq`, `unvmed_configure_adminq` | admin/controller |
| [unvmed-enable-ctrl.md](unvmed-enable-ctrl.md) | `unvmed_enable_ctrl` | controller |
| [unvmed-reset.md](unvmed-reset.md) | `unvmed_reset_ctrl[_graceful]`, `unvmed_subsystem_reset`, `unvmed_flr`, `unvmed_hot_reset`, `unvmed_link_disable` | controller |
| [unvmed-ctx.md](unvmed-ctx.md) | `unvmed_ctx_init/restore/free`, `unvmed_disable_ctx`, `unvmed_reset_ctx`, `unvmed_free_ctx`, `unvmed_[un]quiesce_sq[_all]` | controller |
| [unvmed-ns-driver.md](unvmed-ns-driver.md) | `unvmed_init_ns`, `unvmed_init_meta_ns`, `unvmed_ns_get/put/find`, `unvmed_get_nslist`, `unvmed_init_id_ctrl`, `unvmed_get_max_xfer_size` | controller |
| [unvmed-hmb.md](unvmed-hmb.md) | `struct unvme_hmb`, `unvmed_hmb_*` | controller |
| [unvmed-cmb.md](unvmed-cmb.md) | `struct unvme_cmb`, `unvmed_cmb_*` | controller |
| [unvmed-misc.md](unvmed-misc.md) | `unvmed_nr_cmds`, `unvmed_nr_irqs`, `unvmed_to_json`, MMIO 헬퍼 | controller |

## I/O 큐 / 커맨드 경로

| 문서 | 심볼 | 분류 |
|---|---|---|
| [unvmed-io-queues.md](unvmed-io-queues.md) | `unvmed_create_sq/cq`, `unvmed_init_sq/cq[_iova]`, `unvmed_enable/disable_sq/cq`, `unvmed_free_sq/cq`, `unvmed_sq/cq_get/put/find`, `unvmed_get_sqs/cqs` | io |
| [unvmed-cmd-alloc.md](unvmed-cmd-alloc.md) | `unvmed_alloc_cmd[_nodata/_meta]`, `unvmed_cmd_get/put` | io |
| [unvmed-cmd-submit.md](unvmed-cmd-submit.md) | `unvmed_cmd_prep`, `unvmed_cmd_post`, `unvmed_sq_update_tail`, `unvmed_cmd_issue_and_wait`, `unvmed_cmd_wait`, `unvmed_passthru` | io |
| [unvmed-cq-reap.md](unvmed-cq-reap.md) | `unvmed_cq_run`, `unvmed_cq_run_n`, `__unvmed_cq_run_n` | io |
| [unvmed-dma.md](unvmed-dma.md) | `unvmed_to_iova/vaddr`, `unvmed_map/unmap_vaddr`, `unvmed_mem_alloc/get/free`, `unvmed_pgmap*`, `unvmed_pagesize` | memory |
| [unvmed-prp-sgl.md](unvmed-prp-sgl.md) | `[__]unvmed_mapv_prp[_list]`, `[__]unvmed_mapv_sgl[_seg]` | io |
| [unvmed-cancel.md](unvmed-cancel.md) | `unvmed_cancel_cmd`, `unvmed_cancel_allocated_state_cmds` | io/error |

## NVMe 커맨드 래퍼

| 문서 | 심볼 | 분류 |
|---|---|---|
| [unvmed-admin-identify.md](unvmed-admin-identify.md) | `unvmed_id_ctrl`, `unvmed_id_ns`, `unvmed_id_active_nslist`, `unvmed_nvm_id_ns`, `unvmed_id_primary_ctrl_caps`, `unvmed_id_secondary_ctrl_list` (+ prep) | admin |
| [unvmed-admin-features.md](unvmed-admin-features.md) | `unvmed_set_features[_hmb]`, `unvmed_get_features` (+ prep) | admin |
| [unvmed-admin-queue-cmds.md](unvmed-admin-queue-cmds.md) | `unvmed_cmd_prep_create/delete_sq/cq` | admin |
| [unvmed-admin-ns-mgmt.md](unvmed-admin-ns-mgmt.md) | `unvmed_create/delete/attach/detach_ns`, `unvmed_format`, `unvmed_virt_mgmt` (+ prep) | admin |
| [unvmed-io-cmds.md](unvmed-io-cmds.md) | `unvmed_read`, `unvmed_write` (+ prep) | io |

## 로깅

| 문서 | 심볼 | 분류 |
|---|---|---|
| [unvmed-logs.md](unvmed-logs.md) | `unvmed_log_err/info/debug`, `unvmed_log_cmd_*` | logging |

## 범위 밖 (비공개)

- `lib/libunvmed-private.h` — `struct unvme` 실제 정의 등 내부 구현. 공개 API가 아니므로 문서화하지 않음.
