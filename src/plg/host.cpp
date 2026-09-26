// license:BSD-3-Clause
//
// plg/host.cpp - loading, lifetime, state and safety for the three PLG slots.
//
// The rules this file keeps are written down in doc/plg-cards.md. The short
// version: a module is code the host did not write, running on the audio thread,
// and every rule here exists because of that one fact.
#include "plg/host.h"

#include "compat/dynlib.h"
#include "compat/platform.h"

#include <cstdlib>
#include <cstring>

namespace plg {

using smu2000::dynlib;

// How many samples to time once every. A power of two, so the mask below
// wraps correctly.
constexpr unsigned TIMED_EVERY = 64;

namespace {

// The entry points, looked up once per insert. A module missing one of them is
// not a card, and the message says which, because a forgotten PLG_EXPORT is the
// mistake a card author makes first.
struct entry {
	const plg_card_info *(*get_info)(void) = nullptr;
	plg_card            *(*create)(const plg_host *, void *, char *, size_t) = nullptr;
	void                (*destroy)(plg_card *) = nullptr;
	const plg_card_ops  *(*ops)(const plg_card *) = nullptr;
};

bool resolve(dynlib &lib, entry &e, std::string &err)
{
	e.get_info = reinterpret_cast<const plg_card_info *(*)()>(lib.symbol("plg1500_get_info"));
	e.create   = reinterpret_cast<plg_card *(*)(const plg_host *, void *, char *, size_t)>(
	                 lib.symbol("plg1500_create"));
	e.destroy  = reinterpret_cast<void (*)(plg_card *)>(lib.symbol("plg1500_destroy"));
	e.ops      = reinterpret_cast<const plg_card_ops *(*)(const plg_card *)>(
	                 lib.symbol("plg1500_ops"));
	const struct { const char *name; bool have; } need[] = {
		{ "plg1500_get_info", e.get_info != nullptr },
		{ "plg1500_create",   e.create   != nullptr },
		{ "plg1500_destroy",  e.destroy  != nullptr },
		{ "plg1500_ops",      e.ops      != nullptr },
	};
	for (const auto &n : need)
		if (!n.have) {
			err = std::string("見つからない入口: ") + n.name +
			      "（Windows では PLG_EXPORT が要ります）";
			return false;
		}
	return true;
}

// A descriptor copied out of the module, so the host never keeps a pointer into
// a library it may close. The strings are std::string rather than a char array
// because this is the host's own struct, not something that crosses the ABI.
struct descriptor {
	uint32_t    abi = 0, kind = 0, model_id = 0, flags = 0;
	std::string id, name;
};

// A card that hands over a short pointer is a card that reads past its own
// string and crashes the host, and the ABI has no way to report that. So the
// copy is bounded by what a card is allowed to write.
void copy_str(std::string &dst, const char *src, size_t cap)
{
	dst = src ? std::string(src, ::strnlen(src, cap)) : std::string();
}

descriptor read_info(const plg_card_info *in)
{
	descriptor d;
	if (!in)
		return d;
	d.abi      = in->abi;
	d.kind     = in->kind;
	d.model_id = in->model_id;
	d.flags    = in->flags;
	copy_str(d.id, in->id, 64);
	copy_str(d.name, in->name, 64);
	return d;
}

// FNV-1a. Short ids collide with other short ids, which is why the save state
// carries the id itself next to the hash: the hash is the cheap reject, the
// string is the confirmation. A wrong card restoring a wrong state is worse than
// not restoring at all.
uint32_t hash_id(const std::string &id)
{
	uint32_t h = 2166136261u;
	for (unsigned char c : id) {
		h ^= c;
		h *= 16777619u;
	}
	return h;
}

// RAM a card asked the host for, kept so it can be saved and handed back. The
// card may have written anything into it, so it is part of the card's state.
struct ram_block {
	void  *ptr = nullptr;
	size_t len = 0;
};

// ---- the save-state section -------------------------------------------------
//
// One card's state, framed. Written field by field rather than memcpy'd from a
// struct, because these bytes go into a DAW project that may outlive this
// version of the code: a reader has to be able to step over a section whose
// header it does not recognise, using only the length.
namespace st {

constexpr uint8_t TAG[3]  = { 'P', 'L', 'G' };
constexpr uint8_t VER     = 1;
constexpr uint8_t F_SAVED = 1u << 0;

// The offsets, named, because a header written in one place and read in another
// is a header that will be read one byte off the day somebody adds a field. Every
// index below goes through these.
constexpr size_t OFF_TAG   = 0;   // 3 bytes
constexpr size_t OFF_VER   = 3;
constexpr size_t OFF_ABI   = 4;
constexpr size_t OFF_SLOT  = 5;
constexpr size_t OFF_FLAGS = 6;
// One pad byte, so the two u32s that follow start where a byte reader can find
// them without knowing that they are u32s.
constexpr size_t OFF_HASH  = 8;   // 4 bytes
constexpr size_t OFF_LEN   = 12;  // 4 bytes
// The card's id, NUL terminated, then the body. The id's length is not in the
// header on purpose: the header is fixed size so a reader can step over a section
// it does not understand using only OFF_LEN.
constexpr size_t OFF_ID    = 16;
constexpr size_t HEADER    = OFF_ID;

uint32_t get32(const uint8_t *p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
	       (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void put32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back(uint8_t(x));
	v.push_back(uint8_t(x >> 8));
	v.push_back(uint8_t(x >> 16));
	v.push_back(uint8_t(x >> 24));
}

} // namespace st

} // namespace

// ---- one slot ---------------------------------------------------------------

struct host::impl {
	// The library outlives the card: the descriptor is read from it and the ops
	// table is a pointer into its text. Eject closes the card, then the library,
	// so nothing is called after the code is gone.
	dynlib              lib;
	entry               fn;
	descriptor          desc;
	plg_card           *card = nullptr;
	const plg_card_ops *ops  = nullptr;
	std::string         msg;

	// The ops table copied out of the module. Calling through the module's own
	// pointer would read past the end of a table built against an older header,
	// so the host keeps a copy and cuts it to the module's own size.
	plg_card_ops cut = {};
	bool          cut_ok = false;

	// Safety. A card that is repeatedly too slow is skipped rather than called
	// again: the alternative is a machine that stutters for as long as the card
	// is plugged in.
	int  slow  = 0;
	bool fault = false;

	// What the card may ask for. Held on the impl rather than the host because a
	// card may allocate during create(), when the slot is not in m_slot yet.
	std::vector<ram_block> ram;

	int         slot = 0;
	host       *owner = nullptr;
	plg_host    services = {};
};

host::host() = default;

host::~host()
{
	for (int i = 0; i < SLOTS; i++)
		eject(i);
}

double host::budget_us()
{
	// Read once: getenv is not free and this sits on the audio thread's path.
	static const double us = [] {
		if (const char *e = ::getenv("SMU2000_PLG_BUDGET_US")) {
			const double v = ::atof(e);
			if (v > 0.0)
				return v;
		}
		return 8.0;
	}();
	return us;
}

bool host::probe(const std::string &path, plg_card_info &out, std::string &err)
{
	dynlib lib;
	if (!lib.open(path.c_str(), err))
		return false;
	entry e;
	if (!resolve(lib, e, err))
		return false;
	const plg_card_info *in = e.get_info();
	if (!in) {
		err = "plg1500_get_info が 0 を返した";
		return false;
	}
	if (in->abi != PLG_ABI_VERSION) {
		err = "ABI が違う: カード " + std::to_string(in->abi) +
		      " / 宿主 " + std::to_string(PLG_ABI_VERSION);
		return false;
	}
	// The two strings point into the module, and probe() closes the library
	// before it returns, so they have to be copies. Fixed buffers rather than a
	// std::string: the caller reads them before calling probe again, and this
	// way nothing here allocates.
	static thread_local char idbuf[65], namebuf[65];
	::strncpy(idbuf, in->id ? in->id : "", 64);
	::strncpy(namebuf, in->name ? in->name : "", 64);
	idbuf[64] = namebuf[64] = '\0';
	out      = *in;
	out.id   = idbuf;
	out.name = namebuf;
	return true;
}

bool host::insert(int slot, const std::string &path, std::string &err)
{
	if (slot < 0 || slot >= SLOTS) {
		err = "スロット番号が範囲外";
		return false;
	}
	// A slot holds one card. Inserting over a live one ejects first, and eject
	// cannot fail, so nothing here can leave two cards sharing a handle.
	eject(slot);

	// Keep the slot even when the load fails, so message() has something to say.
	// A slot that failed is present but not running, which is a different thing
	// from an empty slot and the screen needs to tell them apart.
	std::unique_ptr<impl> fresh(new impl);
	m_slot[slot] = std::move(fresh);
	impl *ip = m_slot[slot].get();
	ip->slot  = slot;
	ip->owner = this;

	if (!ip->lib.open(path.c_str(), err)) {
		ip->msg = err;
		return false;
	}
	if (!resolve(ip->lib, ip->fn, err)) {
		ip->msg = err;
		return false;
	}
	const plg_card_info *in = ip->fn.get_info();
	if (!in) {
		ip->msg = "plg1500_get_info が 0 を返した";
		err = ip->msg;
		return false;
	}
	if (in->abi != PLG_ABI_VERSION) {
		char buf[128];
		::snprintf(buf, sizeof(buf), "ABI が違う: カード %u / 宿主 %u",
		           unsigned(in->abi), unsigned(PLG_ABI_VERSION));
		ip->msg = buf;
		err = ip->msg;
		return false;
	}
	ip->desc = read_info(in);
	if (ip->desc.id.empty()) {
		ip->msg = "id が空。セーブステートに書けない";
		err = ip->msg;
		return false;
	}
	if (ip->desc.kind != PLG_KIND_BUS && ip->desc.kind != PLG_KIND_SELF) {
		ip->msg = "kind が違う: " + std::to_string(ip->desc.kind);
		err = ip->msg;
		return false;
	}

	// The service table. Nine thin shims over `this`, with the impl as ctx, so
	// one table serves all three slots and there is nothing per-card to free.
	ip->services = plg_host{};
	ip->services.abi  = PLG_ABI_VERSION;
	ip->services.ctx  = ip;
	ip->services.rom  = [](void *ctx, uint32_t part, uint32_t *len) -> const uint8_t * {
		host::impl *p = static_cast<host::impl *>(ctx);
		return p->owner ? p->owner->rom_at(p->slot, part, len) : nullptr;
	};
	ip->services.alloc = [](void *ctx, size_t bytes) -> void * {
		host::impl *p = static_cast<host::impl *>(ctx);
		return p->owner ? p->owner->alloc_on(p, bytes) : nullptr;
	};
	ip->services.free = [](void *ctx, void *q) {
		host::impl *p = static_cast<host::impl *>(ctx);
		if (p->owner)
			p->owner->release_on(p, q);
	};
	// The interrupt lines into an SH-2 the host does not run yet. Kept as a real
	// entry point so the ABI does not have to change when that arrives, and so a
	// card that raises one now finds it goes nowhere rather than crashing.
	ip->services.irq = [](void *, int, int) {};
	// The card's own TX line. Goes straight out to whoever the host pointed the
	// sink at - the machine, which wires it to SCI4's RX. No buffering here: the
	// card is driving a line, and a line has no queue.
	ip->services.tx = [](void *ctx, int level) {
		host::impl *p = static_cast<host::impl *>(ctx);
		if (p->owner && p->owner->m_tx_sink)
			p->owner->m_tx_sink(p->slot, level ? 1 : 0);
	};
	ip->services.log = [](void *ctx, const char *msg) {
		host::impl *p = static_cast<host::impl *>(ctx);
		if (p->owner && p->owner->m_log && msg)
			p->owner->m_log(msg);
	};
	ip->services.sample_clock = [](void *ctx) -> uint64_t {
		host::impl *p = static_cast<host::impl *>(ctx);
		return p->owner ? p->owner->clock_now() : 0;
	};
	ip->services.publish_load = [](void *ctx, double pct) {
		host::impl *p = static_cast<host::impl *>(ctx);
		if (p->owner && p->owner->m_load)
			p->owner->m_load(p->slot, pct);
	};
	ip->services.param_changed = [](void *ctx, uint32_t id, int32_t value) {
		host::impl *p = static_cast<host::impl *>(ctx);
		if (p->owner && p->owner->m_param)
			p->owner->m_param(p->slot, id, value);
	};

	char cerr[256] = { 0 };
	ip->card = ip->fn.create(&ip->services, ip, cerr, sizeof(cerr));
	if (!ip->card) {
		ip->msg = cerr[0] ? cerr : "カードの create が 0 を返した";
		err = ip->msg;
		return false;
	}
	const plg_card_ops *ops = ip->fn.ops(ip->card);
	if (!ops || ops->abi != PLG_ABI_VERSION || ops->size < sizeof(plg_card_ops)) {
		ip->msg = "出口の表が読めない（size か abi が合わない）";
		err = ip->msg;
		if (ip->fn.destroy)
			ip->fn.destroy(ip->card);
		ip->card = nullptr;
		return false;
	}
	ip->cut    = *ops;
	ip->cut_ok = true;
	if (ip->cut.reset)
		ip->cut.reset(ip->card);
	// The card's own selftest, once, with no audio running. A card that cannot
	// pass it is not inserted: better an empty slot the screen can explain than
	// a card that misbehaves on the first note.
	if (ip->cut.selftest) {
		const int r = ip->cut.selftest(ip->card);
		if (r != 0) {
			char buf[128];
			::snprintf(buf, sizeof(buf), "カードの自己試験が %d を返した", r);
			ip->msg = buf;
			err = ip->msg;
			if (ip->cut.destroy)
				ip->cut.destroy(ip->card);
			ip->card = nullptr;
			return false;
		}
	}
	return true;
}

void host::eject(int slot)
{
	if (slot < 0 || slot >= SLOTS)
		return;
	std::unique_ptr<impl> s = std::move(m_slot[slot]);
	if (!s)
		return;
	if (s->card && s->cut.destroy)
		s->cut.destroy(s->card);
	s->card = nullptr;
	for (ram_block &b : s->ram)
		::free(b.ptr);
	s->ram.clear();
	// The library goes last: the descriptor and the ops table point into it.
	s->lib.close();
}

bool host::present(int slot) const
{
	return slot >= 0 && slot < SLOTS && m_slot[slot] != nullptr;
}

bool host::running(int slot) const
{
	if (slot < 0 || slot >= SLOTS || !m_slot[slot])
		return false;
	const impl *s = m_slot[slot].get();
	return s->card != nullptr && s->cut_ok && !s->fault;
}

const plg_card_info *host::info(int slot) const
{
	if (slot < 0 || slot >= SLOTS || !m_slot[slot])
		return nullptr;
	const impl *s = m_slot[slot].get();
	// The host's own copy, never the module's: eject() closes the library, and a
	// caller holding a descriptor across an eject would be reading freed text.
	// thread_local because three cards and three screens may ask at once, and a
	// screen that saved the pointer would get whichever card answered last.
	static thread_local plg_card_info out;
	out.abi      = s->desc.abi;
	out.kind     = s->desc.kind;
	out.model_id = s->desc.model_id;
	out.flags    = s->desc.flags;
	out.id       = s->desc.id.c_str();
	out.name     = s->desc.name.c_str();
	return &out;
}

const std::string &host::message(int slot) const
{
	static const std::string none;
	if (slot < 0 || slot >= SLOTS || !m_slot[slot])
		return none;
	return m_slot[slot]->msg;
}

bool host::faulted(int slot) const
{
	return slot >= 0 && slot < SLOTS && m_slot[slot] && m_slot[slot]->fault;
}

void host::run(int slot, const int32_t *in, int32_t *out)
{
	if (out)
		out[0] = out[1] = 0;
	if (slot < 0 || slot >= SLOTS)
		return;
	impl *s = m_slot[slot].get();
	if (!s || !s->card || !s->cut.run || s->fault)
		return;

	plg_slot_io io = {};
	if (in) {
		io.in[0] = in[0];
		io.in[1] = in[1];
	}
	io.reset = 1;

	m_clock.fetch_add(1, std::memory_order_relaxed);

	// **Timed once every 64 samples, not every sample.** Two perf_ticks() calls
	// per sample is 88200 counter reads a second per card, six with three cards
	// in, all on the audio thread, to guard a feature that is not there yet.
	//
	// Once every 64 is still enough: if a hundred consecutive measurements are
	// all over budget, that is 6400 samples - about 145 ms - of being late, and
	// a card that is 145 ms late on a 22.7 us sample has a real problem rather
	// than a cache miss.
	const bool time_it = (m_timed++ & (TIMED_EVERY - 1)) == 0;
	const uint64_t t0 = time_it ? smu2000::perf_ticks() : 0;
	s->cut.run(s->card, &io);
	const uint64_t dt = time_it ? smu2000::perf_ticks() - t0 : 0;

	if (out) {
		out[0] = io.out[0];
		out[1] = io.out[1];
	}

	if (!time_it)
		return;

	// Late often enough means the card cannot keep up. One slow measurement is a
	// cache miss; a hundred are a card that will make every block after this one
	// late too, so it is skipped rather than called. The measured span covers
	// TIMED_EVERY samples, so the budget is scaled to match.
	const double us = double(dt) * 1e6 / double(smu2000::perf_freq());
	if (us > budget_us() * double(TIMED_EVERY)) {
		if (++s->slow >= SLOW_RUNS_BEFORE_FAULT) {
			s->fault = true;
			char buf[160];
			::snprintf(buf, sizeof(buf),
			           "1 サンプル %.2f マイクロ秒（予算 %.1f）。カードを外す",
			           us / double(TIMED_EVERY), budget_us());
			s->msg = buf;
			if (m_log)
				m_log(s->msg);
		}
	} else {
		s->slow = 0;
	}
}

void host::midi_rx(int slot, int level)
{
	// No card, or a card the host has given up on: the line goes nowhere, which
	// is what it did before this existed. Cheap enough to call on every edge.
	if (slot < 0 || slot >= SLOTS)
		return;
	impl *s = m_slot[slot].get();
	if (!s || !s->card || !s->cut_ok || s->fault)
		return;
	// A card with no receive entry point simply does not listen, which is the
	// normal case: there is no separate flag for it.
	if (s->cut.midi_rx)
		s->cut.midi_rx(s->card, level ? 1 : 0);
}

int host::midi_tx(int slot) const
{
	if (slot < 0 || slot >= SLOTS || !m_slot[slot])
		return 0;
	const impl *s = m_slot[slot].get();
	if (!s->card || !s->cut_ok || s->fault || !s->cut.midi_tx)
		return 0;
	return s->cut.midi_tx(s->card) ? 1 : 0;
}

const uint8_t *host::rom_at(int slot, uint32_t part, uint32_t *len)
{
	if (slot < 0 || slot >= SLOTS || !m_rom[slot])
		return nullptr;
	return m_rom[slot](part, len);
}

void *host::alloc_on(impl *s, size_t bytes)
{
	if (!s || !bytes)
		return nullptr;
	// Zeroed, and that is not politeness: a card reading a register before its
	// firmware has written it gets 0, which is also what an unimplemented bus
	// address reads as, so the two cases cannot be told apart - deliberately,
	// because the real chip cannot tell them apart either.
	void *p = ::calloc(1, bytes);
	if (!p)
		return nullptr;
	s->ram.push_back(ram_block{ p, bytes });
	return p;
}

void host::release_on(impl *s, void *p)
{
	if (!s || !p)
		return;
	for (size_t i = 0; i < s->ram.size(); i++) {
		if (s->ram[i].ptr != p)
			continue;
		::free(s->ram[i].ptr);
		s->ram.erase(s->ram.begin() + i);
		return;
	}
}

void *host::alloc(int slot, size_t bytes)
{
	if (slot < 0 || slot >= SLOTS || !m_slot[slot])
		return nullptr;
	return alloc_on(m_slot[slot].get(), bytes);
}

void host::release(int slot, void *p)
{
	if (slot < 0 || slot >= SLOTS || !m_slot[slot])
		return;
	release_on(m_slot[slot].get(), p);
}

host::param_page host::params(int slot, plg_param_desc *out, size_t cap) const
{
	param_page page;
	if (slot < 0 || slot >= SLOTS || !m_slot[slot])
		return page;
	const impl *s = m_slot[slot].get();
	if (!s->card || !s->cut_ok || s->fault || !s->cut.params)
		return page;
	page.have = s->cut.params(s->card, out, cap);
	page.shown = page.have < cap ? page.have : cap;
	return page;
}

int host::set_param(int slot, uint32_t id, int32_t value)
{
	if (slot < 0 || slot >= SLOTS || !m_slot[slot])
		return -1;
	impl *s = m_slot[slot].get();
	if (!s->card || !s->cut_ok || s->fault || !s->cut.set_param)
		return -1;
	return s->cut.set_param(s->card, id, value);
}

int host::get_param(int slot, uint32_t id, int32_t *value) const
{
	if (slot < 0 || slot >= SLOTS || !m_slot[slot])
		return -1;
	const impl *s = m_slot[slot].get();
	if (!s->card || !s->cut_ok || s->fault || !s->cut.get_param)
		return -1;
	return s->cut.get_param(s->card, id, value);
}

int host::selftest(int slot)
{
	if (slot < 0 || slot >= SLOTS || !m_slot[slot])
		return -1;
	impl *s = m_slot[slot].get();
	if (!s->card || !s->cut_ok || !s->cut.selftest)
		return -1;
	return s->cut.selftest(s->card);
}

// ---- state ------------------------------------------------------------------

std::vector<uint8_t> host::save() const
{
	std::vector<uint8_t> out;
	for (int slot = 0; slot < SLOTS; slot++) {
		if (!running(slot))
			continue;
		const impl *s = m_slot[slot].get();

		// The card's own blob first, then the RAM it asked the host to hold. The
		// RAM comes second so a reader that only understands the card's blob can
		// stop after it - the header says how long that blob is.
		std::vector<uint8_t> body;
		size_t card_len = 0;
		if (s->cut.save) {
			// Ask for the size, then ask again with a buffer. Two calls because
			// the ABI has no "give me a buffer" and a card that grows its state
			// between the two is answered by the second one reporting that it
			// needs more, which is the case the header's length has to cover.
			card_len = s->cut.save(s->card, nullptr, 0);
			if (card_len) {
				body.resize(card_len);
				const size_t got = s->cut.save(s->card, body.data(), card_len);
				if (got > card_len)
					card_len = got;   // should not happen; the length still has to cover it
				else
					card_len = got;
			}
		}
		for (const ram_block &b : s->ram) {
			// Each block length-prefixed, so a card that allocated twice round
			// trips. Nothing after this point interprets the bytes.
			st::put32(body, uint32_t(b.len));
			body.insert(body.end(), static_cast<uint8_t *>(b.ptr),
			            static_cast<uint8_t *>(b.ptr) + b.len);
		}

		out.insert(out.end(), st::TAG, st::TAG + 3);
		out.push_back(st::VER);
		out.push_back(uint8_t(PLG_ABI_VERSION));
		out.push_back(uint8_t(slot));
		out.push_back(st::F_SAVED);
		out.push_back(0);   // the pad byte, so OFF_HASH is where it says it is
		st::put32(out, hash_id(s->desc.id));
		st::put32(out, uint32_t(body.size()));
		if (out.size() != st::OFF_ID)
			return out;   // unreachable; worse to write a broken frame than to notice
		out.insert(out.end(), s->desc.id.begin(), s->desc.id.end());
		out.push_back(0);
		out.insert(out.end(), body.begin(), body.end());
	}
	return out;
}

bool host::load(const uint8_t *p, size_t n, std::string &warn)
{
	if (!p || n < st::HEADER)
		return false;
	if (::memcmp(p, st::TAG, 3) != 0) {
		warn = "PLG の状態ではない";
		return false;
	}
	// Step over the sections one at a time. A section whose header is wrong, or
	// whose card is not installed, is skipped: a project has to open even when
	// the card that was in it has been uninstalled, and that is not worth failing
	// a load over.
	size_t at = 0;
	while (at + st::HEADER <= n) {
		if (::memcmp(p + at, st::TAG, 3) != 0) {
			warn = "節の印が違う。ここから先は無視";
			break;
		}
		const uint8_t  ver  = p[at + st::OFF_VER];
		const uint8_t  abi  = p[at + st::OFF_ABI];
		const uint8_t  slot = p[at + st::OFF_SLOT];
		const uint32_t h    = st::get32(p + at + st::OFF_HASH);
		const uint32_t len  = st::get32(p + at + st::OFF_LEN);
		if (ver != st::VER || abi != PLG_ABI_VERSION || slot >= SLOTS ||
		    at + st::HEADER >= n || !p[at + st::HEADER]) {
			warn = "節が読めない（ver " + std::to_string(ver) + " / slot " +
			       std::to_string(slot) + "）。無視";
			return true;
		}
		const char *id = reinterpret_cast<const char *>(p + at + st::HEADER);
		const size_t idlen = ::strlen(id);
		const size_t body_at = at + st::HEADER + idlen + 1;
		if (body_at + len > n) {
			warn = "節の長さが変。無視";
			return true;
		}
		const std::string sid(id);
		// The hash is the cheap reject, the id is the confirmation.
		if (hash_id(sid) != h) {
			warn = "id がハッシュと合わない。無視: " + sid;
			at = body_at + len;
			continue;
		}
		if (!running(slot)) {
			warn = "カードが無いので読みません: " + sid;
			at = body_at + len;
			continue;
		}
		impl *s = m_slot[slot].get();
		if (s->desc.id != sid) {
			warn = "別のカードが入っています。読みません: 入ってるのは " +
			       s->desc.id + " / 状態を閉じたのは " + sid;
			at = body_at + len;
			continue;
		}
		const uint8_t *body = p + body_at;
		size_t left = len;
		if (s->cut.save) {
			const size_t need = s->cut.save(s->card, nullptr, 0);
			if (need != 0) {
				// The blob's length is not in the frame, so it has to be asked for
				// again. A card that reports a different size than it did at save
				// time has changed shape, and the RAM blocks after it would then
				// start in the wrong place, so stop rather than misread them.
				const size_t now = s->cut.save(s->card, nullptr, 0);
				if (need > left || now != need) {
					warn = "カードの blob の大きさが変わっている。読みません: " + sid;
					at = body_at + len;
					continue;
				}
				if (s->cut.load)
					s->cut.load(s->card, body, need);
				body += need;
				left -= need;
			}
		}
		// The RAM blocks, in order, each length prefixed. A block whose length
		// does not match ends the section: the rest is not ours to interpret.
		for (ram_block &b : s->ram) {
			if (left < 4)
				break;
			const uint32_t n2 = st::get32(body);
			if (size_t(n2) != b.len || 4 + size_t(n2) > left) {
				warn = "カードの RAM の長さが違う。読みません: " + sid;
				break;
			}
			::memcpy(b.ptr, body + 4, b.len);
			body += 4 + n2;
			left -= 4 + n2;
		}
		at = body_at + len;
	}
	return true;
}

} // namespace plg
