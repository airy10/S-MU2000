// license:BSD-3-Clause
//
// compat/dynlib.h - opening a shared library and finding one symbol in it.
//
// One function, because that is all the PLG card host needs
// (src/plg/host.cpp). It lives here rather than in src/plg/ so that the card
// code carries no #ifdef, the same way src/compat/platform.h keeps the
// emulator's platform primitives in one place.
//
// This is not a sandbox and not a plugin system: it opens a path the user named
// and looks up a symbol the user asked for. What runs is the user's problem, and
// doc/plg-cards.md is where the promises are written down.
#ifndef S_MU2000_COMPAT_DYNLIB_H
#define S_MU2000_COMPAT_DYNLIB_H

#include <string>

namespace smu2000 {

class dynlib {
public:
	~dynlib() { close(); }
	dynlib() = default;
	dynlib(const dynlib &) = delete;
	dynlib &operator=(const dynlib &) = delete;

	// Load the library at `path`. On failure returns false and puts a one-line
	// reason in `err`; the library is left closed. Loading the same path twice
	// is fine and gives two handles, which is what the three slots do.
	bool open(const char *path, std::string &err);

	// One symbol, or nullptr. `name` is not mangled on any platform, so the
	// caller writes "plg1500_get_info" and not "_Z17plg1500_get_infov".
	void *symbol(const char *name) const;

	void close();

	const std::string &path() const { return m_path; }
	bool is_open() const { return m_handle != nullptr; }

private:
	void       *m_handle = nullptr;
	std::string m_path;
};

} // namespace smu2000

#endif // S_MU2000_COMPAT_DYNLIB_H
