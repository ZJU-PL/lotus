# 注意：本文件需在 project() 之后 include。
# 需要 CMake >= 3.18（CheckLinkerFlag）

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LOTUS_SUPPRESS_UNUSED_WARNINGS "Suppress noisy -Wunused-* warnings" ON)

include(CheckCXXCompilerFlag)

# GCC / Clang / AppleClang，排除 clang-cl（MSVC 风格命令行）
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$"
   AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(LOTUS_GNU_LIKE_CXX ON)
else()
  set(LOTUS_GNU_LIKE_CXX OFF)
endif()

# ---------------------------------------------------------------------------
# 辅助函数：仅在编译器支持时才添加 C++ 警告选项
#   lotus_cxx_enable_warnings(reorder ...)   -> -Wreorder
#   lotus_cxx_disable_warnings(unused-xxx)   -> -Wno-unused-xxx
# 对 disable 检测的是正向 -W<name>，因为 GCC 会静默接受任何未知的 -Wno-*，
# 用正向选项检测才准确。
# ---------------------------------------------------------------------------
function(_lotus_cxx_warning_impl prefix)
  foreach(name IN LISTS ARGN)
    string(MAKE_C_IDENTIFIER "LOTUS_CXX_HAS_W_${name}" var)
    check_cxx_compiler_flag("-W${name}" ${var})
    if(${var})
      add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${prefix}${name}>")
    endif()
  endforeach()
endfunction()

function(lotus_cxx_enable_warnings)
  _lotus_cxx_warning_impl("-W" ${ARGN})
endfunction()

function(lotus_cxx_disable_warnings)
  _lotus_cxx_warning_impl("-Wno-" ${ARGN})
endfunction()

# ---------------------------------------------------------------------------
# 异常 / RTTI
# ---------------------------------------------------------------------------
if(LOTUS_GNU_LIKE_CXX)
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fexceptions;-frtti>")
elseif(MSVC)
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/EHsc;/GR>")
endif()

# ---------------------------------------------------------------------------
# 警告
# ---------------------------------------------------------------------------
if(LOTUS_GNU_LIKE_CXX)
  # 额外开启的有用警告（GCC 与 Clang 都支持；Clang 的 -Wreorder 已包含 -Wreorder-ctor）
  lotus_cxx_enable_warnings(
    reorder
    ignored-qualifiers
  )

  # 关闭噪声较大的 unused 类警告
  if(LOTUS_SUPPRESS_UNUSED_WARNINGS)
    lotus_cxx_disable_warnings(
      unused-parameter
      unused-variable
      unused-function
      unused-but-set-variable
      missing-field-initializers
      pessimizing-move
      overloaded-virtual
      unused-private-field     # 仅 Clang，GCC 会被自动跳过
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# macOS: 静态库归档与链接器设置
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_GENERATOR STREQUAL "Xcode")
  # BSD `ar` 用 `q` 追加到已存在的归档时会产生重复成员。
  # 改用 `r`（替换）。ARCHIVE_APPEND 也要一起改：目标文件很多时，
  # CMake 会分批调用它。
  get_property(_lotus_langs GLOBAL PROPERTY ENABLED_LANGUAGES)
  foreach(lang IN ITEMS C CXX OBJC OBJCXX)
    if(lang IN_LIST _lotus_langs)
      set(CMAKE_${lang}_ARCHIVE_CREATE "<CMAKE_AR> cr <TARGET> <LINK_FLAGS> <OBJECTS>")
      set(CMAKE_${lang}_ARCHIVE_APPEND "<CMAKE_AR> r <TARGET> <LINK_FLAGS> <OBJECTS>")
    endif()
  endforeach()
  unset(_lotus_langs)
endif()

if(APPLE)
  # Xcode 15+ 的新链接器 (ld-prime) 会对重复库发出警告
  include(CheckLinkerFlag)
  check_linker_flag(CXX "-Wl,-no_warn_duplicate_libraries" LOTUS_HAS_NO_WARN_DUPLICATE_LIBS)
  if(LOTUS_HAS_NO_WARN_DUPLICATE_LIBS)
    add_link_options("-Wl,-no_warn_duplicate_libraries")
  endif()
endif()