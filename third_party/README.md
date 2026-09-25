# Vendored third-party sources

These files are vendored (committed) rather than fetched at build time so that
codec builds are reproducible without network access. Each entry records the
exact upstream revision. Only the files listed here are vendored; the rest of
each upstream project is not.

Keep this file and `docs/licensing.md` in sync when a vendored file is updated.

## dr_libs

- Upstream: https://github.com/mackron/dr_libs
- Revision: `dfe8377631000664666519fdb83da193fd8037f4`
- Files: `dr_libs/dr_flac.h` (v0.13.4), `dr_libs/dr_mp3.h` (v0.7.4),
  `dr_libs/dr_wav.h` (v0.14.6), `dr_libs/LICENSE`
- License: public domain (Unlicense) or MIT-0, at your option (see
  `dr_libs/LICENSE`; the choice is also embedded at the top of each header)
- Implementation translation units: `impl/dr_flac_impl.c` and
  `impl/dr_mp3_impl.c` (compiled with warnings suppressed, outside EAF's
  lint/format scope)

## stb

- Upstream: https://github.com/nothings/stb
- Revision: `2c980bb59875b0d32144a71867fbdebb2f77cd20`
- Files: `stb/stb_vorbis.c` (v1.22), plus the EAF-authored shim
  `stb/stb_vorbis_header.h` (declarations only) and the implementation
  translation unit `impl/stb_vorbis_impl.c`
- License: public domain (Unlicense) or MIT, at your option (embedded at the top
  of the file)

## OI/libsbc

- Upstream: https://github.com/zephyrproject-rtos/libsbc
- Revision: `8e1beda02acb8972e29e6edbb423f7cafe16e445`
- Files: `libsbc/decoder/include/*.h`, `libsbc/decoder/srce/*.c`,
  `libsbc/decoder/srce/readsamplesjoint.inc`, `libsbc/LICENSE`,
  `libsbc/README.upstream.md` (renamed from upstream `README.md`)
- License: Apache-2.0 (Android `system/embdrv/sbc`, see `libsbc/LICENSE`)
- Only the decoder is vendored; the upstream encoder and Zephyr module wrapper
  are not. The adapter `apps/decoders/sbc_oi.c` is EAF-authored and calls the
  decoder through `include/eaf/eaf_sbc_oi.h`.
- `cmake/sbc_fixups.cmake` generates build-local copies of three synthesis files
  with the negative signed shifts rewritten as unsigned operations, so the
  vendored sources stay byte-for-byte upstream.

## SHA-256

```
111144e778f55738db6851cb226015c419e00d04b916a09506d4856d9cff945c  dr_libs/dr_flac.h
997b7ee18de6e6b81e2a83f1ea9fc62aef25c62b28d48db95635f49e65de0a2f  dr_libs/dr_mp3.h
03e70c1a2d9787cd7ed3e966c075bea7bac6373f759db9cd7ca9ccdfc4ec4493  dr_libs/dr_wav.h
4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636  stb/stb_vorbis.c
37e3e1e920dc49461774d940275f806d95e4b0e6d6e9b948cfc527133f14c41c  libsbc/decoder/include/oi_assert.h
4791457695031c423cb0efb816c8f11ce3ce6415f2b383eb77f9abae968da7a2  libsbc/decoder/include/oi_bitstream.h
66ed61ed85aee8eb3f64bef738e6f77813ec6cb9cec997173ed57b7cfa045ac0  libsbc/decoder/include/oi_bt_spec.h
5df696eca911afd3a094e99831f2b235f364b5c19157e7c65df1243373f5358f  libsbc/decoder/include/oi_codec_sbc.h
a8ba08775b5b31692fd96f906f9714b6aa569b71e16dee84a7fbcfd4c21491df  libsbc/decoder/include/oi_codec_sbc_private.h
34e69e3dcdbe106de0c5ff96c02b448f231659fb81e77968b260e7b3fc22a126  libsbc/decoder/include/oi_common.h
51f0999baa4561a894ce6f8b85074b88b9286946d1e509344c201230ef0686a0  libsbc/decoder/include/oi_cpu_dep.h
994bb8580a306e3c4e5f958ceed8e19f6ad623957f6bdd6d6eff23188a398d18  libsbc/decoder/include/oi_modules.h
5c3d463ca1943ab801cb3617e8366559adda7ec2219e93680e9b5b81d5b41d87  libsbc/decoder/include/oi_osinterface.h
5121db37b90e42a7ab4a092b0dbdac894019a02e01961ca4df515b0cd1542075  libsbc/decoder/include/oi_status.h
511e00ed895ae5bf87ad2f6bddbf474afc9e36e7bf64b71a75e5d1500d2e5049  libsbc/decoder/include/oi_stddefs.h
c4f0e6192f53f718bdb24bb68977c95097332148ce2eb639c49c13c7a9ea3ef6  libsbc/decoder/include/oi_string.h
34d8191ffb70ba0d1037d2a20ec9090caf699bca069ce1654dbddc6907ad49e9  libsbc/decoder/include/oi_time.h
57d01d2df48d159d25fb04b54e62fc318d4e1ccebca8d0af00dd4d250133af92  libsbc/decoder/include/oi_utils.h
86b2b5e653d8c85cda23a9eeb8b37f2bfcbf01e7784c3ec071f63eeb1a2df6ca  libsbc/decoder/srce/alloc.c
6b918bedb1ad766039fe4c87c8091a3578540481bc3f72c0e8a893430cc9ab4a  libsbc/decoder/srce/bitalloc.c
38ff6d676669f61c283273a8403727b9ecf44411628c41cbbd2fe243881ed52e  libsbc/decoder/srce/bitalloc-sbc.c
3336d994518fe97c2d240612747800b2bf8423fa4d15c483b02ed3129d32090e  libsbc/decoder/srce/bitstream-decode.c
58b8467da96333d79592478fb9e7bef57400c75ba9aead14cff9258079fe4fb5  libsbc/decoder/srce/decoder-oina.c
60d456fddd3839b7aa7f883213205d1be396c94690c4ebeaf8bb7f8784978c8a  libsbc/decoder/srce/decoder-private.c
fd44f3d8ce667df2c7c39aa0fd1c5ca597c5f0ad953d4ccf63ff5a7d6a493aca  libsbc/decoder/srce/decoder-sbc.c
3bdd22415b311138891b6cf651dc952c3c8cdda13dd62580114a5d193e149e47  libsbc/decoder/srce/dequant.c
da2f951e4b0722ebec4a0b948e2942e6fdda9f8dcba0e09907060efea3202384  libsbc/decoder/srce/framing.c
f5efa3367155338e3c8088b23c93b6977a0dc6c51377a9c6cb705c408c839ef5  libsbc/decoder/srce/framing-sbc.c
9ba9653eb145853dc8d937a164a25808478dd59a8b5747ba7cc9cdd7a1cae716  libsbc/decoder/srce/oi_codec_version.c
47ea8500e8191e43e47c00f1a1c2a2febb6839252b27e3f5410aacdcd3a6170a  libsbc/decoder/srce/readsamplesjoint.inc
236d897cbce62cb62c76cfd8fe5b3300b303a49be95fab6eb8b2ba681ac86be8  libsbc/decoder/srce/synthesis-8-generated.c
1aa444502c50a62a66475e860637044043166a8e55108c20d25cc17feb784073  libsbc/decoder/srce/synthesis-dct8.c
82bb804881385d1ca10ac15e5ac76cdaef7efc68495dab430b78745538dadb16  libsbc/decoder/srce/synthesis-sbc.c
```

## Opus

Opus has no single-header decoder, so it is tracked as a git submodule instead
(see `.gitmodules`, `third_party/opus`, pinned to tag `v1.6.1`). Builds that
consume it must initialise submodules (`git submodule update --init`). See
`docs/licensing.md`.
