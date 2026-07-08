---
type: Cython Binding Target
title: unvmed_init_ns / unvmed_ns_get 계열 (드라이버 측 네임스페이스 관리)
description: 네임스페이스 인스턴스를 드라이버 컨텍스트에 등록·조회·해제하고 Identify Controller 데이터를 보관하는 API.
resource: lib/libunvmed.h::unvmed_init_ns
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
int unvmed_init_ns(struct unvme *u, uint32_t nsid, void *identify);
int unvmed_init_meta_ns(struct unvme *u, uint32_t nsid, void *nvm_id_ns);

struct unvme_ns *unvmed_ns_get(struct unvme *u, uint32_t nsid);
int unvmed_ns_put(struct unvme *u, struct unvme_ns *ns);
struct unvme_ns *unvmed_ns_find(struct unvme *u, uint32_t nsid);
int unvmed_get_nslist(struct unvme *u, struct unvme_ns **nslist);

int unvmed_init_id_ctrl(struct unvme *u, void *id_ctrl);
ssize_t unvmed_get_max_xfer_size(struct unvme *u);
```

## 설명

- `unvmed_init_ns(u, nsid, identify)`: 네임스페이스 인스턴스를 드라이버 컨텍스트에 등록한다. `identify`가 non-NULL이면 이미 얻어둔 Identify Namespace 데이터로 간주해 커맨드 발행을 생략하고, NULL이면 직접 Identify Namespace admin 커맨드를 발행해 등록한다. thread-safe.
- `unvmed_init_meta_ns(u, nsid, nvm_id_ns)`: 메타데이터 관련 정보를 네임스페이스 인스턴스에 설정한다. 컨트롤러가 이전에 identify되지 않았다면 ELBAS(Extended LBA Support) 확인을 위해 Identify Controller 커맨드를 발행하며, `nvm_id_ns` NULL 여부에 따른 동작은 `unvmed_init_ns`와 동일한 패턴(NVM command set용 Identify Namespace, CNS 5h). thread-safe.
- `unvmed_ns_get()`/`unvmed_ns_put()`: refcnt를 올리고/내리며 인스턴스를 획득/반납. refcnt가 0에 도달하면 해제된다. `unvmed_ns_find()`는 refcnt 변경 없이 조회만 한다.
- `unvmed_get_nslist()`: 드라이버 컨텍스트가 인지한(= `unvmed_init_ns()`된) 네임스페이스 목록을 복사해 준다. 실제 하드웨어에 attach된 네임스페이스라도 driver 컨텍스트가 본 적이 없으면 포함되지 않는다.
- `unvmed_init_id_ctrl()`: Identify Controller 데이터를 `u->id_ctrl`에 복사·보관해 런타임에 참조 가능하게 한다.
- `unvmed_get_max_xfer_size()`: 최대 I/O 전송 크기(바이트)를 반환한다 (실패 시 -1).

## ctests에서의 사용 맥락

(추정) 네임스페이스 fixture에서 Identify 발행 후 `unvmed_init_ns()`로 등록하고, 테스트 본문에서는 `unvmed_ns_get()`으로 얻은 `struct unvme_ns`의 lba_size/nr_lbas로 I/O 파라미터를 계산하는 흐름. `unvmed_get_max_xfer_size()`는 MDTS 경계 테스트에 사용될 수 있다.

## 관련

- 관련 NVMe 스펙 조항: NVMe Base Specification의 Identify 커맨드 (CNS 0h Identify Namespace, CNS 1h Identify Controller), MDTS; NVM Command Set Specification의 CNS 5h (I/O Command Set specific Identify Namespace)
- 관련 컨셉: `struct-unvme-ns.md`, `unvmed-admin-identify.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
