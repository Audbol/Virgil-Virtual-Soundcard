# Builds bridge/ (Rust: Inferno + C ABI) with cargo and exposes it as the
# imported static library `virgil_dante`.
find_program(CARGO_EXECUTABLE cargo HINTS $ENV{HOME}/.cargo/bin REQUIRED)

set(_cargo_dir ${CMAKE_BINARY_DIR}/cargo)
set(_manifest ${CMAKE_SOURCE_DIR}/bridge/Cargo.toml)
set(_targets "")
if(APPLE)
  if(CMAKE_OSX_ARCHITECTURES)
    foreach(arch IN LISTS CMAKE_OSX_ARCHITECTURES)
      if(arch STREQUAL "arm64")
        list(APPEND _targets aarch64-apple-darwin)
      else()
        list(APPEND _targets ${arch}-apple-darwin)
      endif()
    endforeach()
  endif()
elseif(WIN32 AND MINGW)
  list(APPEND _targets x86_64-pc-windows-gnu)
elseif(WIN32)
  list(APPEND _targets x86_64-pc-windows-msvc)
endif()

if(WIN32 AND NOT MINGW)
  set(_libname virgil_dante.lib)
else()
  set(_libname libvirgil_dante.a)
endif()

set(_outputs "")
set(_commands "")
if(_targets STREQUAL "")
  set(_lib ${_cargo_dir}/release/${_libname})
  list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E env RUSTFLAGS=-Awarnings ${CARGO_EXECUTABLE} build --release --quiet
       --manifest-path ${_manifest} --target-dir ${_cargo_dir})
else()
  foreach(t IN LISTS _targets)
    list(APPEND _outputs ${_cargo_dir}/${t}/release/${_libname})
    list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E env RUSTFLAGS=-Awarnings ${CARGO_EXECUTABLE} build --release --quiet
         --manifest-path ${_manifest} --target-dir ${_cargo_dir} --target ${t})
  endforeach()
  list(LENGTH _outputs _n)
  if(_n GREATER 1)
    set(_lib ${_cargo_dir}/universal/${_libname})
    list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E make_directory ${_cargo_dir}/universal
         COMMAND lipo -create ${_outputs} -output ${_lib})
  else()
    list(GET _outputs 0 _lib)
  endif()
endif()

file(GLOB_RECURSE _rust_sources CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/bridge/src/*.rs ${CMAKE_SOURCE_DIR}/third_party/inferno/*.rs)
add_custom_command(
  OUTPUT ${_lib}
  ${_commands}
  DEPENDS ${_rust_sources} ${_manifest}
  COMMENT "Building Dante bridge (Rust/Inferno)"
  VERBATIM)
add_custom_target(virgil_dante_build DEPENDS ${_lib})

add_library(virgil_dante STATIC IMPORTED GLOBAL)
set_target_properties(virgil_dante PROPERTIES IMPORTED_LOCATION ${_lib})
add_dependencies(virgil_dante virgil_dante_build)
if(WIN32)
  set_property(TARGET virgil_dante PROPERTY INTERFACE_LINK_LIBRARIES
    iphlpapi advapi32 cfgmgr32 fwpuclnt gdi32 kernel32 msimg32 ntdll ole32 shell32
    user32 winspool ws2_32 bcrypt userenv dbghelp synchronization)
elseif(APPLE)
  set_property(TARGET virgil_dante PROPERTY INTERFACE_LINK_LIBRARIES
    "-framework CoreFoundation" "-framework SystemConfiguration" "-framework Security"
    "-framework IOKit" resolv)
else()
  set_property(TARGET virgil_dante PROPERTY INTERFACE_LINK_LIBRARIES pthread dl m util rt)
endif()
