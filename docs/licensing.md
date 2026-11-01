# 라이선스 관문 (License gate)

이 문서는 이후 모든 태스크가 따르는 라이선스 판정 세 가지를 기록한다.

1. 커널 `fs/xfs` 파일의 SPDX 결과
2. WinFsp FLOSS 예외 조항 판정
3. WinFsp FFI 방식 결정

프로젝트 라이선스는 **GPL-2.0-only**다. 사용자 결정에 따라 GPL-3.0 코드를 프로젝트(소스와 배포 바이너리)에 섞지 않는다.

> 작성자는 변호사가 아니다. 아래 판정은 라이선스 원문을 읽고 내린 엔지니어링 판단이며 법률 자문이 아니다.

## 1. 커널 `fs/xfs` SPDX 스캔

- 도구: `python3 -I tools/license-scan.py [--summary] <dir>` (각 `*.c`/`*.h`의 처음 5줄에서 `SPDX-License-Identifier`를 찾는다. 폐기된 표기 `GPL-2.0`은 `GPL-2.0-only`로 정규화한다.)
- 대상: `https://github.com/torvalds/linux` 커밋 `d135ab453a7400a34930bdda1ef4bdc8acfac80e` (2026-10-26), sparse checkout `fs/xfs`. 저장소 밖 스크래치 디렉터리에서 받았다.
- 스캔 일자: 2026-10-28

| 라이선스 | 파일 수 |
|---|---:|
| `GPL-2.0-only` (원문 표기 `GPL-2.0`) | 187 |
| `GPL-2.0-or-later` | 146 |
| `GPL-2.0+` (`GPL-2.0-or-later`의 폐기된 표기) | 22 |
| `LGPL-2.1` | 1 |
| 표기 없음 | 0 |
| **합계** | **356** |

`*.c`/`*.h`가 아닌 파일은 `fs/xfs/Makefile`, `fs/xfs/Kconfig` 둘뿐이다(빌드 설정이라 포팅 대상이 아니다).

### 해석

- 모든 파일이 GPL-2.0-only로 배포하는 결합 저작물에 들어갈 수 있다. `GPL-2.0-or-later`/`GPL-2.0+`는 GPLv2 조건으로 쓸 수 있고, `LGPL-2.1`은 LGPL-2.1 3절에 따라 GPLv2로 바꿔 쓸 수 있다.
- 따라서 이 프로젝트 전체를 GPL-2.0-only로 두는 방침과 충돌하는 파일은 없다. 반대로 이 코드를 포팅한 바이너리는 GPL-3.0 전용 코드와 결합할 수 없다(187개 파일이 GPL-2.0-only).
- 포팅한 파일에는 원본의 SPDX 줄과 저작권 표시를 그대로 유지한다.

### `GPL-2.0-only`가 아닌 파일 목록

#### `LGPL-2.1` (1)

- `fs/xfs/libxfs/xfs_fs.h`

#### `GPL-2.0+` (22)

- `fs/xfs/libxfs/xfs_ag_resv.c`
- `fs/xfs/libxfs/xfs_ag_resv.h`
- `fs/xfs/libxfs/xfs_bmap_item.h`
- `fs/xfs/libxfs/xfs_defer.c`
- `fs/xfs/libxfs/xfs_defer.h`
- `fs/xfs/libxfs/xfs_errortag.h`
- `fs/xfs/libxfs/xfs_health.h`
- `fs/xfs/libxfs/xfs_refcount.c`
- `fs/xfs/libxfs/xfs_refcount.h`
- `fs/xfs/libxfs/xfs_refcount_btree.c`
- `fs/xfs/libxfs/xfs_refcount_btree.h`
- `fs/xfs/libxfs/xfs_refcount_item.h`
- `fs/xfs/libxfs/xfs_rmap.h`
- `fs/xfs/libxfs/xfs_rmap_item.h`
- `fs/xfs/xfs_bmap_item.c`
- `fs/xfs/xfs_fsmap.c`
- `fs/xfs/xfs_fsmap.h`
- `fs/xfs/xfs_health.c`
- `fs/xfs/xfs_refcount_item.c`
- `fs/xfs/xfs_reflink.c`
- `fs/xfs/xfs_reflink.h`
- `fs/xfs/xfs_rmap_item.c`

#### `GPL-2.0-or-later` (146)

- `fs/xfs/libxfs/xfs_attr_item.h`
- `fs/xfs/libxfs/xfs_btree_staging.c`
- `fs/xfs/libxfs/xfs_btree_staging.h`
- `fs/xfs/libxfs/xfs_exchmaps.c`
- `fs/xfs/libxfs/xfs_exchmaps.h`
- `fs/xfs/libxfs/xfs_exchmaps_item.h`
- `fs/xfs/libxfs/xfs_metadir.c`
- `fs/xfs/libxfs/xfs_metadir.h`
- `fs/xfs/libxfs/xfs_metafile.c`
- `fs/xfs/libxfs/xfs_metafile.h`
- `fs/xfs/libxfs/xfs_rtgroup.c`
- `fs/xfs/libxfs/xfs_rtgroup.h`
- `fs/xfs/libxfs/xfs_rtrefcount_btree.c`
- `fs/xfs/libxfs/xfs_rtrefcount_btree.h`
- `fs/xfs/libxfs/xfs_rtrmap_btree.c`
- `fs/xfs/libxfs/xfs_rtrmap_btree.h`
- `fs/xfs/scrub/agb_bitmap.c`
- `fs/xfs/scrub/agb_bitmap.h`
- `fs/xfs/scrub/agheader.c`
- `fs/xfs/scrub/agheader_repair.c`
- `fs/xfs/scrub/agino_bitmap.h`
- `fs/xfs/scrub/alloc.c`
- `fs/xfs/scrub/alloc_repair.c`
- `fs/xfs/scrub/attr.c`
- `fs/xfs/scrub/attr.h`
- `fs/xfs/scrub/attr_repair.c`
- `fs/xfs/scrub/attr_repair.h`
- `fs/xfs/scrub/bitmap.c`
- `fs/xfs/scrub/bitmap.h`
- `fs/xfs/scrub/bmap.c`
- `fs/xfs/scrub/bmap_repair.c`
- `fs/xfs/scrub/btree.c`
- `fs/xfs/scrub/btree.h`
- `fs/xfs/scrub/common.c`
- `fs/xfs/scrub/common.h`
- `fs/xfs/scrub/cow_repair.c`
- `fs/xfs/scrub/dab_bitmap.h`
- `fs/xfs/scrub/dabtree.c`
- `fs/xfs/scrub/dabtree.h`
- `fs/xfs/scrub/dir.c`
- `fs/xfs/scrub/dir_repair.c`
- `fs/xfs/scrub/dirtree.c`
- `fs/xfs/scrub/dirtree.h`
- `fs/xfs/scrub/dirtree_repair.c`
- `fs/xfs/scrub/dqiterate.c`
- `fs/xfs/scrub/findparent.c`
- `fs/xfs/scrub/findparent.h`
- `fs/xfs/scrub/fsb_bitmap.h`
- `fs/xfs/scrub/fscounters.c`
- `fs/xfs/scrub/fscounters.h`
- `fs/xfs/scrub/fscounters_repair.c`
- `fs/xfs/scrub/health.c`
- `fs/xfs/scrub/health.h`
- `fs/xfs/scrub/ialloc.c`
- `fs/xfs/scrub/ialloc_repair.c`
- `fs/xfs/scrub/ino_bitmap.h`
- `fs/xfs/scrub/inode.c`
- `fs/xfs/scrub/inode_repair.c`
- `fs/xfs/scrub/iscan.c`
- `fs/xfs/scrub/iscan.h`
- `fs/xfs/scrub/listxattr.c`
- `fs/xfs/scrub/listxattr.h`
- `fs/xfs/scrub/metapath.c`
- `fs/xfs/scrub/newbt.c`
- `fs/xfs/scrub/newbt.h`
- `fs/xfs/scrub/nlinks.c`
- `fs/xfs/scrub/nlinks.h`
- `fs/xfs/scrub/nlinks_repair.c`
- `fs/xfs/scrub/off_bitmap.h`
- `fs/xfs/scrub/orphanage.c`
- `fs/xfs/scrub/orphanage.h`
- `fs/xfs/scrub/parent.c`
- `fs/xfs/scrub/parent_repair.c`
- `fs/xfs/scrub/quota.c`
- `fs/xfs/scrub/quota.h`
- `fs/xfs/scrub/quota_repair.c`
- `fs/xfs/scrub/quotacheck.c`
- `fs/xfs/scrub/quotacheck.h`
- `fs/xfs/scrub/quotacheck_repair.c`
- `fs/xfs/scrub/rcbag.c`
- `fs/xfs/scrub/rcbag.h`
- `fs/xfs/scrub/rcbag_btree.c`
- `fs/xfs/scrub/rcbag_btree.h`
- `fs/xfs/scrub/readdir.c`
- `fs/xfs/scrub/readdir.h`
- `fs/xfs/scrub/reap.c`
- `fs/xfs/scrub/reap.h`
- `fs/xfs/scrub/refcount.c`
- `fs/xfs/scrub/refcount.h`
- `fs/xfs/scrub/refcount_repair.c`
- `fs/xfs/scrub/repair.c`
- `fs/xfs/scrub/repair.h`
- `fs/xfs/scrub/rgb_bitmap.h`
- `fs/xfs/scrub/rgsuper.c`
- `fs/xfs/scrub/rmap.c`
- `fs/xfs/scrub/rmap_repair.c`
- `fs/xfs/scrub/rtb_bitmap.h`
- `fs/xfs/scrub/rtbitmap.c`
- `fs/xfs/scrub/rtbitmap.h`
- `fs/xfs/scrub/rtbitmap_repair.c`
- `fs/xfs/scrub/rtrefcount.c`
- `fs/xfs/scrub/rtrefcount_repair.c`
- `fs/xfs/scrub/rtrmap.c`
- `fs/xfs/scrub/rtrmap_repair.c`
- `fs/xfs/scrub/rtsummary.c`
- `fs/xfs/scrub/rtsummary.h`
- `fs/xfs/scrub/rtsummary_repair.c`
- `fs/xfs/scrub/scrub.c`
- `fs/xfs/scrub/scrub.h`
- `fs/xfs/scrub/stats.c`
- `fs/xfs/scrub/stats.h`
- `fs/xfs/scrub/symlink.c`
- `fs/xfs/scrub/symlink_repair.c`
- `fs/xfs/scrub/tempexch.h`
- `fs/xfs/scrub/tempfile.c`
- `fs/xfs/scrub/tempfile.h`
- `fs/xfs/scrub/trace.c`
- `fs/xfs/scrub/trace.h`
- `fs/xfs/scrub/xfarray.c`
- `fs/xfs/scrub/xfarray.h`
- `fs/xfs/scrub/xfblob.c`
- `fs/xfs/scrub/xfblob.h`
- `fs/xfs/scrub/xfile.c`
- `fs/xfs/scrub/xfile.h`
- `fs/xfs/scrub/xfs_scrub.h`
- `fs/xfs/xfs_attr_item.c`
- `fs/xfs/xfs_buf_mem.c`
- `fs/xfs/xfs_buf_mem.h`
- `fs/xfs/xfs_dahash_test.c`
- `fs/xfs/xfs_dahash_test.h`
- `fs/xfs/xfs_drain.c`
- `fs/xfs/xfs_drain.h`
- `fs/xfs/xfs_exchmaps_item.c`
- `fs/xfs/xfs_exchrange.c`
- `fs/xfs/xfs_exchrange.h`
- `fs/xfs/xfs_healthmon.c`
- `fs/xfs/xfs_healthmon.h`
- `fs/xfs/xfs_hooks.c`
- `fs/xfs/xfs_hooks.h`
- `fs/xfs/xfs_iwalk.c`
- `fs/xfs/xfs_iwalk.h`
- `fs/xfs/xfs_notify_failure.h`
- `fs/xfs/xfs_pwork.c`
- `fs/xfs/xfs_pwork.h`
- `fs/xfs/xfs_verify_media.c`
- `fs/xfs/xfs_verify_media.h`

## 2. WinFsp FLOSS 예외 조항 판정

출처: `https://raw.githubusercontent.com/winfsp/winfsp/master/License.txt`, `master` 커밋 `4425d4807aea4626affb151e38c1cc774ccd5e75` 기준, 2026-10-28 내려받음.

### 원문 (License.txt 1-29행, 그대로 인용)

```
The WinFsp project is Copyright (C) Bill Zissimopoulos. It is licensed
under the terms of the GPLv3.

As a special exception to GPLv3, Bill Zissimopoulos grants additional
permissions to Free/Libre and Open Source Software ("FLOSS") without requiring
that such software is covered by the GPLv3.

    1. Permission to link with a platform specific version of the WinFsp DLL
    (one of: winfsp-a64.dll, winfsp-x64.dll, winfsp-x86.dll, winfsp-msil.dll).

    2. Permission to distribute unmodified binary releases of the WinFsp
    installer (as released by the WinFsp project).

    These permissions (and no other) are granted provided that the software:

    1. Is distributed under a license that satisfies the Free Software
    Definition Version 1.141 (https://www.gnu.org/philosophy/free-sw.en.html)
    or the Open Source Definition Version 1.9 (https://opensource.org/osd).

    2. Includes the copyright notice "WinFsp - Windows File System Proxy,
    Copyright (C) Bill Zissimopoulos" and a link to the WinFsp repository in
    its user-interface and any user-facing documentation.

    3. Is not linked or distributed with proprietary (non-FLOSS) software.
    [You cannot mix FLOSS and proprietary software while using WinFsp under
    this special exception.]

Commercial licensing options are also available: Please contact
Bill Zissimopoulos <billziss at navimatics.com>.
```

### (a) 예외 조항의 허용 라이선스에 GPL-2.0 단독이 포함되는가

**답: 예. 확신도 높음.**

- 예외 조항에는 라이선스 *목록*이 없다. 대신 조건이 있다: "Is distributed under a license that satisfies the Free Software Definition Version 1.141 ... or the Open Source Definition Version 1.9".
- GPL-2.0-only는 FSF가 자유 소프트웨어 라이선스로 인정하고 OSI가 승인한 라이선스다. 두 정의를 모두 만족하므로 이 조건을 충족한다. "GPLv2"라는 단어가 원문에 직접 나오지는 않지만, 조건이 정의 기반이라 해석의 여지가 거의 없다.
- 같은 예외를 쓰려면 함께 지켜야 할 의무가 있다(아래 "결정" 참고): 저작권 표시와 저장소 링크(조건 2), 독점 소프트웨어와 링크/동봉 금지(조건 3).

조건 3에 관한 작은 모호함: 모든 Windows 프로그램은 `kernel32.dll` 같은 독점 OS 구성요소에 링크된다. 원문은 OS 구성요소를 따로 제외하지 않는다. 예외의 목적(Windows에서 FLOSS 파일시스템을 허용)을 생각하면 OS 구성요소는 조건 3의 대상이 아니라고 읽는 것이 합리적이지만, 원문에 명시되어 있지는 않다. 위험은 낮다고 본다. 우리 쪽에서 지킬 것: 독점 라이브러리(OS 구성요소 제외)에 링크하지 않고, 독점 소프트웨어와 함께 배포하지 않는다.

### (b) `LoadLibrary`로 동적 로드하고 API를 직접 선언하면 결합 저작물 문제를 피하는가

**답: 그 자체로는 피하지 못한다. 다만 WinFsp 쪽 문제는 예외 조항이 이미 해결한다. 남는 문제는 GPL-2.0-only(XFS) 쪽이며, 이것은 모호하다. 확신도 중간.**

근거를 두 방향으로 나눈다.

1. **WinFsp(GPLv3) 쪽 저작권자의 요구.** 예외 조항 1은 "Permission to link with a platform specific version of the WinFsp DLL"이다. import 라이브러리로 링크하든 `LoadLibrary`로 런타임에 로드하든 모두 DLL과의 링크로 보는 것이 자연스럽고, FSF의 일반적인 해석도 런타임 로드가 같은 주소 공간에서 데이터 구조를 주고받으면 결합 저작물이 된다는 쪽이다. 즉 `LoadLibrary`는 결합 여부를 바꾸지 않는다. 하지만 FLOSS 조건을 지키면 예외 조항이 이 링크를 허락하므로 GPLv3가 우리 코드에 요구되지 않는다. 이 방향은 해결된다.

2. **XFS(GPL-2.0-only) 쪽 저작권자의 요구.** GPLv2 2절은 배포하는 "work based on the Program" 전체가 GPLv2 조건을 따를 것을 요구한다. WinFsp DLL은 GPLv2로 쓸 수 없다(GPLv3 + 예외). GPLv2의 시스템 라이브러리 예외("major components ... of the operating system")는 WinFsp처럼 따로 설치하는 서드파티 드라이버/DLL에는 적용되지 않는다고 보는 것이 맞다. WinFsp 예외 조항은 WinFsp 저작권자의 허락일 뿐이므로 커널 XFS 저작권자 쪽 요구를 풀어 주지 못한다. 따라서 GPLv2-only 코드와 WinFsp DLL이 한 프로세스에서 결합된 프로그램은, `LoadLibrary`를 쓰든 안 쓰든, 엄격하게 읽으면 GPLv2와 맞지 않을 수 있다. 이것은 법원에서 다뤄진 적이 없는 회색지대다.

   이 위험을 줄이는 요소:
   - 우리가 배포하는 것은 GPLv2 소스와 그것으로 만든 바이너리뿐이고, WinFsp DLL/설치 프로그램은 함께 배포하지 않으면 결합은 사용자 컴퓨터에서 실행 시점에만 일어난다. GPLv2는 실행과 비배포 결합을 제한하지 않는다("The act of running the Program is not restricted").
   - 직접 선언하면 WinFsp 헤더(`winfsp.h`)의 코드, 특히 헤더 안의 인라인 함수가 우리 소스와 바이너리에 들어오지 않는다. 헤더에서 바인딩을 생성하거나 헤더를 포함하면 GPLv3 코드가 GPL-2.0-only 바이너리에 직접 들어가므로, 이 점에서 직접 선언은 실질적인 의미가 있다. (인터페이스 선언 자체의 저작권 범위는 법적으로 정리되지 않았으므로, 선언은 필요한 이름과 구조체 배치만 최소한으로 직접 쓴다.)

   남는 점: 프로그램이 WinFsp 없이는 동작하지 않도록 설계되어 있다는 사실은 FSF 해석에서 결합 저작물 쪽으로 기우는 요소다. 이 부분은 확신을 가지고 "안전하다"고 말할 수 없다.

### 결정 (판정은 모호하므로 사용자 확인이 필요하다)

브리프 Step 4에 따라, (b)가 모호하므로 WinFsp 어댑터 구현은 사용자가 아래 선택지 중 하나를 고를 때까지 진행하지 않는다. 설계자 권고는 선택지 A다.

- **A. 계획대로 진행하되 위험을 줄인다 (권고).** `winfsp-x64.dll`을 `LoadLibrary`로 로드하고 필요한 API만 직접 선언한다(출처 사실은 3절: 공개 헤더/소스를 읽었으나 복사하지 않았다). WinFsp 헤더, import 라이브러리, 바인딩 생성(bindgen 등)을 쓰지 않는다. WinFsp DLL/설치 프로그램은 동봉하지 않고 사용자가 공식 설치 프로그램으로 따로 설치하게 한다. 예외 조항 조건 2에 따라 CLI(`--version`/`--help`)와 README 등 사용자용 문서에 "WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos"와 `https://github.com/winfsp/winfsp` 링크를 넣는다. 남는 위험: 위 (b)-2의 회색지대.
- **B. 프로세스 분리.** XFS(GPLv2-only) 백엔드를 WinFsp를 로드하는 프로세스와 다른 프로세스에서 돌리고 IPC로 연결한다. 결합 저작물 논점을 가장 확실히 줄이지만, 설계 문서에서 성능 목표와 충돌해 이미 기각한 방식이다.
- **C. 커널 코드를 쓰지 않는 XFS 구현.** 커널 소스를 복사/포팅하지 않고 온디스크 포맷 문서만으로 구현한다. 그러면 프로젝트가 GPL-2.0-only에 묶이지 않아 GPLv3(+WinFsp 예외)와의 결합 문제가 사라진다. 대신 구현 비용이 크게 늘고, 커널 코드를 참고하는 방식 자체를 바꿔야 한다.
- **D. 전문가 확인.** 오픈소스 라이선스에 밝은 변호사나 FSF/SFLC 같은 단체에 질의한다. WinFsp 저작자에게 문의하는 것은 GPLv2 쪽 문제를 풀지 못한다(XFS 저작권자가 따로 있다).

## 3. FFI 방식 결정

- `winfsp`/`winfsp-sys` Rust 크레이트(GPL-3.0)는 쓰지 않는다.
- WinFsp는 런타임에 `LoadLibrary`/`GetProcAddress`(또는 `libloading`)로 로드한다. import 라이브러리(`winfsp-x64.lib`)에 링크하지 않는다.
- 필요한 함수 시그니처와 구조체는 이 저장소의 스타일로 직접 손으로 선언한다. `winfsp.h`를 포함하거나 거기서 바인딩을 생성하지 않는다.
- 이 결정은 위 2절의 "결정"이 사용자에게 확인될 때까지 잠정적이다.

### 실제로 무엇을 읽었는가 (출처 사실관계)

이전 문구("공개 문서만 보고 선언")는 사실과 다르므로 정정한다.

- T11 에이전트(`winfsp_ffi.rs`)는 WinFsp **공개 v2.1 헤더**를 스크래치 디렉터리에서 읽고 구조체 배치(VOLUME_PARAMS 504, VOLUME_INFO 88, FILE_INFO 72, DIR_INFO 104, 인터페이스 표 64슬롯), 플래그 비트(예: 사용자 모드 예약 비트, 폐기 표시), `DIR_INFO` 꼬리 공용체, `OPEN_FILE_INFO` 배치를 파악했다.
- T12 에이전트(어댑터)는 WinFsp v2.1 헤더와 함께 **`fsop.c`, `dirctl.c`, `create.c`**를 읽고 동작(`.`/`..`을 WinFsp가 추가하지 않는 점, 대소문자 비구분 볼륨의 정규화된 이름 등)을 파악했다.
- 소스 텍스트는 복사하지 않았다. 검토자가 연속 일치(7단어 이상)를 찾지 못했다. 선언은 이 저장소의 스타일로 직접 쓴 것이다.
- 그러나 레이아웃과 플래그 값은 헤더의 사실과 일치할 수밖에 없고, 구현 동작은 WinFsp 소스를 읽고 알았다. 따라서 "공개 문서만 보고 작성했다"는 주장은 하지 않는다.
- 비공개(비공개 저장소/비공개 문서) WinFsp 소스는 읽지 않았다. 읽은 것은 모두 공개 GitHub 저장소의 파일이다.

### 선택지별 함의 (위 사실을 반영)

| 선택지 | 이 출처 사실에서 따라오는 것 |
|---|---|
| A. 런타임 로드, 독자 선언 (권고) | 선택지 A의 원문은 "공개 문서 기준" 선언을 전제했다. 실제로는 공개 헤더와 `.c` 파일을 읽었으므로, A를 택해도 "문서만 보고 쓴" 클린룸이라고 주장할 수 없다. 주장 가능한 것: 헤더/소스 텍스트를 복사하지 않았고 바인딩을 생성하지 않았으며 직접 선언했다. 위험은 2절 (b)-2의 회색지대에 더해, 배치 사실이 헤더와 일치하는 점이다(일반적으로 인터페이스 사실은 저작권 보호가 약하다고 보지만 법적으로 확정되지 않았다). |
| B. 프로세스 분리 | 출처 사실과 무관하게 결합 논점을 가장 확실히 줄인다. 선언이 헤더에서 유래했는지 여부의 영향도 줄어든다(WinFsp 쪽 프로세스만 해당). |
| C. 커널 코드를 쓰지 않는 XFS 구현 | XFS 쪽 문제만 해결한다. WinFsp 쪽의 출처 사실(헤더/소스를 읽음)은 그대로이므로 이 문제는 남는다. |
| D. 전문가 확인 | 위 사실(읽은 파일 목록)을 그대로 전달해 질의한다. 어느 선택지든 최종 결정 전에 권장한다. |

결정(A-D)은 **여전히 사용자에게 열려 있다.** 개발은 A를 잠정 전제로 진행하며 아무것도 배포하지 않는다.

## 4. Rust 의존성 라이선스

`cargo deny check licenses`가 CI 관문이다(`deny.toml`, `tools/check.sh`에서 실행). 허용: MIT, Apache-2.0, BSD-2-Clause, BSD-3-Clause, ISC, Unicode-3.0, Zlib, GPL-2.0-only. 허용 목록에 없는 라이선스는 모두 거부되며, 여기에는 GPL-3.0\*, AGPL\*, LGPL-3.0\*이 포함된다. (GPL-3.0-only 크레이트가 실제로 거부되는지 임시 크레이트로 확인했다.)

사용 예정 크레이트의 라이선스. crates.io API의 최신 버전 메타데이터로 2026-10-28에 확인했다. 전이 의존성은 확인하지 않았으며 `cargo deny`가 추가 시점에 검사한다.

| 크레이트 | 확인한 버전 | 라이선스 |
|---|---|---|
| `bindgen` | 0.73.2 | BSD-3-Clause (빌드 도구. 단, WinFsp 헤더에는 쓰지 않는다) |
| `libloading` | 0.9.0 | ISC |
| `clap` | 4.6.7 | MIT OR Apache-2.0 |
| `windows-sys` | 0.61.2 | MIT OR Apache-2.0 |
| `proptest` | 1.12.0 | MIT OR Apache-2.0 |

## 5. 반입한 커널 소스 (`third_party/linux-xfs/`)

### 범위

- 커밋 `d135ab453a7400a34930bdda1ef4bdc8acfac80e`의 `fs/xfs` 중 160개 파일을 **바이트 단위로 그대로** 복사했다(`tools/vendor-xfs.sh --src <fs/xfs>`). M2(읽기 경로) 123개: `libxfs/*.c` 29개(M2 설계 5.1), `libxfs/*.h` 전부 65개, 최상위 편집 대상 6개(M2 설계 5.3), 최상위 헤더 23개. M3(쓰기·로그, 2026-10-31 추가) 35개: M3 설계 3절의 `libxfs/*.c` 11개, 최상위 `.c` 21개와 그 `.c`들이 `#include`하는 최상위 헤더 3개, 그리고 M3a T4(2026-11-01)에서 `xfs_fsops.c` 1개, parent pointer 쓰기(2026-11-01)에서 `libxfs/xfs_parent.c` 1개. 최상위 헤더는 참고용이고 실제 컴파일은 `backends/xfs/shim/`의 헤더를 쓴다.
- 파일별 SHA-256과 커밋 SHA는 `third_party/linux-xfs/SOURCE`에 있다. `tools/vendor-xfs.sh --verify`(`tools/check.sh`가 실행)가 어긋남, 누락, 추가 파일을 모두 실패로 처리한다.
- 수정이 필요하면 `third_party/linux-xfs-patches/`의 패치로만 한다. 반입 트리는 수정하지 않는다.
- `third_party/`는 Rust 크레이트가 아니다(워크스페이스 멤버 아님). `cargo deny`/`fmt`/`clippy`에 영향이 없다.

### 커널 `lib/` (`third_party/linux-lib/`)

- 같은 커밋의 `lib/rbtree.c`, `include/linux/rbtree.h`, `rbtree_types.h`, `rbtree_augmented.h` 4개(전부 `GPL-2.0-or-later`). 해시는 `third_party/linux-lib/SOURCE.sha256`(`tools/check.sh`가 `sha256sum -c`로 검사), SPDX는 `license-scan.py --check-allowed third_party/linux-lib`로 검사한다.
- 수정은 `third_party/linux-lib-patches/`의 패치로만 한다(현재 `0001-rbtree-uintptr`: LLP64에서 포인터를 담는 `unsigned long`을 `uintptr_t`로).

### 게이트

`python3 -I tools/license-scan.py --check-allowed third_party/linux-xfs`(`tools/check.sh`가 실행)와 `tools/test_license_scan.py`의 `test_vendored_xfs_files_keep_spdx`가 모든 `.c`/`.h`의 SPDX를 허용 집합과 대조한다. 허용 집합은 반입 파일에 실제로 있는 것만이다.

| SPDX (정규화 후) | 파일 수 | 원문 표기 |
|---|---:|---|
| `GPL-2.0-only` | 122 | `GPL-2.0` |
| `GPL-2.0-or-later` | 17 | `GPL-2.0-or-later` |
| `GPL-2.0+` | 18 | `GPL-2.0+` (`GPL-2.0-or-later`의 폐기된 표기) |
| `LGPL-2.1` | 1 | `libxfs/xfs_fs.h` |
| 표기 없음 / 그 외 | 0 | |
| **합계** | **123** | |

`GPL-3.0*`, `AGPL*`, `LGPL-3.0*`, 알 수 없는 ID, SPDX가 없는 파일은 스캔이 실패한다(테스트 `test_check_allowed_rejects_gpl3_unknown_and_missing`).

### 호환성 근거

- 프로젝트는 GPL-2.0-only다. `GPL-2.0-only` 파일은 그대로 같은 조건이다.
- `GPL-2.0-or-later`(및 `GPL-2.0+`) 파일은 "GPLv2 또는 이후 버전" 중 받는 사람이 고를 수 있으므로 GPLv2를 고르면 된다. 이들을 GPL-2.0-only 코드와 하나의 결합 저작물로 배포하면 전체가 GPLv2 조건을 따르며 어떤 파일의 조건도 어기지 않는다. 이 결합물의 일부를 GPLv3로 쓰는 것은 GPL-2.0-only 파일 때문에 불가능하다.
- `LGPL-2.1`(`xfs_fs.h`, 사용자 공간 ioctl 헤더) 파일: LGPL-2.1 3절은 해당 코드에 LGPL 대신 일반 GPL 조건(GPLv2 또는 그 이후)을 적용하도록 허용한다. GPLv2를 골라 GPL-2.0-only 결합물에 넣을 수 있다.
- GPL-3.0 전용 코드는 반입하지 않는다. 위 게이트가 이를 강제한다.
- 반입 파일의 SPDX 줄과 저작권 표시는 바이트 단위로 보존된다(`--verify`가 보장). 이 프로젝트의 새 파일은 `GPL-2.0-only`다.
