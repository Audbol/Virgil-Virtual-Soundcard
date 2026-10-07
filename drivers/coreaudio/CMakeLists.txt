# Builds DSVAudio.driver, an AudioServerPlugIn bundle.
# Install: sudo cp -R DSVAudio.driver /Library/Audio/Plug-Ins/HAL/ &&
#          sudo killall coreaudiod
add_library(DSVAudio MODULE DSVPlugIn.cpp)
target_link_libraries(DSVAudio PRIVATE dsv_client
  "-framework CoreAudio" "-framework CoreFoundation")
configure_file(Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
set_target_properties(DSVAudio PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION driver
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
  CXX_VISIBILITY_PRESET hidden)
install(TARGETS DSVAudio LIBRARY DESTINATION /Library/Audio/Plug-Ins/HAL)
