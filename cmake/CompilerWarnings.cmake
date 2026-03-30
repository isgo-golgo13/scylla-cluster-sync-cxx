# cmake/CompilerWarnings.cmake
# Apply opinionated C++20 warning set to a target.
# Usage: target_apply_warnings(my_target)

function(target_apply_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:
            -Wall -Wextra -Wpedantic
            -Wconversion -Wsign-conversion
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
            -Wno-unused-parameter
        >
        $<$<CXX_COMPILER_ID:Clang,AppleClang>:
            -Wall -Wextra -Wpedantic
            -Wconversion -Wsign-conversion
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
            -Wno-unused-parameter
            -Wno-c++98-compat
        >
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4 /WX /permissive-
        >
    )
endfunction()
