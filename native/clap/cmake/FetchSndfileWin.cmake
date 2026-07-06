# libsndfile prebuilt for Windows — used ONLY by axon_bench (the plugins never
# link sndfile). On macOS/Linux the library comes from homebrew/apt via
# pkg-config; Windows has no such convention, so we pin the official win64
# release zip from GitHub, mirroring the FetchOnnxRuntime pattern.
#
# The 1.2.2 win64 zip layout (CMake-built upstream package):
#   include/sndfile.h
#   lib/sndfile.lib        (import lib; records DLL name "sndfile.dll")
#   bin/sndfile.dll        (copied next to axon_bench post-build)
#
# Updating: bump SNDFILE_WIN_VERSION, sha256sum -b the new zip, paste below.
# NEVER leave the hash empty (same integrity rule as FetchOnnxRuntime.cmake).
include(FetchContent)

set(SNDFILE_WIN_VERSION "1.2.2" CACHE STRING "libsndfile release tag (Windows prebuilt)")
set(SNDFILE_WIN_SHA256  "2173935c0c1ed13cf627951d34483f9d405ead2eb473190461c42ba220643a3f")

FetchContent_Declare(sndfile_win
    URL "https://github.com/libsndfile/libsndfile/releases/download/${SNDFILE_WIN_VERSION}/libsndfile-${SNDFILE_WIN_VERSION}-win64.zip"
    URL_HASH SHA256=${SNDFILE_WIN_SHA256}
)

FetchContent_MakeAvailable(sndfile_win)

set(SNDFILE_INCLUDE_DIRS "${sndfile_win_SOURCE_DIR}/include")
set(SNDFILE_LIBRARY_DIRS "${sndfile_win_SOURCE_DIR}/lib")
set(SNDFILE_LIBRARIES    sndfile)
set(SNDFILE_WIN_DLL      "${sndfile_win_SOURCE_DIR}/bin/sndfile.dll")
