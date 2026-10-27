# Toolchain for everything linked into vesktop_media.node.
#
# The prebuilt libwebrtc (third_party/webrtc, shiguredo webrtc-build) is compiled with clang against
# Chromium's own libc++ (namespace std::__Cr) and bundles that libc++, libc++abi, BoringSSL and opus.
# All our C++ code, mlspp and libdave must use the same libc++ headers and configuration, or the
# std:: types crossing into libwebrtc would not match.

get_filename_component(VV_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(VV_THIRD_PARTY "${VV_ROOT}/third_party")
set(VV_WEBRTC_DIR "${VV_THIRD_PARTY}/webrtc" CACHE PATH "prebuilt libwebrtc (include/, lib/libwebrtc.a)")

if(CMAKE_HOST_WIN32)
    # Windows: clang-cl and lld-link (LLVM for Windows), the static CRT (/MT) libwebrtc is built with. libc++ is
    # included with -I ahead of the MSVC headers and without libc++abi: on Windows libc++ sits on the MSVC runtime
    # (vcruntime) and the Microsoft C++ ABI, as in Chromium (build/config/c++/BUILD.gn)
    set(CMAKE_C_COMPILER clang-cl)
    set(CMAKE_CXX_COMPILER clang-cl)
    set(CMAKE_LINKER lld-link)
    set(CMAKE_AR llvm-lib)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
    # also in projects whose cmake_minimum_required predates the setting (mlspp), which would build with /MD
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(VV_LIBCXX_FLAGS
        "-I${VV_THIRD_PARTY}/libcxx-config/include \
-I${VV_THIRD_PARTY}/libcxx/include \
-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS \
-D_LIBCPP_INSTRUMENTED_WITH_ASAN=0 \
-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE \
-D_HAS_EXCEPTIONS=0")
    # source paths relative to the project in __FILE__, as on Linux
    set(VV_PREFIX_MAP "/clang:-ffile-prefix-map=${VV_ROOT}/=")
    set(CMAKE_CXX_FLAGS_INIT "${VV_LIBCXX_FLAGS} ${VV_PREFIX_MAP} /utf-8 /Zc:__cplusplus -Wno-nullability-completeness -Wno-deprecated-declarations -Wno-error")
    set(CMAKE_C_FLAGS_INIT "${VV_PREFIX_MAP} /utf-8")
else()
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
endif()

# BoringSSL from libwebrtc stands in for OpenSSL (mlspp / libdave support it natively)
set(OPENSSL_ROOT_DIR "${VV_WEBRTC_DIR}")
set(OPENSSL_INCLUDE_DIR "${VV_WEBRTC_DIR}/include/third_party/boringssl/src/include")
if(CMAKE_HOST_WIN32)
    # FindOpenSSL with MSVC-style compilers reads LIB_EAY / SSL_EAY and derives OPENSSL_*_LIBRARY from them
    foreach(v LIB_EAY_RELEASE LIB_EAY_DEBUG SSL_EAY_RELEASE SSL_EAY_DEBUG
              OPENSSL_CRYPTO_LIBRARY OPENSSL_SSL_LIBRARY)
        set(${v} "${VV_WEBRTC_DIR}/lib/webrtc.lib")
    endforeach()
else()
    set(OPENSSL_CRYPTO_LIBRARY "${VV_WEBRTC_DIR}/lib/libwebrtc.a")
    set(OPENSSL_SSL_LIBRARY "${VV_WEBRTC_DIR}/lib/libwebrtc.a")
endif()
