# Copyright (C) 2026  Sutou Kouhei <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

# * Ubuntu 22.04: 3.22
cmake_minimum_required(VERSION 3.22)

message(STATUS "CMake: ${CMAKE_VERSION}")

project(
    groonga-tokenizer-mecab-ko
    VERSION "1.0.0"
    DESCRIPTION "Groonga tokenizer for mecab-ko"
    LANGUAGES C
)

message(STATUS "CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")

if(NOT DEFINED CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 99)
endif()
set(CMAKE_C_STANDARD_REQUIRED TRUE)

include(GNUInstallDirs)

if(NOT DEFINED GROONGA_TOKENIZER_MECAB_KO_DOC_DIR)
    set(GROONGA_TOKENIZER_MECAB_KO_DOC_DIR
        "${CMAKE_INSTALL_DOCDIR}/${PROJECT_NAME}"
    )
endif()

find_package(Groonga REQUIRED)

find_program(MECAB_KO_CONFIG "mecab-config" REQUIRED)
execute_process(
    COMMAND ${MECAB_KO_CONFIG} --inc-dir
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_VARIABLE MECAB_KO_INCLUDE_DIRS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND ${MECAB_KO_CONFIG} --libs-only-L
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_VARIABLE MECAB_KO_LIBRARY_DIRS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
find_library(
    MECAB_KO_LIBRARY
    NAMES mecab
    PATHS ${MECAB_KO_LIBRARY_DIRS}
    REQUIRED
    NO_DEFAULT_PATH
)

add_subdirectory(tokenizers)

install(
    FILES "COPYING" "README.md"
    DESTINATION "${GROONGA_TOKENIZER_MECAB_KO_DOC_DIR}"
)
