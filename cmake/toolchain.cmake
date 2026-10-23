# Toolchain for everything linked into vesktop_media.node.
#
# The prebuilt libwebrtc (third_party/webrtc, shiguredo webrtc-build) is compiled with clang against
# Chromium's own libc++ (namespace std::__Cr) and bundles that libc++, libc++abi, BoringSSL and opus.
# All our C++ code, mlspp and libdave must use the same libc++ headers and configuration, or the
# std:: types crossing into libwebrtc would not match.

get_filename_component(VV_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(VV_THIRD_PARTY "${VV_ROOT}/third_party")
set(VV_WEBRTC_DIR "${VV_THIRD_PARTY}/webrtc" CACHE PATH "prebuilt libwebrtc (include/, lib/libwebrtc.a)")

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

# Chromium's libc++ configuration (buildtools __config_site + build/config/c++ defines)
set(VV_LIBCXX_FLAGS
    "-nostdinc++ \
-isystem ${VV_THIRD_PARTY}/libcxx-config/include \
-isystem ${VV_THIRD_PARTY}/libcxx/include \
-isystem ${VV_THIRD_PARTY}/libcxxabi/include \
-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS \
-D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS \
-D_LIBCPP_INSTRUMENTED_WITH_ASAN=0 \
-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE")

# Source paths in the binaries (log messages use __FILE__) relative to the project, not where it was built
set(VV_PREFIX_MAP "-ffile-prefix-map=${VV_ROOT}/=")
set(CMAKE_CXX_FLAGS_INIT "${VV_LIBCXX_FLAGS} ${VV_PREFIX_MAP} -fPIC -fvisibility=hidden -Wno-nullability-completeness -Wno-deprecated-declarations -Wno-error")
set(CMAKE_C_FLAGS_INIT "${VV_PREFIX_MAP} -fPIC -fvisibility=hidden")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-nostdlib++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-nostdlib++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib++")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# BoringSSL from libwebrtc stands in for OpenSSL (mlspp / libdave support it natively)
set(OPENSSL_ROOT_DIR "${VV_WEBRTC_DIR}")
set(OPENSSL_INCLUDE_DIR "${VV_WEBRTC_DIR}/include/third_party/boringssl/src/include")
set(OPENSSL_CRYPTO_LIBRARY "${VV_WEBRTC_DIR}/lib/libwebrtc.a")
set(OPENSSL_SSL_LIBRARY "${VV_WEBRTC_DIR}/lib/libwebrtc.a")
