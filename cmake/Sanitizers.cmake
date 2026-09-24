function(kvstore_enable_sanitizers target_name)
  if(KVSTORE_ENABLE_TSAN AND (KVSTORE_ENABLE_ASAN OR KVSTORE_ENABLE_UBSAN))
    message(FATAL_ERROR "ThreadSanitizer must run separately from ASan/UBSan")
  endif()

  if(KVSTORE_ENABLE_ASAN)
    target_compile_options(${target_name} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target_name} PRIVATE -fsanitize=address)
  endif()

  if(KVSTORE_ENABLE_UBSAN)
    target_compile_options(${target_name} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(${target_name} PRIVATE -fsanitize=undefined)
  endif()

  if(KVSTORE_ENABLE_TSAN)
    target_compile_options(${target_name} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target_name} PRIVATE -fsanitize=thread)
  endif()
endfunction()

