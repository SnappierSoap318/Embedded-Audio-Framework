# Optional host build of exactly the decoder used by the Zephyr manifest.
# External source remains at the caller-supplied path, with its original license.
set(EAF_LIBSBC_ROOT "" CACHE PATH "Zephyr libsbc checkout (8e1beda02acb8972e29e6edbb423f7cafe16e445)")
if(EAF_LIBSBC_ROOT)
  set(sbc_files alloc bitalloc bitalloc-sbc bitstream-decode decoder-oina
    decoder-private decoder-sbc dequant framing framing-sbc oi_codec_version
    synthesis-8-generated synthesis-dct8 synthesis-sbc)
  add_library(eaf_sbc_vendor STATIC)
  foreach(file IN LISTS sbc_files)
    target_sources(eaf_sbc_vendor PRIVATE ${EAF_LIBSBC_ROOT}/decoder/srce/${file}.c)
  endforeach()
  include(${CMAKE_CURRENT_LIST_DIR}/sbc_fixups.cmake)
  eaf_sbc_fixups(eaf_sbc_vendor ${EAF_LIBSBC_ROOT})
  # Vendor diagnostics are not governed by EAF's warning policy.
  target_compile_options(eaf_sbc_vendor PRIVATE -w)
  target_compile_definitions(eaf_sbc_vendor PRIVATE SBC_FOR_EMBEDDED_LINUX SBC_NO_PCM_CPY_OPTION)
  target_include_directories(eaf_sbc_vendor SYSTEM PUBLIC
    ${EAF_LIBSBC_ROOT}/decoder/include ${EAF_LIBSBC_ROOT}/decoder/srce)
  target_sources(eaf PRIVATE apps/decoders/sbc_oi.c)
  target_link_libraries(eaf PUBLIC eaf_sbc_vendor)
endif()
