// license:BSD-3-Clause
//
// abort()・std::terminate で止まるときに、呼び出し元を書き残す（Windows の gui）。
//
// イベントログには「msvcrt.dll の abort」としか残らず、誰が abort を呼んだのか分からない。
// SIGABRT と std::terminate を受けて、そのときの呼び出しの並び（モジュール + オフセット）と、
// 例外で止まったならその what() を <設定>/S-MU2000/crash.txt に足していく。
// オフセットは nm -C --defined-only build/gui.exe の番地（0x140000000 起点）と突き合わせる
// （doc/debugging のメモ「落ちたらイベントログのフォールトオフセット＋nm」と同じやり方）。
#ifndef S_MU2000_COMPAT_CRASH_LOG_H
#define S_MU2000_COMPAT_CRASH_LOG_H
#pragma once

#if defined(_WIN32)

#include "compat/paths.h"

#include <windows.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <typeinfo>

namespace smu2000::crash_log {

inline void write(const char *why)
{
	const std::string dir = smu2000::config_dir();
	if (dir.empty())
		return;
	std::FILE *f = std::fopen((dir + "crash.txt").c_str(), "ab");
	if (!f)
		return;
	const std::time_t t = std::time(nullptr);
	char when[64];
	std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
	std::fprintf(f, "%s  %s (thread %lu)\r\n", when, why, GetCurrentThreadId());
	void *frames[48];
	const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
	for (USHORT i = 0; i < n; i++) {
		HMODULE mod = nullptr;
		char name[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       static_cast<LPCSTR>(frames[i]), &mod) && mod) {
			char full[MAX_PATH];
			if (GetModuleFileNameA(mod, full, MAX_PATH)) {
				const char *base = std::strrchr(full, '\\');
				std::snprintf(name, sizeof(name), "%s", base ? base + 1 : full);
			}
		}
		const unsigned long long off = mod ? (unsigned long long)(reinterpret_cast<const char *>(frames[i]) -
		                                                         reinterpret_cast<const char *>(mod)) : 0;
		std::fprintf(f, "  #%-2u %s+0x%llx\r\n", unsigned(i), name, off);
	}
	std::fclose(f);
}

inline void on_abort(int)
{
	write("SIGABRT");
}

inline void on_terminate()
{
	std::string why = "std::terminate";
	if (const std::exception_ptr e = std::current_exception()) {
		try {
			std::rethrow_exception(e);
		} catch (const std::exception &x) {
			why += std::string(": ") + typeid(x).name() + ": " + x.what();
		} catch (...) {
			why += ": (not a std::exception)";
		}
	}
	write(why.c_str());
	std::abort();
}

inline void install()
{
	std::signal(SIGABRT, on_abort);
	std::set_terminate(on_terminate);
}

// ---- プラグイン（ホストのプロセスに読み込まれる DLL）用
// abort・std::terminate に加えて、**この DLL の中で**起きたアクセス違反なども書く（ホストやほかのプラグインの
// ものは書かない）。DLL が外れるときに必ず uninstall すること: 残すと、消えたコードを指す受け口がプロセスに残る
inline HMODULE &plugin_module() { static HMODULE m = nullptr; return m; }
inline void *&plugin_veh() { static void *h = nullptr; return h; }
inline void (*&plugin_prev_abort())(int) { static void (*p)(int) = SIG_DFL; return p; }

inline LONG CALLBACK on_exception(EXCEPTION_POINTERS *x)
{
	static LONG count = 0;
	const DWORD code = x->ExceptionRecord->ExceptionCode;
	if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
	    code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
		return EXCEPTION_CONTINUE_SEARCH;
	HMODULE mod = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        static_cast<LPCSTR>(x->ExceptionRecord->ExceptionAddress), &mod) || mod != plugin_module())
		return EXCEPTION_CONTINUE_SEARCH;
	if (InterlockedIncrement(&count) > 4)        // 同じ所でくり返すなら、最初の数回だけ
		return EXCEPTION_CONTINUE_SEARCH;
	char why[96];
	std::snprintf(why, sizeof(why), "exception 0x%08lx at +0x%llx", code,
	              (unsigned long long)(static_cast<const char *>(x->ExceptionRecord->ExceptionAddress) - reinterpret_cast<const char *>(mod)));
	write(why);
	return EXCEPTION_CONTINUE_SEARCH;
}

inline void install_plugin(HMODULE self)
{
	if (plugin_module())
		return;
	plugin_module() = self;
	plugin_prev_abort() = std::signal(SIGABRT, on_abort);
	std::set_terminate(on_terminate);
	plugin_veh() = AddVectoredExceptionHandler(0, on_exception);
}

inline void uninstall_plugin()
{
	if (!plugin_module())
		return;
	if (plugin_veh())
		RemoveVectoredExceptionHandler(plugin_veh());
	plugin_veh() = nullptr;
	std::signal(SIGABRT, plugin_prev_abort());
	plugin_module() = nullptr;
}

} // namespace smu2000::crash_log

#endif

#endif
