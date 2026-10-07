# cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed.cmake
# Turns a file into a C array so the control panel ships inside virgild.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR bytes "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" arr "${arr}")
file(WRITE "${OUT}" "// Generated from ${IN}; do not edit.\n#pragma once\n#include <cstddef>\n\nstatic const unsigned char ${NAME}[] = {\n${arr}0};\nstatic const size_t ${NAME}Size = ${bytes};\n")
