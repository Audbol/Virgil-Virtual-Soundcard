find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)

add_library(asound_module_pcm_dsv MODULE pcm_dsv.cpp)
target_include_directories(asound_module_pcm_dsv PRIVATE ${ALSA_INCLUDE_DIRS})
target_compile_definitions(asound_module_pcm_dsv PRIVATE PIC)
target_link_libraries(asound_module_pcm_dsv PRIVATE dsv_client ${ALSA_LIBRARIES})
set_target_properties(asound_module_pcm_dsv PROPERTIES PREFIX "lib")

# Matches the distro layout, e.g. /usr/lib/x86_64-linux-gnu/alsa-lib
pkg_get_variable(ALSA_LIBDIR alsa libdir)
set(DSV_ALSA_PLUGIN_DIR "${ALSA_LIBDIR}/alsa-lib" CACHE PATH "ALSA plugin install dir")
install(TARGETS asound_module_pcm_dsv LIBRARY DESTINATION ${DSV_ALSA_PLUGIN_DIR})
install(FILES 50-dsv.conf DESTINATION /etc/alsa/conf.d)
