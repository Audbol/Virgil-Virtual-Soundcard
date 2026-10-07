# VirgilAsio.dll - ASIO driver. The Steinberg ASIO SDK is not redistributed;
# download it from https://www.steinberg.net/asiosdk and pass ASIO_SDK_DIR.
if(NOT ASIO_SDK_DIR OR NOT EXISTS "${ASIO_SDK_DIR}/common/asio.h")
  message(WARNING "ASIO_SDK_DIR not set or invalid; skipping the ASIO driver")
  return()
endif()

add_library(VirgilAsio SHARED VirgilAsio.cpp VirgilAsio.def)
target_include_directories(VirgilAsio PRIVATE "${ASIO_SDK_DIR}/common" "${ASIO_SDK_DIR}/host/pc")
target_compile_definitions(VirgilAsio PRIVATE WIN32_LEAN_AND_MEAN)
target_link_libraries(VirgilAsio PRIVATE virgil_client ole32 uuid advapi32 user32)
set_target_properties(VirgilAsio PROPERTIES PREFIX "")
install(TARGETS VirgilAsio RUNTIME DESTINATION bin LIBRARY DESTINATION bin)
