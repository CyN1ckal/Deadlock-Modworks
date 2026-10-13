# libarchive for reading downloaded mod archives (zip, 7z, rar), with zlib for deflate and xz for LZMA/LZMA2.
# libarchive's configure step test-links against zlib and liblzma, so the three are built as external projects
# into one prefix before the main build uses them. They are built once in Release with the static CRT (/MT).
# Debug builds of the app use /MTd, so the core target drops libcmt there; these are C libraries and only need
# the C runtime functions both variants provide.

include(ExternalProject)

set(SR_ARCHIVE_PREFIX "${CMAKE_BINARY_DIR}/archive-deps")
set(SR_ARCHIVE_INSTALL "${SR_ARCHIVE_PREFIX}/install")
file(MAKE_DIRECTORY "${SR_ARCHIVE_INSTALL}/include")

set(SR_ARCHIVE_COMMON_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${SR_ARCHIVE_INSTALL}
    -DCMAKE_PREFIX_PATH=${SR_ARCHIVE_INSTALL}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
    "-DCMAKE_C_FLAGS_RELEASE=/MT /O2 /Ob2 /DNDEBUG"
)
set(SR_ARCHIVE_BUILD ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release)
set(SR_ARCHIVE_INSTALL_STEP ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release)

set(SR_ZLIB_LIBRARY "${SR_ARCHIVE_INSTALL}/lib/zlibstatic.lib")
ExternalProject_Add(sr_zlib_ep
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${SR_ARCHIVE_PREFIX}/zlib"
    CMAKE_ARGS ${SR_ARCHIVE_COMMON_ARGS} -DZLIB_BUILD_EXAMPLES=OFF
    BUILD_COMMAND ${SR_ARCHIVE_BUILD}
    INSTALL_COMMAND ${SR_ARCHIVE_INSTALL_STEP}
    BUILD_BYPRODUCTS "${SR_ZLIB_LIBRARY}"
)

set(SR_LZMA_LIBRARY "${SR_ARCHIVE_INSTALL}/lib/lzma.lib")
ExternalProject_Add(sr_xz_ep
    URL https://github.com/tukaani-project/xz/releases/download/v5.8.1/xz-5.8.1.tar.gz
    URL_HASH SHA256=507825b599356c10dca1cd720c9d0d0c9d5400b9de300af00e4d1ea150795543
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${SR_ARCHIVE_PREFIX}/xz"
    CMAKE_ARGS ${SR_ARCHIVE_COMMON_ARGS}
        -DXZ_NLS=OFF -DXZ_DOC=OFF -DBUILD_TESTING=OFF
        -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF
        -DXZ_TOOL_SCRIPTS=OFF -DXZ_TOOL_SYMLINKS=OFF -DXZ_TOOL_SYMLINKS_LZMA=OFF
    BUILD_COMMAND ${SR_ARCHIVE_BUILD}
    INSTALL_COMMAND ${SR_ARCHIVE_INSTALL_STEP}
    BUILD_BYPRODUCTS "${SR_LZMA_LIBRARY}"
)

set(SR_LIBARCHIVE_LIBRARY "${SR_ARCHIVE_INSTALL}/lib/archive.lib")
ExternalProject_Add(sr_libarchive_ep
    URL https://github.com/libarchive/libarchive/releases/download/v3.8.1/libarchive-3.8.1.tar.gz
    URL_HASH SHA256=bde832a5e3344dc723cfe9cc37f8e54bde04565bfe6f136bc1bd31ab352e9fab
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${SR_ARCHIVE_PREFIX}/libarchive"
    DEPENDS sr_zlib_ep sr_xz_ep
    CMAKE_ARGS ${SR_ARCHIVE_COMMON_ARGS}
        -DZLIB_INCLUDE_DIR=${SR_ARCHIVE_INSTALL}/include
        -DZLIB_LIBRARY=${SR_ZLIB_LIBRARY}
        -DLIBLZMA_INCLUDE_DIR=${SR_ARCHIVE_INSTALL}/include
        -DLIBLZMA_LIBRARY=${SR_LZMA_LIBRARY}
        -DENABLE_ZLIB=ON -DENABLE_LZMA=ON
        -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_CNG=OFF
        -DENABLE_LIBB2=OFF -DENABLE_LZ4=OFF -DENABLE_LZO=OFF -DENABLE_ZSTD=OFF -DENABLE_BZip2=OFF
        -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_WIN32_XMLLITE=OFF
        -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF -DENABLE_LIBGCC=OFF -DENABLE_ICONV=OFF
        -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF
        -DENABLE_XATTR=OFF -DENABLE_ACL=OFF -DENABLE_TEST=OFF -DENABLE_WERROR=OFF -DPOSIX_REGEX_LIB=NONE
    BUILD_COMMAND ${SR_ARCHIVE_BUILD} --target archive_static
    INSTALL_COMMAND ${SR_ARCHIVE_INSTALL_STEP}
    BUILD_BYPRODUCTS "${SR_LIBARCHIVE_LIBRARY}"
)

add_library(sr_archive INTERFACE)
add_dependencies(sr_archive sr_libarchive_ep)
target_include_directories(sr_archive INTERFACE "${SR_ARCHIVE_INSTALL}/include")
target_compile_definitions(sr_archive INTERFACE LIBARCHIVE_STATIC LZMA_API_STATIC)
target_link_libraries(sr_archive INTERFACE "${SR_LIBARCHIVE_LIBRARY}" "${SR_LZMA_LIBRARY}" "${SR_ZLIB_LIBRARY}")
if(MSVC)
    target_link_options(sr_archive INTERFACE $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmt.lib>)
endif()
