# Included into mlspp and libdave on Windows (CMAKE_PROJECT_INCLUDE, after their project()). For Clang they add
# GCC's warning flags with -Werror; clang-cl reads -Wall as -Weverything and does not know -pedantic, so every
# file would fail. Their warning flags are dropped, everything else they add stays
function(add_compile_options)
    set(keep)
    foreach(option IN LISTS ARGN)
        if(NOT option MATCHES "^(-Wall|-Wextra|-pedantic|-Werror|-Wmissing-declarations|-Wimplicit-int-conversion|-Wno-dangling-reference|/W4|/WX)$")
            list(APPEND keep "${option}")
        endif()
    endforeach()
    _add_compile_options(${keep})
endfunction()
