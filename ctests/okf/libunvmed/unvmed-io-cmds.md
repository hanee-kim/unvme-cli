---
type: Cython Binding Target
title: unvmed_read / unvmed_write (I/O 커맨드)
description: end-to-end PI 파라미터를 포함한 Read/Write I/O 커맨드 발행 및 prep API.
resource: lib/libunvmed.h::unvmed_read
tags: [libunvmed, upstream, io]
upstream_repo: https://github.com/SamsungDS/unvme-cli
upstream_commit: e555bb7e976c96584a28ccffe12c72c5ba6ba597
timestamp: 2026-07-08
---

## 시그니처

```c
/**
 * @slba: start logical block address
 * @nlb: number of logical blocks (0-based)
 * @prinfo: prinfo for end-to-end pi
 * @atag/@atag_mask: app tag / mask, @rtag: reference tag,
 * @stag/@stag_check: storage tag / check 여부
 * @mbuf: metadata buffer
 * @opaque: 비동기 완료용 opaque private data
 *
 * prep: 0 성공 / -1 실패(errno). 발행: 0 성공 / 그 외 CQE status field.
 * 모두 thread-safe
 */
int unvmed_cmd_prep_read(struct unvme_cmd *cmd, uint32_t nsid, uint64_t slba,
			 uint16_t nlb, uint8_t prinfo, uint16_t atag,
			 uint16_t atag_mask, uint64_t rtag, uint64_t stag,
			 bool stag_check, struct iovec *iov, int nr_iov,
			 void *mbuf, void *opaque);
int unvmed_read(struct unvme_cmd *cmd, uint32_t nsid, uint64_t slba,
		uint16_t nlb, uint8_t prinfo, uint16_t atag, uint16_t atag_mask,
		uint64_t rtag, uint64_t stag, bool stag_check,
		struct iovec *iov, int nr_iov, void *mbuf, void *opaque);

int unvmed_cmd_prep_write(struct unvme_cmd *cmd, uint32_t nsid, uint64_t slba,
		uint16_t nlb, uint8_t prinfo, uint16_t atag, uint16_t atag_mask,
		uint64_t rtag, uint64_t stag, bool stag_check,
		struct iovec *iov, int nr_iov, void *mbuf, void *opaque);
int unvmed_write(struct unvme_cmd *cmd, uint32_t nsid, uint64_t slba,
		 uint16_t nlb, uint8_t prinfo, uint16_t atag,
		 uint16_t atag_mask, uint64_t rtag, uint64_t stag,
		 bool stag_check, struct iovec *iov, int nr_iov, void *mbuf,
		 void *opaque);
```

## 설명

- NVM command set의 Read/Write 커맨드를 발행한다. `nlb`는 스펙 규약대로 0-based(0 = 1블록)다.
- end-to-end data protection 파라미터(prinfo, app tag/mask, reference tag, storage tag/check)를 모두 노출하므로 PI 활성 네임스페이스에 대한 테스트가 가능하다.
- 데이터는 `iov`/`nr_iov`, 별도 버퍼 방식 메타데이터는 `mbuf`로 전달한다 (DIX 모드에서 유효 — `struct unvme_cmd`의 mbuf 주석 참고).
- `opaque`는 비동기 완료 시 사용할 호출자 소유 데이터 포인터다.
- prep/발행 두 층 구조와 반환값 규약은 다른 커맨드 계열과 동일하다.

## ctests에서의 사용 맥락

(추정) 데이터 경로 테스트의 핵심 바인딩. write → read → 데이터 비교(verify) 패턴, PI 필드 조작에 따른 상태 코드 검증, nlb/slba 경계값 테스트 등에 직접 사용된다. 파이썬 버퍼 프로토콜과 iovec 간 변환 헬퍼가 바인딩 계층에 필요할 것이다.

## 관련

- 관련 NVMe 스펙 조항: NVM Command Set Specification의 Read/Write 커맨드 (SLBA, NLB, PRINFO 필드), End-to-end Data Protection (App/Ref/Storage Tag)
- 관련 컨셉: `unvmed-cmd-alloc.md`, `unvmed-cmd-submit.md`, `struct-unvme-ns.md`, `unvmed-prp-sgl.md`
- 관련 ctests 컨셉: `[TODO: ctests 쪽 fixture/바인딩 문서와 나중에 연결]`
