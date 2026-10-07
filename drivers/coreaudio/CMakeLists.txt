# Builds VirgilAudio.driver, an AudioServerPlugIn bundle.
# Install: sudo cp -R VirgilAudio.driver /Library/Audio/Plug-Ins/HAL/ &&
#          sudo killall coreaudiod
add_library(VirgilAudio MODULE VirgilPlugIn.cpp)
target_link_libraries(VirgilAudio PRIVATE virgil_client
  "-framework CoreAudio" "-framework CoreFoundation")
configure_file(Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
set_target_properties(VirgilAudio PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION driver
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
  CXX_VISIBILITY_PRESET hidden)
install(TARGETS VirgilAudio LIBRARY DESTINATION /Library/Audio/Plug-Ins/HAL)
