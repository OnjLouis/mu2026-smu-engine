// license:BSD-3-Clause
//
// The UI language, shared by the panel strings (ui/texts.h) and the
// ImGui help texts (ui/xg_ui.cpp).
//
// One code per language, chosen once at startup. Precedence:
//
//   1. --lang <code> on the command line (pinned: editor.ini cannot win)
//   2. lang= in editor.ini (the help combo writes it back, so the choice
//      sticks; see xgui::help_checkbox)
//   3. the locale: Japanese when LC_ALL/LC_MESSAGES/LANG/LANGUAGE says ja,
//      English otherwise. Where the environment is mute, the OS is asked:
//      GetUserDefaultLocaleName on Windows, CFLocale on macOS (Finder-
//      launched apps have no LANG). Plain Unix has no such store -- env
//      vars are the native mechanism there (SDL's Unix locale backend reads
//      LANG/LANGUAGE only, so SDL would add nothing and would drag an SDL
//      link into the headless plug-ins and offline tools).
//
// Adding an OS locale provider: the backend reports its OS default locale
// tag through lang_detail::os_locale_provider, registered before main()
// (pc_window.cpp on Windows, pc_window_mac.mm on macOS). Linux registers
// none: the env check below is its native path. Mapping a tag to our
// tables stays here, so a new language never touches the backends.
//
// Adding a language: append its code to LANG_CODES (and its display name to
// LANG_NAMES at the same index), add src/ui/texts_<code>.h with the same
// .field set, dispatch it in ui::texts(), extend the HELP tables in
// xg_ui.cpp, then run python3 tools/check_texts.py. Recompiling is enough;
// nothing is loaded at runtime.
//
// Header-only on purpose, and dependency-free: no windows.h, no
// CoreFoundation, no SDL in here -- every panel TU includes this, so the
// platform headers stay in the platform files. No build file on any
// platform needs a new source.

#ifndef S_MU2000_UI_LANG_H
#define S_MU2000_UI_LANG_H

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "compat/paths.h"

namespace ui {

// THE list of UI languages. One line per language; the enum, the codes,
// the display names, the count and the texts() dispatch below all derive
// from it, so adding a language is one line here plus its data:
//
//   texts_xx.h          the panel/window table (checked by tools/check_texts.py)
//   HELP/fx_help rows   one text per row (a missing one falls back to Japanese)
//   UI_LANG_LIST entry  this list
//
// English must stay listed: it is the fallback when the wanted language
// has no table.
#define UI_LANG_LIST(X) \
	X(ja, "日本語") \
	X(en, "English")

enum class lang : int {
#define X(code, name) code,
	UI_LANG_LIST(X)
#undef X
};

inline constexpr const char *LANG_CODES[] = {
#define X(code, name) #code,
	UI_LANG_LIST(X)
#undef X
};
inline constexpr const char *LANG_NAMES[] = {
#define X(code, name) name,
	UI_LANG_LIST(X)
#undef X
};
inline constexpr int NLANG =
    int(sizeof(LANG_CODES) / sizeof(LANG_CODES[0]));

// Primary-subtag match ("fr" for "fr_FR.UTF-8", "en-US", "LANGUAGE" lists
// use their first entry here; callers split first). -1 when unlisted.
inline constexpr int find_lang_code(std::string_view code)
{
	for (int i = 0; i < NLANG; i++)
		if (std::string_view(LANG_CODES[i]) == code)
			return i;
	return -1;
}

// English is the fallback: it must be listed.
static_assert(find_lang_code("en") >= 0, "English must stay in UI_LANG_LIST");

inline int lang_index(lang l) { return int(l); }

inline const char *lang_code(lang l)
{
	const int i = int(l);
	return (i >= 0 && i < NLANG) ? LANG_CODES[i] : LANG_CODES[1];
}

inline const char *lang_name(lang l)
{
	const int i = int(l);
	return (i >= 0 && i < NLANG) ? LANG_NAMES[i] : LANG_NAMES[1];
}

// Strict match for --lang and editor.ini (a typo must stay visible, so no
// prefix guessing here). False when the code names no listed language.
inline bool lang_try_parse(const char *code, lang &out)
{
	if (!code || !*code)
		return false;
	const int i = find_lang_code(code);
	if (i < 0)
		return false;
	out = lang(i);
	return true;
}

namespace lang_detail {

inline bool is_c_locale(const char *v)
{
	return v && (!std::strcmp(v, "C") || !std::strcmp(v, "POSIX"));
}

// The OS default locale, as a tag ("ja-JP", "en-US", ...; "" when the OS
// says nothing). One provider per platform, registered by that platform's
// pc_window backend before main() runs:
//
//   Windows  src/ui/pc_window.cpp      GetUserDefaultLocaleName
//   macOS    src/ui/pc_window_mac.mm   CFLocale (Finder apps have no LANG)
//   Linux    none: env vars are the native mechanism (SDL's Unix locale
//            backend reads LANG/LANGUAGE only, so SDL would add nothing
//            while dragging an SDL link into headless plug-ins)
//
// Binaries without a backend (offline tools such as render, which reach
// texts() through bootcache.h/engine.h) simply have no provider and fall
// back to env vars -- the sane mechanism for CLI batch tools. Same pattern
// as pc_window::set_drop_handler: runtime registration, no build changes,
// no link-time dependency. Exactly one backend per binary.
using os_locale_provider = std::string (*)();
inline os_locale_provider &os_locale_provider_slot()
{
	static os_locale_provider p = nullptr;
	return p;
}
inline void set_os_locale_provider(os_locale_provider p)
{
	os_locale_provider_slot() = p;
}
// Backends run this in a static initializer:
// `static const os_locale_registrar r(query);`
struct os_locale_registrar {
	explicit os_locale_registrar(os_locale_provider p) { set_os_locale_provider(p); }
};

} // namespace lang_detail

// The primary language subtag of a locale tag: "fr" for "fr_FR.UTF-8",
// "ja" for "ja-JP" or "Japanese_Japan.932".
inline std::string primary_subtag(const char *tag)
{
	if (!tag || !*tag)
		return {};
	if (!std::strncmp(tag, "Japanese", 8) || !std::strncmp(tag, "japanese", 8))
		return "ja";
	std::string out;
	for (const char *p = tag; *p; p++) {
		if (*p == '_' || *p == '-' || *p == '.' || *p == '@')
			break;
		char c = *p;   // locale tags are ASCII; fold case by hand
		if (c >= 'A' && c <= 'Z')
			c += char('a' - 'A');
		out += c;
	}
	return out;
}

// Japanese iff the locale lists it, English otherwise: every candidate
// (each env var, then the OS provider) contributes its primary subtag, and
// the first subtag naming a listed language wins. No language is special-
// cased here, so a new UI_LANG_LIST entry is picked up with no code change;
// an unlisted one (or unknown/empty/C) falls through to English.
inline lang locale_default_lang()
{
	const char *keys[] = { "LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE" };
	for (const char *k : keys) {
		const char *v = std::getenv(k);
		if (!v || !*v || lang_detail::is_c_locale(v))
			continue;
		// LANGUAGE may hold a colon list ("fr:ja"); try each entry in order.
		const char *at = v;
		for (;;) {
			const char *colon = std::strchr(at, ':');
			const std::string sub = primary_subtag(
			    colon ? std::string(at, colon).c_str() : at);
			const int i = find_lang_code(sub);
			if (i >= 0)
				return lang(i);
			if (!colon)
				break;
			at = colon + 1;
		}
	}
	// The environment is mute (no LANG for Windows GUI processes or
	// Finder-launched apps); ask the OS through the pc_window backend.
	if (lang_detail::os_locale_provider_slot()) {
		const int i = find_lang_code(
		    primary_subtag(lang_detail::os_locale_provider_slot()().c_str()));
		if (i >= 0)
			return lang(i);
	}
	return lang::en;
}

// editor.ini lives next to gui.ini (compat/paths.h); the help combo owns
// the lang= line, this only reads it for startup resolution.
inline std::string editor_ini_path()
{
	const std::string dir = smu2000::config_dir();
	if (dir.empty())
		return {};
	return smu2000::join(dir, "editor.ini");
}

inline bool editor_ini_lang(std::string &code_out)
{
	const std::string path = editor_ini_path();
	if (path.empty())
		return false;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char line[256];
	bool found = false;
	while (std::fgets(line, sizeof(line), f)) {
		line[std::strcspn(line, "\r\n")] = 0;
		if (!std::strncmp(line, "lang=", 5)) {
			code_out = line + 5;
			found = true;
		}
	}
	std::fclose(f);
	return found;
}

namespace lang_detail {

inline lang &slot()
{
	static lang s = lang::en;
	return s;
}
inline bool &ready()
{
	static bool b = false;
	return b;
}
inline bool &pinned()
{
	static bool b = false;
	return b;
}
inline std::string &flag()
{
	static std::string s;
	return s;
}

inline void resolve()
{
	if (ready())
		return;
	ready() = true;
	lang l = lang::en;
	if (!flag().empty()) {
		if (lang_try_parse(flag().c_str(), l)) {
			slot() = l;
			return;
		}
		std::fprintf(stderr, "--lang: unknown language '%s' (want ja|en)\n",
		             flag().c_str());
	}
	std::string code;
	if (editor_ini_lang(code) && lang_try_parse(code.c_str(), l)) {
		slot() = l;
		return;
	}
	slot() = locale_default_lang();
}

} // namespace lang_detail

inline lang get_lang()
{
	lang_detail::resolve();
	return lang_detail::slot();
}

inline int help_lang_index() { return int(get_lang()); }

// English unless the language is Japanese: the fallback every bilingual
// call site (help texts, tooltips) uses, so a future third language still
// reads English rather than silently showing Japanese.
inline bool show_english() { return get_lang() != lang::ja; }

// A user or --lang choice: wins over editor.ini from now on.
inline void set_lang(lang l)
{
	lang_detail::slot() = l;
	lang_detail::ready() = true;
	lang_detail::pinned() = true;
}

// True once --lang or the help combo has chosen (editor.ini must not win).
inline bool lang_pinned() { return lang_detail::pinned(); }

// Apply an editor.ini lang= line unless --lang (or the combo) already chose.
inline void apply_ini_lang(const char *code)
{
	if (lang_pinned())
		return;
	lang l = lang::en;
	if (lang_try_parse(code, l))
		lang_detail::slot() = l;
}

// Remember --lang for the upcoming resolution (empty: no flag given) and
// pin it: a forced language wins over editor.ini now and later (a later
// editor.ini read must not flip a running session, which mixes snapshotted
// window titles with repainted strings). Unknown codes warn and pin
// nothing, falling back to ini, then locale.
inline void pin_flag_lang(const char *code)
{
	lang l = lang::en;
	if (!code || !*code)
		return;
	if (!lang_try_parse(code, l)) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			std::string want;
			for (int i = 0; i < NLANG; i++) {
				if (i)
					want += '|';
				want += LANG_CODES[i];
			}
			std::fprintf(stderr, "--lang: unknown language '%s' (want %s)\n",
			             code, want.c_str());
		}
		return;
	}
	lang_detail::flag() = code;
	lang_detail::pinned() = true;
}

// Resolve the language from the parsed --lang value (empty: no flag given).
// Called once in each main, right after the shared parser filled
// tool_args::lang and before any texts() use. Precedence: --lang, then
// lang= in editor.ini, then the locale.
inline void init_lang(const char *flag_code_or_null)
{
	pin_flag_lang(flag_code_or_null);
	(void)get_lang();
}

} // namespace ui

#endif // S_MU2000_UI_LANG_H
