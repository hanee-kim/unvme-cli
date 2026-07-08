---
type: Cython Binding Target
title: struct unvme_ns
description: 드라이버 컨텍스트에 등록된 네임스페이스 인스턴스 구조체 (LBA 포맷, 메타데이터, PI 정보 포함).
resource: lib/libunvmed.h::unvme_declare_ns
tags: [libunvmed, upstream, controller]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/*
 * struct unvme_ns - Namespace instance
 * 매크로로 선언되며 unvme_declare_ns(unvme_ns); 로 struct unvme_ns가 정의됨
 */
#define unvme_declare_ns(name)	\
struct name {			\
	struct unvme *u;	\
				\
	int refcnt;		\
	bool enabled;		\
				\
	uint32_t nsid;		\
	uint8_t format_idx;	\
	unsigned int lba_size;	\
	unsigned long nr_lbas;	\
				\
	uint16_t ms;		\
	uint8_t mset;		\
	uint8_t pif;		\
	uint8_t dps;		\
	uint8_t sts;		\
	uint64_t lbstm;		\
}
```

## 설명

헤더 주석 기준 필드 의미:

| 필드 | 의미 |
|---|---|
| `u` | 소속 unvme 컨트롤러 인스턴스 |
| `refcnt` | 참조 카운트 (`unvmed_ns_get`/`unvmed_ns_put`으로 관리) |
| `enabled` | 인스턴스가 enabled 상태이면 `true` |
| `nsid` | 네임스페이스 식별자 |
| `format_idx` | Identify Namespace 데이터의 LBA format index |
| `lba_size` | LBA 크기 (바이트) |
| `nr_lbas` | 논리 블록 개수 |
| `ms` | 메타데이터 크기 (바이트) |
| `mset` | 메타데이터 설정 (1: extended data LBA로 전송, 그 외: 별도 버퍼로 전송) |
| `pif` | Protection Information Format (00b: 16b, 01b: 32b, 10b: 64b guard) |
| `dps` | 데이터 보호 설정 (헤더 주석에는 항목 설명 없음) |
| `sts` | Storage Tag Size |
| `lbstm` | Logical Block Storage Tag Mask |

인스턴스는 `unvmed_init_ns()` / `unvmed_init_meta_ns()`로 드라이버 컨텍스트에 등록되고, `unvmed_ns_get()`/`unvmed_ns_find()`/`unvmed_get_nslist()`로 조회한다.

## ctests에서의 사용 맥락

(추정) 테스트에서 대상 네임스페이스의 `lba_size`, `nr_lbas`, `ms`, `pif` 등을 읽어 I/O 크기 계산이나 end-to-end PI 테스트 파라미터 결정에 사용할 가능성이 높다. Cython에서는 매크로 전개 결과인 `struct unvme_ns`를 그대로 cdef 선언해 필드 접근을 제공하면 된다.

## 관련

- 관련 NVMe 스펙 조항: NVMe NVM Command Set Specification의 Identify Namespace 데이터 구조 (FLBAS, MS, DPS 등), End-to-end Data Protection
- 관련 컨셉: `unvmed-ns-driver.md`, `unvmed-admin-identify.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
