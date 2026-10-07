find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)

add_library(asound_module_pcm_virgil MODULE pcm_virgil.cpp)
target_include_directories(asound_module_pcm_virgil PRIVATE ${ALSA_INCLUDE_DIRS})
target_compile_definitions(asound_module_pcm_virgil PRIVATE PIC)
target_link_libraries(asound_module_pcm_virgil PRIVATE virgil_client ${ALSA_LIBRARIES})
set_target_properties(asound_module_pcm_virgil PROPERTIES PREFIX "lib")

# Matches the distro layout, e.g. /usr/lib/x86_64-linux-gnu/alsa-lib
pkg_get_variable(ALSA_LIBDIR alsa libdir)
set(VIRGIL_ALSA_PLUGIN_DIR "${ALSA_LIBDIR}/alsa-lib" CACHE PATH "ALSA plugin install dir")
# Install prefix-relative when possible so packagers don't treat the plugin
# as an absolute (config-like) path.
set(_alsa_dest "${VIRGIL_ALSA_PLUGIN_DIR}")
string(FIND "${VIRGIL_ALSA_PLUGIN_DIR}" "${CMAKE_INSTALL_PREFIX}/" _pos)
if(_pos EQUAL 0)
  file(RELATIVE_PATH _alsa_dest "${CMAKE_INSTALL_PREFIX}" "${VIRGIL_ALSA_PLUGIN_DIR}")
endif()
install(TARGETS asound_module_pcm_virgil LIBRARY DESTINATION ${_alsa_dest})
install(FILES 50-virgil.conf DESTINATION /etc/alsa/conf.d)
