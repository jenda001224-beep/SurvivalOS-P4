# Zstandard, decompression only.
#
# ZIM archives from Kiwix store their article clusters as zstd streams, so the
# Wikipedia reader needs a decompressor. Only the decode half of the library is
# vendored here; the compressor would roughly double the code size for nothing.
#
# Upstream: https://github.com/facebook/zstd (v1.5.6, BSD-3-Clause / GPLv2)

file(GLOB ZSTD_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/common/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/decompress/*.c")

idf_component_register(
    SRCS ${ZSTD_SOURCES}
    INCLUDE_DIRS "lib"
    PRIV_INCLUDE_DIRS "lib/common" "lib/decompress")

# ZSTD_MULTITHREAD stays off, and the assembly fast path is x86 only.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ZSTD_DISABLE_ASM=1
    ZSTD_LEGACY_SUPPORT=0
    ZSTD_LIB_COMPRESSION=0
    ZSTD_LIB_DEPRECATED=0
    ZSTD_NO_TRACE=1
    XXH_NAMESPACE=ZSTD_)

# Upstream builds cleanly but not at this project's warning level.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-unused-function
    -Wno-unused-variable)
