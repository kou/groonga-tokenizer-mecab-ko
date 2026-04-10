/*
  Copyright (C) 2009-2018  Brazil
  Copyright (C) 2018-2026  Sutou Kouhei <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <groonga.h>
#include <groonga/tokenizer.h>

#include <mecab.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _MSC_VER
#  include <fcntl.h>
#endif

typedef struct {
  mecab_model_t *mecab_model;
  mecab_t *mecab;
  grn_plugin_mutex *mutex;
  grn_encoding encoding;
} grn_mecab_ko;

static const char *grn_mecab_ko_lattice_variable_name = "TokenMeCabKo.lattice";

static grn_mecab_ko mecab_ko_default;
static grn_mecab_ko mecab_ko_newline;

static bool grn_mecab_ko_chunked_tokenize_enabled = false;
static int32_t grn_mecab_ko_chunk_size_threshold = 8192;

static const size_t GRN_MECAB_KO_FEATURE_LOCATION_CLASS = 0;
static const size_t GRN_MECAB_KO_FEATURE_LOCATION_SUBCLASS0 = 1;
static const size_t GRN_MECAB_KO_FEATURE_LOCATION_SUBCLASS1 = 2;
static const size_t GRN_MECAB_KO_FEATURE_LOCATION_SUBCLASS2 = 3;
static const size_t GRN_MECAB_KO_FEATURE_LOCATION_BASE_FORM = 6;
static const size_t GRN_MECAB_KO_FEATURE_LOCATION_READING = 7;

typedef struct {
  bool chunked_tokenize;
  int32_t chunk_size_threshold;
  bool include_class;
  bool include_reading;
  bool include_form;
  bool use_reading;
  bool use_base_form;
  grn_obj target_classes;
} grn_mecab_ko_tokenizer_options;

typedef struct {
  grn_mecab_ko_tokenizer_options *options;
  grn_mecab_ko *mecab;
  mecab_lattice_t *lattice;
  grn_obj buf;
  const char *next;
  const char *end;
  grn_tokenizer_query *query;
  grn_obj feature_locations;
} grn_mecab_ko_tokenizer;

static grn_encoding
translate_mecab_ko_charset_to_grn_encoding(const char *charset)
{
  if (grn_strcasecmp(charset, "euc-jp") == 0) {
    return GRN_ENC_EUC_JP;
  } else if (grn_strcasecmp(charset, "utf-8") == 0 ||
             grn_strcasecmp(charset, "utf8") == 0) {
    return GRN_ENC_UTF8;
  } else if (grn_strcasecmp(charset, "shift_jis") == 0 ||
             grn_strcasecmp(charset, "shift-jis") == 0 ||
             grn_strcasecmp(charset, "sjis") == 0) {
    return GRN_ENC_SJIS;
  }
  return GRN_ENC_NONE;
}

static void
grn_mecab_ko_init(grn_ctx *ctx, grn_mecab_ko *mecab, const char *tag)
{
  mecab->mecab_model = NULL;
  mecab->mecab = NULL;
  mecab->mutex = grn_plugin_mutex_open(ctx);
  if (!mecab->mutex) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_NO_MEMORY_AVAILABLE,
                     "[plugin][tokenizer][mecab-ko][init]%s "
                     "failed to initialize mutex",
                     tag);
    return;
  }
  mecab->encoding = GRN_ENC_NONE;
}

static void
grn_mecab_ko_fin(grn_ctx *ctx, grn_mecab_ko *mecab)
{
  if (mecab->mecab) {
    mecab_destroy(mecab->mecab);
    mecab->mecab = NULL;
  }
  if (mecab->mecab_model) {
    mecab_model_destroy(mecab->mecab_model);
    mecab->mecab_model = NULL;
  }
  if (mecab->mutex) {
    grn_plugin_mutex_close(ctx, mecab->mutex);
    mecab->mutex = NULL;
  }
  mecab->encoding = GRN_ENC_NONE;
}

static grn_encoding
get_mecab_ko_encoding(mecab_t *mecab)
{
  grn_encoding encoding = GRN_ENC_NONE;
  const mecab_dictionary_info_t *dictionary_info;
  dictionary_info = mecab_dictionary_info(mecab);
  if (dictionary_info) {
    const char *charset = dictionary_info->charset;
    encoding = translate_mecab_ko_charset_to_grn_encoding(charset);
  }
  return encoding;
}

static void
mecab_ko_tokenizer_options_init(grn_mecab_ko_tokenizer_options *options)
{
  options->chunked_tokenize = grn_mecab_ko_chunked_tokenize_enabled;
  options->chunk_size_threshold = grn_mecab_ko_chunk_size_threshold;
  options->include_class = false;
  options->include_reading = false;
  options->include_form = false;
  options->use_reading = false;
  options->use_base_form = false;
  GRN_TEXT_INIT(&(options->target_classes), GRN_OBJ_VECTOR);
}

static bool
mecab_ko_tokenizer_options_need_default_output(
  grn_ctx *ctx, grn_mecab_ko_tokenizer_options *options)
{
  if (!options) {
    return false;
  }

  if (options->include_class) {
    return true;
  }

  if (options->include_reading) {
    return true;
  }

  if (options->include_form) {
    return true;
  }

  if (options->use_reading) {
    return true;
  }

  if (options->use_base_form) {
    return true;
  }

  if (grn_vector_size(ctx, &(options->target_classes)) > 0) {
    return true;
  }

  return false;
}

static void *
mecab_ko_tokenizer_options_open(grn_ctx *ctx,
                                grn_obj *lexicon,
                                grn_obj *raw_options,
                                void *user_data)
{
  grn_mecab_ko_tokenizer_options *options;

  options = GRN_PLUGIN_MALLOC(ctx, sizeof(grn_mecab_ko_tokenizer_options));
  if (!options) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_NO_MEMORY_AVAILABLE,
                     "[tokenizer][mecab-ko] "
                     "failed to allocate memory for options");
    return NULL;
  }

  mecab_ko_tokenizer_options_init(options);

  GRN_OPTION_VALUES_EACH_BEGIN(ctx, raw_options, i, name, name_length)
  {
    grn_raw_string name_raw;
    name_raw.value = name;
    name_raw.length = name_length;

    if (GRN_RAW_STRING_EQUAL_CSTRING(name_raw, "chunked_tokenize")) {
      options->chunked_tokenize =
        grn_vector_get_element_bool(ctx,
                                    raw_options,
                                    i,
                                    options->chunked_tokenize);
    } else if (GRN_RAW_STRING_EQUAL_CSTRING(name_raw, "chunk_size_threshold")) {
      options->chunk_size_threshold =
        grn_vector_get_element_int32(ctx,
                                     raw_options,
                                     i,
                                     options->chunk_size_threshold);
    } else if (GRN_RAW_STRING_EQUAL_CSTRING(name_raw, "include_class")) {
      options->include_class =
        grn_vector_get_element_bool(ctx,
                                    raw_options,
                                    i,
                                    options->include_class);
    } else if (GRN_RAW_STRING_EQUAL_CSTRING(name_raw, "include_reading")) {
      options->include_reading =
        grn_vector_get_element_bool(ctx,
                                    raw_options,
                                    i,
                                    options->include_reading);
    } else if (GRN_RAW_STRING_EQUAL_CSTRING(name_raw, "include_form")) {
      options->include_form =
        grn_vector_get_element_bool(ctx, raw_options, i, options->include_form);
    } else if (GRN_RAW_STRING_EQUAL_CSTRING(name_raw, "use_reading")) {
      options->use_reading =
        grn_vector_get_element_bool(ctx, raw_options, i, options->use_reading);
    } else if (GRN_RAW_STRING_EQUAL_CSTRING(name_raw, "use_base_form")) {
      options->use_base_form =
        grn_vector_get_element_bool(ctx,
                                    raw_options,
                                    i,
                                    options->use_base_form);
    } else if (GRN_RAW_STRING_EQUAL_CSTRING(name_raw, "target_class")) {
      const char *target_class = NULL;
      unsigned int target_class_length;
      grn_id domain;

      target_class_length = grn_vector_get_element(ctx,
                                                   raw_options,
                                                   i,
                                                   &target_class,
                                                   NULL,
                                                   &domain);
      if (grn_type_id_is_text_family(ctx, domain) && target_class_length > 0) {
        grn_vector_add_element(ctx,
                               &(options->target_classes),
                               target_class,
                               target_class_length,
                               0,
                               GRN_DB_TEXT);
      }
    }
  }
  GRN_OPTION_VALUES_EACH_END();

  return options;
}

static void
mecab_ko_tokenizer_options_close(grn_ctx *ctx, void *data)
{
  grn_mecab_ko_tokenizer_options *options = data;
  GRN_OBJ_FIN(ctx, &(options->target_classes));
  GRN_PLUGIN_FREE(ctx, options);
}

static inline bool
is_delimiter_character(grn_ctx *ctx, const char *character, int character_bytes)
{
  switch (character_bytes) {
  case 1:
    switch (character[0]) {
    case ',':
    case '.':
    case '!':
    case '?':
      return true;
    default:
      return false;
    }
  case 3:
    switch ((unsigned char)(character[0])) {
    case 0xE3:
      switch ((unsigned char)(character[1])) {
      case 0x80:
        switch ((unsigned char)(character[2])) {
        case 0x81: /* U+3001 (0xE3 0x80 0x81 in UTF-8) IDEOGRAPHIC COMMA */
        case 0x82: /* U+3002 (0xE3 0x80 0x82 in UTF-8) IDEOGRAPHIC FULL STOP */
          return true;
        default:
          return false;
        }
      default:
        return false;
      }
      return false;
    case 0xEF:
      switch ((unsigned char)(character[1])) {
      case 0xBC:
        switch ((unsigned char)(character[2])) {
        case 0x81:
          /* U+FF01 (0xEF 0xBC 0x81 in UTF-8) FULLWIDTH EXCLAMATION MARK */
        case 0x9F:
          /* U+FF1F (0xEF 0xBC 0x9F in UTF-8) FULLWIDTH QUESTION MARK */
          return true;
        default:
          return false;
        }
      default:
        return false;
      }
      return false;
    default:
      return false;
    }
  default:
    return false;
  }
}

static bool
chunked_tokenize_utf8_chunk(grn_ctx *ctx,
                            grn_mecab_ko_tokenizer *tokenizer,
                            const char *chunk,
                            size_t chunk_bytes)
{
  const char *tokenized_chunk;
  size_t tokenized_chunk_length;
  mecab_lattice_set_sentence2(tokenizer->lattice, chunk, chunk_bytes);
  if (!mecab_parse_lattice(tokenizer->mecab->mecab, tokenizer->lattice)) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_TOKENIZER_ERROR,
                     "[tokenizer][mecab-ko][chunk] "
                     "mecab_parse_lattice() failed "
                     "len=%zu err=%s",
                     chunk_bytes,
                     mecab_lattice_strerror(tokenizer->lattice));
    return false;
  }
  tokenized_chunk = mecab_lattice_tostr(tokenizer->lattice);
  if (!tokenized_chunk) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_TOKENIZER_ERROR,
                     "[tokenizer][mecab-ko][chunk] "
                     "mecab_sparse_tostr2() failed "
                     "len=%zu err=%s",
                     chunk_bytes,
                     mecab_lattice_strerror(tokenizer->lattice));
    return false;
  }

  if (GRN_TEXT_LEN(&(tokenizer->buf)) > 0) {
    GRN_TEXT_PUTS(ctx, &(tokenizer->buf), "\n");
  }

  tokenized_chunk_length = strlen(tokenized_chunk);
  if (tokenized_chunk_length >= 1 &&
      isspace((int)tokenized_chunk[tokenized_chunk_length - 1])) {
    GRN_TEXT_PUT(ctx,
                 &(tokenizer->buf),
                 tokenized_chunk,
                 tokenized_chunk_length - 1);
  } else {
    GRN_TEXT_PUT(ctx,
                 &(tokenizer->buf),
                 tokenized_chunk,
                 tokenized_chunk_length);
  }

  return true;
}

static bool
chunked_tokenize_utf8(grn_ctx *ctx,
                      grn_mecab_ko_tokenizer *tokenizer,
                      const char *string,
                      unsigned int string_bytes)
{
  const char *chunk_start;
  const char *current;
  const char *last_delimiter;
  const char *string_end = string + string_bytes;

  if ((int32_t)string_bytes < tokenizer->options->chunk_size_threshold) {
    return chunked_tokenize_utf8_chunk(ctx, tokenizer, string, string_bytes);
  }

  chunk_start = current = string;
  last_delimiter = NULL;
  while (current < string_end) {
    int character_bytes;
    const char *current_character;

    if (isspace((int)current[0])) {
      if (chunk_start != current) {
        bool succeeded =
          chunked_tokenize_utf8_chunk(ctx,
                                      tokenizer,
                                      chunk_start,
                                      (size_t)(current - chunk_start));
        if (!succeeded) {
          return succeeded;
        }
      }
      current++;
      chunk_start = current;
      last_delimiter = NULL;
      continue;
    }

    character_bytes = grn_charlen(ctx, current, string_end);
    if (character_bytes == 0) {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_TOKENIZER_ERROR,
                       "[tokenizer][mecab-ko][chunk] "
                       "invalid byte sequence: position=%d",
                       (int)(current - string));
      return false;
    }

    current_character = current;
    current += character_bytes;
    if (is_delimiter_character(ctx, current_character, character_bytes)) {
      last_delimiter = current;
    }

    if ((current - chunk_start) >= tokenizer->options->chunk_size_threshold) {
      bool succeeded;
      if (last_delimiter) {
        succeeded =
          chunked_tokenize_utf8_chunk(ctx,
                                      tokenizer,
                                      chunk_start,
                                      (size_t)(last_delimiter - chunk_start));
        chunk_start = last_delimiter;
      } else {
        succeeded =
          chunked_tokenize_utf8_chunk(ctx,
                                      tokenizer,
                                      chunk_start,
                                      (size_t)(current - chunk_start));
        chunk_start = current;
      }
      if (!succeeded) {
        return succeeded;
      }
      last_delimiter = NULL;
    }
  }

  if (current == chunk_start) {
    return true;
  } else {
    return chunked_tokenize_utf8_chunk(ctx,
                                       tokenizer,
                                       chunk_start,
                                       (size_t)(current - chunk_start));
  }
}

static mecab_model_t *
mecab_ko_model_create(grn_ctx *ctx, grn_mecab_ko_tokenizer_options *options)
{
  mecab_model_t *mecab_model;
  int argc = 0;
  const char *argv[5];
  const char *tag;

  bool need_default_output =
    mecab_ko_tokenizer_options_need_default_output(ctx, options);

  if (need_default_output) {
    tag = "[default]";
  } else {
    tag = "[newline]";
  }

  argv[argc++] = "groonga-tokenizer-mecab-ko";
  if (!need_default_output) {
    argv[argc++] = "-F%m\n";
    argv[argc++] = "-E\n";
  }
#ifdef GRN_WITH_BUNDLED_MECAB_KO
  argv[argc++] = "--rcfile";
  argv[argc++] = GRN_BUNDLED_MECAB_KO_RC_PATH;
#endif

  mecab_model = mecab_model_new(argc, (char **)argv);
  if (!mecab_model) {
    if (need_default_output) {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_TOKENIZER_ERROR,
                       "[tokenizer][mecab-ko][create]%s "
                       "failed to create mecab_model_t: %s: "
                       "mecab_model_new(\"%s\")",
                       tag,
                       mecab_strerror(NULL),
                       argv[0]);
    } else {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_TOKENIZER_ERROR,
                       "[tokenizer][mecab-ko][create]%s "
                       "failed to create mecab_model_t: %s: "
                       "mecab_model_new(\"%s\", \"%s\", \"%s\")",
                       tag,
                       mecab_strerror(NULL),
                       argv[0],
                       argv[1],
                       argv[2]);
    }
  }

  return mecab_model;
}

static void
mecab_ko_init_mecab(grn_ctx *ctx, grn_mecab_ko_tokenizer *tokenizer)
{
  if (mecab_ko_tokenizer_options_need_default_output(ctx, tokenizer->options)) {
    tokenizer->mecab = &mecab_ko_default;
  } else {
    tokenizer->mecab = &mecab_ko_newline;
  }

  if (!tokenizer->mecab->mecab) {
    grn_plugin_mutex_lock(ctx, tokenizer->mecab->mutex);
    if (!tokenizer->mecab->mecab) {
      tokenizer->mecab->mecab_model =
        mecab_ko_model_create(ctx, tokenizer->options);
      if (tokenizer->mecab->mecab_model) {
        tokenizer->mecab->mecab =
          mecab_model_new_tagger(tokenizer->mecab->mecab_model);
        if (tokenizer->mecab->mecab) {
          tokenizer->mecab->encoding =
            get_mecab_ko_encoding(tokenizer->mecab->mecab);
        }
      }
    }
    grn_plugin_mutex_unlock(ctx, tokenizer->mecab->mutex);
  }
}

static void
mecab_ko_next_default_format_skip_eos(grn_ctx *ctx,
                                      grn_mecab_ko_tokenizer *tokenizer)
{
  if (tokenizer->next + 4 < tokenizer->end) {
    return;
  }

  if (strncmp(tokenizer->next, "EOS", 3) == 0) {
    const char *current = tokenizer->next + 3;
    if (current < tokenizer->end && current[0] == '\r') {
      current++;
    }
    if (current < tokenizer->end && current[0] == '\n') {
      current++;
      tokenizer->next = current;
    }
  }
}

typedef struct {
  grn_token *token;
  grn_obj *feature_locations;
  bool ignore_empty_value;
  bool ignore_asterisk_value;
} add_feature_data;

static size_t
mecab_ko_get_feature(grn_ctx *ctx,
                     grn_obj *feature_locations,
                     size_t i,
                     const char **value)
{
  size_t n_locations = GRN_BULK_VSIZE(feature_locations) / sizeof(uint64_t);
  const char *start;
  const char *end;

  if (i + 2 > n_locations) {
    *value = NULL;
    return 0;
  }

  start = (const char *)(GRN_UINT64_VALUE_AT(feature_locations, i));
  end = ((const char *)(GRN_UINT64_VALUE_AT(feature_locations, i + 1))) - 1;
  *value = start;
  return (size_t)(end - start);
}

static void
mecab_ko_next_default_format_add_feature(grn_ctx *ctx,
                                         add_feature_data *data,
                                         const char *name,
                                         size_t i)
{
  grn_token *token = data->token;
  grn_obj *feature_locations = data->feature_locations;
  const char *feature = NULL;
  size_t feature_length;
  grn_obj value;

  feature_length = mecab_ko_get_feature(ctx, feature_locations, i, &feature);
  if (data->ignore_empty_value && feature_length == 0) {
    return;
  }
  if (data->ignore_asterisk_value && feature_length == 1 && feature[0] == '*') {
    return;
  }

  GRN_TEXT_INIT(&value, GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_SET(ctx, &value, feature, feature_length);
  grn_token_metadata_add(ctx,
                         grn_token_get_metadata(ctx, token),
                         name,
                         -1,
                         &value);
  GRN_OBJ_FIN(ctx, &value);
}

static size_t
mecab_ko_next_default_format_consume_token(grn_ctx *ctx,
                                           grn_mecab_ko_tokenizer *tokenizer,
                                           const char **surface_output)
{
  grn_obj *feature_locations = &(tokenizer->feature_locations);
  const char *start;
  const char *current;
  const char *end = tokenizer->end;
  const char *surface = NULL;
  int length = 0;
  size_t surface_length = 0;

  if (surface_output) {
    *surface_output = NULL;
  }

  GRN_BULK_REWIND(feature_locations);
  mecab_ko_next_default_format_skip_eos(ctx, tokenizer);
  start = surface = tokenizer->next;
  for (current = start; current < end; current += length) {
    length = grn_charlen(ctx, current, end);
    if (length == 0) {
      if (surface_output) {
        *surface_output = NULL;
      }
      return 0;
    }

    if (length == 1) {
      if (current[0] == '\r') {
        if (surface_length == 0) {
          surface_length = (size_t)(current - surface);
        } else {
          GRN_UINT64_PUT(ctx, feature_locations, current);
        }
        current++;
        if (current < end && grn_charlen(ctx, current, end) == 1 &&
            current[0] == '\n') {
          current++;
        }
        break;
      } else if (current[0] == '\n') {
        if (surface_length == 0) {
          surface_length = (size_t)(current - surface);
        } else {
          GRN_UINT64_PUT(ctx, feature_locations, current);
        }
        current++;
        break;
      }
    }

    if (surface_length == 0) {
      if (length == 1 && current[0] == '\t') {
        surface_length = (size_t)(current - surface);
        if (current + 1 < end) {
          GRN_UINT64_PUT(ctx, feature_locations, current + 1);
        }
      }
    } else {
      if (length == 1 && current[0] == ',' && current + 1 < end) {
        GRN_UINT64_PUT(ctx, feature_locations, current + 1);
      }
    }
  }
  tokenizer->next = current;
  mecab_ko_next_default_format_skip_eos(ctx, tokenizer);

  if (surface_output) {
    *surface_output = surface;
  }
  return surface_length;
}

static bool
mecab_ko_next_default_format_match_class(grn_ctx *ctx,
                                         const char *target_class,
                                         size_t target_class_length,
                                         const char *class,
                                         size_t class_length)
{
  if (class_length == 0) {
    return false;
  }

  if (target_class_length < class_length) {
    return false;
  }

  return memcmp(target_class, class, class_length) == 0;
}

static void
mecab_ko_next_default_format_consume_needless_tokens(
  grn_ctx *ctx, grn_mecab_ko_tokenizer *tokenizer)
{
  grn_obj *target_classes = &(tokenizer->options->target_classes);
  unsigned int n_target_classes;
  const char *last_next = tokenizer->next;
  bool is_target = false;

  n_target_classes = grn_vector_size(ctx, target_classes);

  if (n_target_classes == 0) {
    return;
  }

  while (tokenizer->next != tokenizer->end && !is_target) {
    const char *surface = NULL;
    size_t surface_length = 0;
    unsigned int i;
    grn_obj *feature_locations;
    const size_t n_classes = 4;
    const char *classes[4];
    size_t class_lengths[4];

    last_next = tokenizer->next;
    surface_length =
      mecab_ko_next_default_format_consume_token(ctx, tokenizer, &surface);

    if (surface_length == 0) {
      break;
    }

    feature_locations = &(tokenizer->feature_locations);
    class_lengths[0] = mecab_ko_get_feature(ctx,
                                            feature_locations,
                                            GRN_MECAB_KO_FEATURE_LOCATION_CLASS,
                                            &(classes[0]));
    class_lengths[1] =
      mecab_ko_get_feature(ctx,
                           feature_locations,
                           GRN_MECAB_KO_FEATURE_LOCATION_SUBCLASS0,
                           &(classes[1]));
    class_lengths[2] =
      mecab_ko_get_feature(ctx,
                           feature_locations,
                           GRN_MECAB_KO_FEATURE_LOCATION_SUBCLASS1,
                           &(classes[2]));
    class_lengths[3] =
      mecab_ko_get_feature(ctx,
                           feature_locations,
                           GRN_MECAB_KO_FEATURE_LOCATION_SUBCLASS2,
                           &(classes[3]));
    for (i = 0; i < n_target_classes; i++) {
      const char *target_class;
      size_t target_class_length;
      bool positive = true;
      size_t j;
      bool matched = false;

      target_class_length = grn_vector_get_element(ctx,
                                                   target_classes,
                                                   i,
                                                   &target_class,
                                                   NULL,
                                                   NULL);
      if (target_class_length > 0) {
        switch (target_class[0]) {
        case '+':
          target_class++;
          target_class_length--;
          break;
        case '-':
          positive = false;
          target_class++;
          target_class_length--;
          break;
        default:
          break;
        }
      }

      for (j = 0; j < n_classes; j++) {
        const size_t class_length = class_lengths[j];

        if (target_class_length == 0) {
          is_target = positive;
          matched = true;
          break;
        }

        if (!mecab_ko_next_default_format_match_class(ctx,
                                                      target_class,
                                                      target_class_length,
                                                      classes[j],
                                                      class_length)) {
          break;
        }
        target_class += class_length;
        target_class_length -= class_length;
        if (target_class_length == 0) {
          is_target = positive;
          matched = true;
          break;
        }
        if (target_class[0] != '/') {
          break;
        }
        target_class++;
        target_class_length--;
      }
      if (matched) {
        break;
      }
    }
  }

  if (is_target) {
    tokenizer->next = last_next;
  }
}

static void
mecab_ko_next_default_format(grn_ctx *ctx,
                             grn_mecab_ko_tokenizer *tokenizer,
                             grn_token *token)
{
  const char *surface;
  size_t surface_length = 0;

  surface_length =
    mecab_ko_next_default_format_consume_token(ctx, tokenizer, &surface);
  if (tokenizer->options->use_reading) {
    grn_obj *feature_locations = &(tokenizer->feature_locations);
    const char *reading = NULL;
    size_t reading_length;
    reading_length = mecab_ko_get_feature(ctx,
                                          feature_locations,
                                          GRN_MECAB_KO_FEATURE_LOCATION_READING,
                                          &reading);
    if (reading_length > 0) {
      grn_token_set_data(ctx, token, reading, (int)reading_length);
    } else {
      grn_token_set_data(ctx, token, surface, (int)surface_length);
    }
  } else if (tokenizer->options->use_base_form) {
    grn_obj *feature_locations = &(tokenizer->feature_locations);
    const char *base_form = NULL;
    size_t base_form_length;
    base_form_length =
      mecab_ko_get_feature(ctx,
                           feature_locations,
                           GRN_MECAB_KO_FEATURE_LOCATION_BASE_FORM,
                           &base_form);
    if (base_form_length > 0) {
      grn_token_set_data(ctx, token, base_form, (int)base_form_length);
    } else {
      grn_token_set_data(ctx, token, surface, (int)surface_length);
    }
  } else {
    grn_token_set_data(ctx, token, surface, (int)surface_length);
  }
  if (tokenizer->options->include_class) {
    add_feature_data data;
    data.token = token;
    data.feature_locations = &(tokenizer->feature_locations);
    data.ignore_empty_value = true;
    data.ignore_asterisk_value = true;
    mecab_ko_next_default_format_add_feature(
      ctx,
      &data,
      "class",
      GRN_MECAB_KO_FEATURE_LOCATION_CLASS);
    mecab_ko_next_default_format_add_feature(
      ctx,
      &data,
      "subclass0",
      GRN_MECAB_KO_FEATURE_LOCATION_SUBCLASS0);
    mecab_ko_next_default_format_add_feature(
      ctx,
      &data,
      "subclass1",
      GRN_MECAB_KO_FEATURE_LOCATION_SUBCLASS1);
    mecab_ko_next_default_format_add_feature(
      ctx,
      &data,
      "subclass2",
      GRN_MECAB_KO_FEATURE_LOCATION_SUBCLASS2);
  }
  if (tokenizer->options->include_reading) {
    add_feature_data data;
    data.token = token;
    data.feature_locations = &(tokenizer->feature_locations);
    data.ignore_empty_value = true;
    data.ignore_asterisk_value = false;
    mecab_ko_next_default_format_add_feature(
      ctx,
      &data,
      "reading",
      GRN_MECAB_KO_FEATURE_LOCATION_READING);
  }
  if (tokenizer->options->include_form) {
    add_feature_data data;
    data.token = token;
    data.feature_locations = &(tokenizer->feature_locations);
    data.ignore_empty_value = true;
    data.ignore_asterisk_value = true;
    mecab_ko_next_default_format_add_feature(ctx, &data, "inflected_type", 4);
    mecab_ko_next_default_format_add_feature(ctx, &data, "inflected_form", 5);
    mecab_ko_next_default_format_add_feature(
      ctx,
      &data,
      "base_form",
      GRN_MECAB_KO_FEATURE_LOCATION_BASE_FORM);
  }
  {
    grn_tokenizer_status status;
    if (surface_length == 0) {
      /* Error */
      status = GRN_TOKEN_LAST;
    } else {
      mecab_ko_next_default_format_consume_needless_tokens(ctx, tokenizer);
      if (tokenizer->next == tokenizer->end) {
        status = GRN_TOKEN_LAST;
      } else {
        status = GRN_TOKEN_CONTINUE;
      }
    }
    grn_token_set_status(ctx, token, status);
  }
}

static void
mecab_ko_next_newline_format(grn_ctx *ctx,
                             grn_mecab_ko_tokenizer *tokenizer,
                             grn_token *token)
{
  int cl;
  const char *p = tokenizer->next, *r;
  const char *e = tokenizer->end;
  grn_tokenizer_status status;

  for (r = p; r < e; r += cl) {
    if (isspace((int)r[0]) && r == p) {
      cl = 1;
      p = r + cl;
      continue;
    }

    if (!(cl = grn_charlen(ctx, r, e))) {
      tokenizer->next = e;
      break;
    }

    if (cl == 1 && r[0] == '\n') {
      tokenizer->next = r + cl;
      break;
    }
  }

  if (r == e || tokenizer->next == e) {
    status = GRN_TOKEN_LAST;
  } else {
    status = GRN_TOKEN_CONTINUE;
  }
  grn_token_set_data(ctx, token, p, (int)(r - p));
  grn_token_set_status(ctx, token, status);
}

static void
grn_mecab_ko_lattice_close(grn_ctx *ctx, void *data)
{
  mecab_lattice_t *lattice = data;
  mecab_lattice_destroy(lattice);
}

/*
  This function is called for a full text search query or a document to be
  indexed. This means that both short/long strings are given.
  The return value of this function is ignored. When an error occurs in this
  function, `ctx->rc' is overwritten with an error code (not GRN_SUCCESS).
 */
static void *
mecab_ko_init(grn_ctx *ctx, grn_tokenizer_query *query)
{
  grn_obj *lexicon;
  grn_mecab_ko_tokenizer *tokenizer;

  lexicon = grn_tokenizer_query_get_lexicon(ctx, query);

  if (!(tokenizer = GRN_PLUGIN_MALLOC(ctx, sizeof(grn_mecab_ko_tokenizer)))) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_NO_MEMORY_AVAILABLE,
                     "[tokenizer][mecab-ko] "
                     "memory allocation to grn_mecab_ko_tokenizer failed");
    return NULL;
  }

  tokenizer->options =
    grn_table_cache_default_tokenizer_options(ctx,
                                              lexicon,
                                              mecab_ko_tokenizer_options_open,
                                              mecab_ko_tokenizer_options_close,
                                              NULL);
  if (ctx->rc != GRN_SUCCESS) {
    GRN_PLUGIN_FREE(ctx, tokenizer);
    return NULL;
  }

  mecab_ko_init_mecab(ctx, tokenizer);
  if (!tokenizer->mecab->mecab) {
    GRN_PLUGIN_FREE(ctx, tokenizer);
    return NULL;
  }
  tokenizer->lattice =
    grn_ctx_get_variable(ctx, grn_mecab_ko_lattice_variable_name, -1);
  if (!tokenizer->lattice) {
    tokenizer->lattice = mecab_model_new_lattice(tokenizer->mecab->mecab_model);
    if (!tokenizer->lattice) {
      GRN_PLUGIN_FREE(ctx, tokenizer);
      return NULL;
    }
    grn_ctx_set_variable(ctx,
                         grn_mecab_ko_lattice_variable_name,
                         -1,
                         tokenizer->lattice,
                         grn_mecab_ko_lattice_close);
  }

  {
    grn_encoding encoding = grn_tokenizer_query_get_encoding(ctx, query);
    if (encoding != tokenizer->mecab->encoding) {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_TOKENIZER_ERROR,
                       "[tokenizer][mecab-ko] "
                       "MeCab-ko dictionary charset (%s) does not match "
                       "the table encoding: <%s>",
                       grn_encoding_to_string(tokenizer->mecab->encoding),
                       grn_encoding_to_string(encoding));
      GRN_PLUGIN_FREE(ctx, tokenizer);
      return NULL;
    }
  }

  tokenizer->query = query;

  {
    grn_obj *string;
    const char *normalized_string;
    unsigned int normalized_string_length;

    string = grn_tokenizer_query_get_normalized_string(ctx, query);
    grn_string_get_normalized(ctx,
                              string,
                              &normalized_string,
                              &normalized_string_length,
                              NULL);
    GRN_TEXT_INIT(&(tokenizer->buf), 0);
    if (grn_tokenizer_query_have_tokenized_delimiter(ctx, query)) {
      tokenizer->next = normalized_string;
      tokenizer->end = tokenizer->next + normalized_string_length;
    } else if (normalized_string_length == 0) {
      tokenizer->next = "";
      tokenizer->end = tokenizer->next;
    } else {
      if (tokenizer->options->chunked_tokenize &&
          ctx->encoding == GRN_ENC_UTF8) {
        if (!chunked_tokenize_utf8(ctx,
                                   tokenizer,
                                   normalized_string,
                                   normalized_string_length)) {
          GRN_PLUGIN_FREE(ctx, tokenizer);
          return NULL;
        }
      } else {
        const char *s;
        mecab_lattice_set_sentence2(tokenizer->lattice,
                                    normalized_string,
                                    normalized_string_length);
        if (!mecab_parse_lattice(tokenizer->mecab->mecab, tokenizer->lattice)) {
          GRN_PLUGIN_ERROR(ctx,
                           GRN_TOKENIZER_ERROR,
                           "[tokenizer][mecab-ko] "
                           "mecab_parse_lattice() failed len=%d err=%s",
                           normalized_string_length,
                           mecab_lattice_strerror(tokenizer->lattice));
          GRN_PLUGIN_FREE(ctx, tokenizer);
          return NULL;
        }
        s = mecab_lattice_tostr(tokenizer->lattice);
        if (!s) {
          GRN_PLUGIN_ERROR(ctx,
                           GRN_TOKENIZER_ERROR,
                           "[tokenizer][mecab-ko] "
                           "mecab_sparse_tostr() failed len=%d err=%s",
                           normalized_string_length,
                           mecab_lattice_strerror(tokenizer->lattice));
          GRN_PLUGIN_FREE(ctx, tokenizer);
          return NULL;
        }
        GRN_TEXT_PUTS(ctx, &(tokenizer->buf), s);
      }
      if (mecab_ko_tokenizer_options_need_default_output(ctx,
                                                         tokenizer->options)) {
        tokenizer->next = GRN_TEXT_VALUE(&(tokenizer->buf));
        tokenizer->end = tokenizer->next + GRN_TEXT_LEN(&(tokenizer->buf));
      } else {
        char *buf, *p;
        size_t bufsize;

        buf = GRN_TEXT_VALUE(&(tokenizer->buf));
        bufsize = GRN_TEXT_LEN(&(tokenizer->buf));
        /* A certain version of MeCab-ko returns trailing lf or spaces. */
        for (p = buf + bufsize - 2; buf <= p && isspace(*(unsigned char *)p);
             p--) {
          *p = '\0';
        }
        tokenizer->next = buf;
        tokenizer->end = p + 1;
      }
    }
  }

  GRN_UINT64_INIT(&(tokenizer->feature_locations), GRN_OBJ_VECTOR);

  mecab_ko_next_default_format_consume_needless_tokens(ctx, tokenizer);

  return tokenizer;
}

/*
  This function returns tokens one by one.
 */
static void
mecab_ko_next(grn_ctx *ctx,
              grn_tokenizer_query *query,
              grn_token *token,
              void *user_data)
{
  grn_mecab_ko_tokenizer *tokenizer = user_data;

  if (grn_tokenizer_query_have_tokenized_delimiter(ctx, tokenizer->query)) {
    grn_encoding encoding = tokenizer->query->encoding;
    tokenizer->next = grn_tokenizer_next_by_tokenized_delimiter(
      ctx,
      token,
      tokenizer->next,
      (unsigned int)(tokenizer->end - tokenizer->next),
      encoding);
  } else if (mecab_ko_tokenizer_options_need_default_output(
               ctx,
               tokenizer->options)) {
    mecab_ko_next_default_format(ctx, tokenizer, token);
  } else {
    mecab_ko_next_newline_format(ctx, tokenizer, token);
  }
}

/*
  This function finalizes a tokenization.
 */
static void
mecab_ko_fin(grn_ctx *ctx, void *user_data)
{
  grn_mecab_ko_tokenizer *tokenizer = user_data;
  if (!tokenizer) {
    return;
  }
  GRN_OBJ_FIN(ctx, &(tokenizer->feature_locations));
  GRN_OBJ_FIN(ctx, &(tokenizer->buf));
  GRN_PLUGIN_FREE(ctx, tokenizer);
}

static void
check_mecab_ko_dictionary_encoding(grn_ctx *ctx)
{
  mecab_model_t *mecab_model;
  mecab_t *mecab;
  grn_encoding encoding;
  bool have_same_encoding_dictionary;

  mecab_model = mecab_ko_model_create(ctx, NULL);
  if (!mecab_model) {
    return;
  }
  mecab = mecab_model_new_tagger(mecab_model);
  if (!mecab) {
    return;
  }

  encoding = GRN_CTX_GET_ENCODING(ctx);
  have_same_encoding_dictionary = (encoding == get_mecab_ko_encoding(mecab));
  mecab_destroy(mecab);

  if (!have_same_encoding_dictionary) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_TOKENIZER_ERROR,
                     "[tokenizer][mecab-ko] "
                     "MeCab has no dictionary that uses the context encoding"
                     ": <%s>",
                     grn_encoding_to_string(encoding));
  }
}

/*
  This function initializes a plugin. This function fails if there is no
  dictionary that uses the context encoding of Groonga.
 */
grn_rc
GRN_PLUGIN_INIT(grn_ctx *ctx)
{
  {
    char env[GRN_ENV_BUFFER_SIZE];

    grn_getenv("GRN_MECAB_KO_CHUNKED_TOKENIZE_ENABLED",
               env,
               GRN_ENV_BUFFER_SIZE);
    grn_mecab_ko_chunked_tokenize_enabled = (env[0] && strcmp(env, "yes") == 0);
  }

  {
    char env[GRN_ENV_BUFFER_SIZE];

    grn_getenv("GRN_MECAB_KO_CHUNK_SIZE_THRESHOLD", env, GRN_ENV_BUFFER_SIZE);
    if (env[0]) {
      char *end = NULL;
      long threshold = strtol(env, &end, 10);
      if (end[0] == '\0') {
        grn_mecab_ko_chunk_size_threshold = threshold;
      }
    }
  }

  grn_mecab_ko_init(ctx, &mecab_ko_default, "[default]");
  grn_mecab_ko_init(ctx, &mecab_ko_newline, "[newline]");
  if (ctx->rc != GRN_SUCCESS) {
    return ctx->rc;
  }

  check_mecab_ko_dictionary_encoding(ctx);
  if (ctx->rc != GRN_SUCCESS) {
    grn_mecab_ko_fin(ctx, &mecab_ko_default);
    grn_mecab_ko_fin(ctx, &mecab_ko_newline);
  }

  return ctx->rc;
}

/*
  This function registers a plugin to a database.
 */
grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *tokenizer;

  tokenizer = grn_tokenizer_create(ctx, "TokenMeCabKo", -1);
  if (tokenizer) {
    grn_tokenizer_set_init_func(ctx, tokenizer, mecab_ko_init);
    grn_tokenizer_set_next_func(ctx, tokenizer, mecab_ko_next);
    grn_tokenizer_set_fin_func(ctx, tokenizer, mecab_ko_fin);
  }

  return rc;
}

/*
  This function finalizes a plugin.
 */
grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  grn_unset_variable(grn_mecab_ko_lattice_variable_name, -1);

  grn_mecab_ko_fin(ctx, &mecab_ko_default);
  grn_mecab_ko_fin(ctx, &mecab_ko_newline);

  return GRN_SUCCESS;
}
