# Third-party code that is not recovered from rfg.exe.

# zlib 1.2.11 (unmodified sources, see zlib/README). rfg.exe links zlib 1.2.3; the deflate stream format and the
# z_stream layout are the same, and vlib only uses inflate/deflate through zlib_stream.
add_library(openrfg_zlib STATIC
    zlib/adler32.c
    zlib/compress.c
    zlib/crc32.c
    zlib/deflate.c
    zlib/infback.c
    zlib/inffast.c
    zlib/inflate.c
    zlib/inftrees.c
    zlib/trees.c
    zlib/uncompr.c
    zlib/zutil.c
)
target_include_directories(openrfg_zlib PUBLIC zlib)
if(MSVC)
    # Drop the project's /W4 first: cl warns (D9025) when /w overrides it.
    get_target_property(openrfg_zlib_options openrfg_zlib COMPILE_OPTIONS)
    list(REMOVE_ITEM openrfg_zlib_options /W4)
    set_target_properties(openrfg_zlib PROPERTIES COMPILE_OPTIONS "${openrfg_zlib_options}")
    target_compile_options(openrfg_zlib PRIVATE /w)
endif()

# libdeflate 1.23 (unmodified sources, see libdeflate/README.openrfg). Decompression only. Used for the fast
# whole-buffer inflate of streamed containers (keen_cf_read_compressed_chain); zlib stays as the fallback.
add_library(openrfg_libdeflate STATIC
    libdeflate/lib/adler32.c
    libdeflate/lib/deflate_decompress.c
    libdeflate/lib/zlib_decompress.c
    libdeflate/lib/utils.c
    libdeflate/lib/x86/cpu_features.c
)
target_include_directories(openrfg_libdeflate PUBLIC libdeflate)
if(MSVC)
    get_target_property(openrfg_ld_options openrfg_libdeflate COMPILE_OPTIONS)
    if(openrfg_ld_options)
        list(REMOVE_ITEM openrfg_ld_options /W4)
        set_target_properties(openrfg_libdeflate PROPERTIES COMPILE_OPTIONS "${openrfg_ld_options}")
    endif()
    target_compile_options(openrfg_libdeflate PRIVATE /w)
endif()
