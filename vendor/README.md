## Client library

`vveye-p2p` uses VVEye's proprietary `libt2u.so`, which is fetched at build
time rather than committed to this repository:

- **Product:** VVEye / Libt2u Android client SDK
- **SDK version:** v106
- **File:** `libt2u.so`
- **Architecture:** x86_64
- **Source archive:**
  <http://www.vveye.com/download/t2u/libt2u_android_v106.7z>
- **Source archive SHA-256:**
  `e81df734b08b5c9b5dc55cffbfc01b1e1634ef35067dde47bab66c983c9ef279`
- **Extracted library SHA-256:**
  `fd5e265ba44bb141efa8c3f6c2ee6f3ad7bb1f5346882dc9c0c573bdd942732f`

`vendor/fetch-libt2u.sh` downloads the archive from the URL above, verifies
its SHA-256 values, and installs both the x86_64 library and the SDK header.

The library's Android package declares `liblog.so`, `libc.so`, `libm.so`,
`libdl.so`, and `libstdc++.so` dependencies.  The Makefile aliases those names
to the system runtime ELF objects under `build/`.

## Licensing

VVEye publishes no license, EULA, or redistribution terms with this SDK — the
SDK archives and <http://www.vveye.com/> contain none.  The only rights
statement is the copyright notice in the SDK's `libt2u.h`:

    Copyright (C) 2014 by Yexr <yexr@vveye.com>

Absent an explicit license, all rights are reserved, so `libt2u.so` must not
be redistributed.  Redistribution requires written permission from VVEye
(Hangzhou Vveye Network Technology Co., Ltd.
