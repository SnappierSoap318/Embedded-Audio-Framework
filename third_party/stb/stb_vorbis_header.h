/* EAF shim: expose stb_vorbis declarations without duplicating the
 * implementation, which is compiled once in third_party/impl/stb_vorbis_impl.c.
 * Included through a system include path so vendor warnings stay suppressed. */
#ifndef EAF_STB_VORBIS_HEADER_H
#define EAF_STB_VORBIS_HEADER_H
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"
#endif
