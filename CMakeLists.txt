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

if(DEFINED MECAB_KO_PRIVATE_PREFIX)
    include(ExternalProject)
    include(ProcessorCount)
    ProcessorCount(NPROC)
    if(NPROC EQUAL 0)
        set(NPROC 1)
    endif()

    set(MECAB_KO_INCLUDE_DIRS "${MECAB_KO_PRIVATE_PREFIX}/include")
    set(MECAB_KO_LIBRARY "${MECAB_KO_PRIVATE_PREFIX}/lib/libmecab.a")
    set(MECAB_KO_RC_PATH "${MECAB_KO_PRIVATE_PREFIX}/etc/mecabrc")
    set(MECAB_KO_DIC_DIR
        "${MECAB_KO_PRIVATE_PREFIX}/lib/mecab/dic/mecab-ko-dic"
    )

    ExternalProject_Add(
        mecab_ko_external
        URL
            https://bitbucket.org/eunjeon/mecab-ko/downloads/mecab-0.996-ko-0.9.2.tar.gz
        PREFIX ${CMAKE_BINARY_DIR}/mecab-ko
        CONFIGURE_COMMAND
            <SOURCE_DIR>/configure --prefix=${MECAB_KO_PRIVATE_PREFIX}
            --enable-static --disable-shared --with-pic
        BUILD_COMMAND make -j${NPROC}
        INSTALL_COMMAND make install
        BUILD_IN_SOURCE FALSE
        BUILD_BYPRODUCTS ${MECAB_KO_LIBRARY}
    )

    ExternalProject_Add(
        mecab_ko_dic_external
        URL
            https://bitbucket.org/eunjeon/mecab-ko-dic/downloads/mecab-ko-dic-2.1.1-20180720.tar.gz
        PREFIX ${CMAKE_BINARY_DIR}/mecab-ko-dic
        DEPENDS mecab_ko_external
        CONFIGURE_COMMAND
            sh -c
            "cd <SOURCE_DIR> && ./autogen.sh && ./configure --prefix=${MECAB_KO_PRIVATE_PREFIX} --with-mecab-config=${MECAB_KO_PRIVATE_PREFIX}/bin/mecab-config --with-dicdir=${MECAB_KO_DIC_DIR}"
        BUILD_COMMAND
            ${CMAKE_COMMAND} -E env
            PATH=${MECAB_KO_PRIVATE_PREFIX}/bin:$ENV{PATH} make -j${NPROC}
        INSTALL_COMMAND make install
        COMMAND
            sed -i "s|^dicdir.*|dicdir = ${MECAB_KO_DIC_DIR}|"
            ${MECAB_KO_RC_PATH}
        BUILD_IN_SOURCE TRUE
    )
else()
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
endif()

add_subdirectory(tokenizers)

install(
    FILES "COPYING" "README.md"
    DESTINATION "${GROONGA_TOKENIZER_MECAB_KO_DOC_DIR}"
)
