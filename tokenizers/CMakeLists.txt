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

add_library(groonga_tokenizer_mecab_ko MODULE mecab_ko.c)
set_target_properties(
    groonga_tokenizer_mecab_ko
    PROPERTIES PREFIX "" OUTPUT_NAME "mecab_ko"
)
target_link_libraries(groonga_tokenizer_mecab_ko PRIVATE Groonga::libgroonga)
target_include_directories(
    groonga_tokenizer_mecab_ko
    SYSTEM
    PRIVATE ${MECAB_KO_INCLUDE_DIRS}
)
target_link_libraries(groonga_tokenizer_mecab_ko PRIVATE ${MECAB_KO_LIBRARY})
if(DEFINED MECAB_KO_PRIVATE_PREFIX)
    add_dependencies(groonga_tokenizer_mecab_ko mecab_ko_dic_external)
    target_compile_definitions(
        groonga_tokenizer_mecab_ko
        PRIVATE
            GRN_WITH_BUNDLED_MECAB_KO
            GRN_BUNDLED_MECAB_KO_RC_PATH="${MECAB_KO_RC_PATH}"
    )
    find_package(Threads REQUIRED)
    target_link_libraries(groonga_tokenizer_mecab_ko PRIVATE Threads::Threads)
endif()
install(
    TARGETS groonga_tokenizer_mecab_ko
    DESTINATION "${GRN_PLUGINS_DIR}/tokenizers"
)
