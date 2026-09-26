// license:BSD-3-Clause
//
// compat/dynlib.cpp - the three lines that differ per platform.
//
// dlopen / dlsym on macOS and Linux, LoadLibrary / GetProcAddress on Windows.
// The error strings are whatever the OS said, trimmed to one line: a card
// author's mistake (a wrong path, a missing dependency) is the common case and
// the OS says it better than we would.
#include "compat/dynlib.h"

#include <cstdio>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace smu2000 {

namespace {

// dlerror() hands back one string per call and clears it, so a NULL return has to
// be read immediately. Windows puts the text in a static buffer, hence the copy.
std::string last_error(const char *what, const char *path)
{
	std::string s = what;
	s += " ";
	s += path;
#if defined(_WIN32)
	char buf[512] = { 0 };
	const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                                nullptr, GetLastError(),
	                                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	                                buf, sizeof(buf) - 1, nullptr);
	if (n)
		s += ": ";
	if (n)
		s.append(buf, n);
#else
	const char *e = dlerror();
	if (e)
		s += ": ";
	if (e)
		s += e;
#endif
	// One line, and short. dyld answers a missing file with every place it looked,
	// which runs to several hundred characters, and this string ends up in the
	// settings file and on the panel's message line - neither of which wants a
	// paragraph. The first clause is the useful one; the rest is dyld's news.
	for (char &c : s)
		if (c == '\n' || c == '\r')
			c = ' ';
	const size_t keep = 200;
	if (s.size() > keep) {
		s.resize(keep);
		s += "...";
	}
	return s;
}

} // namespace

bool dynlib::open(const char *path, std::string &err)
{
	close();
	if (!path || !*path) {
		err = "no path";
		return false;
	}
#if defined(_WIN32)
	// LOAD_WITH_ALTERED_SEARCH_PATH so a card next to the executable finds its own
	// dependencies next to itself rather than in the process's directory. That
	// is what a VST3 bundle needs: the host's own directory has the plugin's
	// copies, not the card's.
	m_handle = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!m_handle)
		err = last_error("LoadLibrary failed:", path);
#else
	// RTLD_NOW rather than RTLD_LAZY: a card with an unresolved symbol should
	// fail at insert with a name in the message, not at the first note with a
	// crash in the middle of a block.
	m_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!m_handle)
		err = last_error("dlopen failed:", path);
#endif
	if (!m_handle)
		return false;
	m_path = path;
	return true;
}

void *dynlib::symbol(const char *name) const
{
	if (!m_handle || !name)
		return nullptr;
#if defined(_WIN32)
	return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
	return dlsym(m_handle, name);
#endif
}

void dynlib::close()
{
	if (!m_handle)
		return;
#if defined(_WIN32)
	FreeLibrary(static_cast<HMODULE>(m_handle));
#else
	dlclose(m_handle);
#endif
	m_handle = nullptr;
	m_path.clear();
}

} // namespace smu2000
