// This file is part of DSpellCheck Plug-in for Notepad++
// Copyright (C)2019 Sergey Semushin <Predelnik@gmail.com>
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#include "SpellCheckerHelpers.h"
#include "CommonFunctions.h"
#include "MappedWString.h"
#include "SciUtils.h"
#include "Settings.h"
#include "npp/EditorInterface.h"
#include "utils/string_utils.h"

namespace SpellCheckerHelpers {
static bool check_text_enabled_for_active_file(const EditorInterface &editor, const Settings &settings) {
  auto ret = !settings.check_those;
  auto full_path = editor.get_full_current_path();
  for (auto token : make_delimiter_tokenizer(settings.file_types, LR"(;)").get_all_tokens()) {
    if (settings.check_those) {
      ret = ret || PathMatchSpec(full_path.c_str(), std::wstring(token).c_str());
      if (ret)
        break;
    } else {
      ret = ret && (!PathMatchSpec(full_path.c_str(), std::wstring(token).c_str()));
      if (!ret)
        break;
    }
  }
  return ret;
}

bool is_spell_checking_needed_for_file(const EditorInterface &editor, const Settings &settings) {
  return check_text_enabled_for_active_file(editor, settings) && settings.auto_check_text;
}

void apply_word_conversions(const Settings &settings, std::wstring &word) {
  for (auto &c : word) {
    if (settings.ignore_yo) {
      if (c == L'ё')
        c = L'е';
      if (c == L'Ё')
        c = L'Е';
    }
    if (settings.convert_single_quotes) {
      if (c == L'’')
        c = L'\'';
    }
  }
}

void print_to_log(const Settings &settings, std::wstring_view line, HWND parent_wnd) {
  if (!settings.write_debug_log)
    return;

  FILE* fp;
  
  auto err = _wfopen_s(&fp, get_debug_log_path().c_str (), L"a");
  if (err != 0) {
    MessageBox(parent_wnd, L"Error while writing to a log file", to_wstring (strerror(err)).c_str (), MB_OK);
    return;
  }
  _fwprintf_p(fp, L"%.*s\n", static_cast<int> (line.length()), line.data());
  fclose(fp);
}

void cut_apostrophes(const Settings &settings, std::wstring_view &word) {
  if (settings.remove_boundary_apostrophes) {
    while (!word.empty() && word.front() == L'\'')
      word.remove_prefix(1);

    while (!word.empty() && word.back() == L'\'')
      word.remove_suffix(1);
  }
}

void replace_all_tokens(EditorInterface &editor, EditorViewType view, const Settings &settings, const char *from, std::wstring_view to, bool
                        is_proper_name) {
  TextPosition pos = 0;
  auto from_len = static_cast<TextPosition>(strlen(from));
  if (from_len == 0)
    return;

  std::wstring modified_to (to);

  while (true) {
    pos = editor.find_next(view, pos, from);
    if (pos >= 0) {
      auto doc_word_start_pos = editor.get_prev_valid_begin_pos(view, pos);
      auto doc_word_end_pos = editor.get_next_valid_end_pos(view, static_cast<TextPosition>(pos + from_len));
      auto mapped_wstr = editor.get_mapped_wstring_range(view, doc_word_start_pos, doc_word_end_pos);
      std::wstring_view src_word_sv;
      TextPosition buf_word_start_pos = 0;
      auto buf_word_end_pos = static_cast<TextPosition>(mapped_wstr.str.length());
      if (!settings.do_with_tokenizer(mapped_wstr.str, [&](const auto &tokenizer) {
            TextPosition end_pos_offset = 0;
            if (doc_word_end_pos != pos + from_len)
              ++end_pos_offset;
            TextPosition start_pos_offset = 0;
            if (doc_word_start_pos != pos)
              ++start_pos_offset;

            buf_word_start_pos += start_pos_offset;
            buf_word_end_pos -= end_pos_offset;

            if (start_pos_offset != 0) {
              if (tokenizer.prev_token_begin(buf_word_end_pos - 1) != buf_word_start_pos)
                return false;
            }
            if (end_pos_offset != 0) {
              if (tokenizer.next_token_end(buf_word_start_pos) != buf_word_end_pos)
                return false;
            }
            src_word_sv = std::wstring_view (mapped_wstr.str).substr(buf_word_start_pos, buf_word_end_pos - buf_word_start_pos);
            if (!SpellCheckerHelpers::is_word_spell_checking_needed(settings, editor, view, src_word_sv, doc_word_start_pos + start_pos_offset))
              return false;

            return true;
          })) {
        pos = pos + static_cast<TextPosition>(from_len);
        continue;
      }

      bool use_modified = false;
      if (!is_proper_name) {
        auto src_case_type = get_string_case_type(src_word_sv);
        if (src_case_type != string_case_type::mixed) {
          apply_case_type(modified_to, src_case_type);
          use_modified = true;
        }
      }
      auto encoded_to = editor.to_editor_encoding(view, use_modified ? modified_to : to);
      editor.replace_text(view, pos, static_cast<TextPosition>(pos + from_len), encoded_to);
      pos = pos + static_cast<TextPosition>(encoded_to.length());
    } else
      break;
  }
}

bool is_word_spell_checking_needed(const Settings &settings, const EditorInterface &editor, EditorViewType view, std::wstring_view word,
                                   TextPosition word_start) {
  if (word.empty())
    return false;

  auto style = editor.get_style_at(view, word_start);
  auto lexer = editor.get_lexer(view);
  auto category = SciUtils::get_style_category(lexer, style, settings);
  if (category == SciUtils::StyleCategory::unknown) {
    return false;
  }

  if (category != SciUtils::StyleCategory::text &&
      !((category == SciUtils::StyleCategory::comment && settings.check_comments) || (category == SciUtils::StyleCategory::string && settings.check_strings) ||
        (category == SciUtils::StyleCategory::identifier && settings.check_variable_functions))) {
    return false;
  }

  if (editor.is_style_hotspot(view, style)) {
    return false;
  }

  if (static_cast<int>(word.length()) < settings.word_minimum_length)
    return false;

  if (settings.ignore_one_letter && word.length() == 1)
    return false;

  if (settings.ignore_containing_digit &&
      std::find_if(word.begin(), word.end(), [](wchar_t wc) { return IsCharAlphaNumeric(wc) && !IsCharAlpha(wc); }) != word.end())
    return false;

  if (settings.ignore_starting_with_capital && IsCharUpper(word.front())) {
    return false;
  }

  if (settings.ignore_having_a_capital || settings.ignore_all_capital) {
    bool all_upper = IsCharUpper(word.front()), any_upper = false;
    for (auto c : std::wstring_view(word).substr(1)) {
      if (IsCharUpper(c)) {
        any_upper = true;
      } else
        all_upper = false;
    }

    if (!all_upper && any_upper && settings.ignore_having_a_capital)
      return false;

    if (all_upper && settings.ignore_all_capital)
      return false;
  }

  if (settings.ignore_having_underscore && word.find(L'_') != std::wstring_view::npos)
    return false;

  if (settings.ignore_starting_or_ending_with_apostrophe) {
    if (word.front() == '\'' || word.back() == '\'')
      return false;
  }

  return true;
}
} // namespace SpellCheckerHelpers
