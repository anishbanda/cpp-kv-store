function(kvstore_set_project_warnings target_name)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Werror
    )
  elseif(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /WX)
  endif()
endfunction()

