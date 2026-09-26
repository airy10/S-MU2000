// license:BSD-3-Clause
//
// plg/host.h - the three PLG slots, and the rules that keep a module from
// breaking the machine.
//
// This is the loading, lifetime, state and safety half of doc/plg-cards.md. The
// audio half is not here yet: nothing calls run() from mu2000::run_sample()
// until the connector exists. What is here is testable on its own, which is why
// src/plgtest.cpp drives all of it without a ROM.
//
// The shape follows src/vst3/engine.{h,cpp} rather than the MAME device model,
// because that is where the host already keeps a machine: a status enum, a
// message() for the one line the screen shows, save_state() as a byte run, and
// load_state() that falls back instead of failing.
#ifndef S_MU2000_PLG_HOST_H
#define S_MU2000_PLG_HOST_H

#include "plg/plg1500.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace smu2000 {
class dynlib;
}

namespace plg {

// One PLG slot holds at most one module. Three of them, because the MU2000 has
// three connectors (src/mame/ymmu2000.cpp:407-425) and the panel has three
// lamps (src/ui/panel.cpp:794).
class host {
public:
	static constexpr int SLOTS = 3;
	static constexpr int IN_CH = 2;    // plg_slot_io::in
	static constexpr int OUT_CH = 2;   // plg_slot_io::out

	// Both of these are here rather than defaulted inline because impl is only
	// forward declared: a unique_ptr cannot be destroyed without seeing it.
	host();
	~host();
	host(const host &) = delete;
	host &operator=(const host &) = delete;

	// ---- lifetime ----------------------------------------------------------
	//
	// insert() and eject() are **not** audio-thread calls. The caller holds the
	// machine lock, the way src/vst3/engine.h:191-194 does it for SmartMedia.
	// Inserting reads the descriptor first and refuses on a bad abi, so a card
	// built against another version cannot get far enough to misbehave.
	bool insert(int slot, const std::string &path, std::string &err);
	void eject(int slot);

	// A slot can be occupied by a module that failed to start - the library
	// loaded, the descriptor was fine, the card's own create() said no. That is
	// worth showing rather than pretending the slot is empty, so `present` means
	// "something is in there" and `running` means "the card will answer".
	bool     present(int slot) const;
	bool     running(int slot) const;
	// The descriptor, or nullptr. Valid until the card is ejected. Owned by the
	// module, so read it and copy out what you need.
	const plg_card_info *info(int slot) const;
	// Why a slot that is present is not running, or has stopped. Empty when
	// there is nothing wrong. One line, for the screen and the settings file.
	const std::string &message(int slot) const;
	// A card the host gave up on: too slow, or it said it was broken. A faulted
	// card is skipped rather than called, so one bad card cannot stutter the
	// machine forever. Clearing it is eject() and insert() again.
	bool     faulted(int slot) const;

	// ---- audio -------------------------------------------------------------
	//
	// One sample, on the SWP30's melo scale. `in` is what the slave sends
	// (melo 18, 19), `out` is where the card's return goes (meli 10..15, two per
	// slot). With nothing in the slot this zeroes out[] and returns, so the
	// caller needs no branch of its own.
	//
	// This is the one call that may be made from the audio thread, and it is the
	// reason for the budget below.
	void run(int slot, const int32_t *in, int32_t *out);

	// ---- MIDI --------------------------------------------------------------
	//
	// Two directions, both edge-driven, because that is what the wire is.
	//
	// midi_rx() is the host pushing SCI4's TX line into the card. The chip
	// generates those edges itself from its divisor, so this is exact.
	// midi_tx() is the card's own line, polled at the bit times; a card that
	// wants to be edge-driven instead calls host->tx(), which lands in tx_sink.
	//
	// **A card cannot yet schedule its own TX in time.** SCI4 samples a bit every
	// few microseconds and an audio sample is 22.7 of them, so a card that emits
	// a whole byte from run() has all ten edges inside one instant and the chip
	// sees framing errors. That is a known gap, not a design: doc/plg-cards.md
	// section 4. Until it is closed, a v1 card is useful for receiving and for
	// proving the plumbing, not for being received.
	void midi_rx(int slot, int level);
	int  midi_tx(int slot) const;

	// Where a card's own TX line goes. The machine points this at SCI4's RX.
	using tx_sink = std::function<void(int slot, int level)>;
	void set_tx_sink(tx_sink f) { m_tx_sink = std::move(f); }

	// ---- state -------------------------------------------------------------
	//
	// save() returns the three slots' sections concatenated, or nothing at all
	// when no slot holds a card that can save. A project saved with no cards is
	// byte-identical to one saved before this existed, which is the point: an
	// empty section must not appear just because the code can write one.
	std::vector<uint8_t> save() const;

	// load() reads every section it recognises. A section is **skipped, never
	// fatal**: a project naming a card that is not installed still has to open,
	// with that slot silent. `warn` collects one line per skip, for the log.
	// Returns false only when the bytes are not a PLG state at all.
	bool load(const uint8_t *p, size_t n, std::string &warn);

	// ---- what a card can ask the host for ----------------------------------

	// A card's ROM, as the user dumped it. `part` is the module's numbering.
	// The host has no idea what any part means; it hands back the n-th file the
	// user listed for this slot, or nothing.
	using rom_fn = std::function<const uint8_t *(uint32_t part, uint32_t *len)>;
	void set_rom_source(int slot, rom_fn f) { m_rom[slot] = std::move(f); }

	// Where a card's log lines go. Left unset they are dropped, because a card
	// that logs every sample would be a card nobody can use.
	using log_fn = std::function<void(const std::string &)>;
	void set_log_sink(log_fn f) { m_log = std::move(f); }
	// Cards that are PLG_F_HEAVY report here.
	using load_fn = std::function<void(int slot, double pct)>;
	void set_load_sink(load_fn f) { m_load = std::move(f); }
	// Cards reporting a parameter they moved themselves.
	using param_fn = std::function<void(int slot, uint32_t id, int32_t value)>;
	void set_param_sink(param_fn f) { m_param = std::move(f); }

	// The RAM a card may have the host write into. Freed when the card is
	// ejected, and included in the card's state, so a project that saves while
	// the card is running comes back with the card's RAM intact.
	void *alloc(int slot, size_t bytes);
	void  release(int slot, void *p);

	// ---- parameters, for a screen ------------------------------------------
	//
	// Straight through to the card. Empty when there is no card, which is what
	// a screen wants: no card, no page. params() returns how many descriptors the
	// card *has*, which can be more than were asked for; `shown` says how many
	// actually landed in the caller's array.
	struct param_page {
		size_t have = 0;   // what the card says it has
		size_t shown = 0;   // what fitted
	};
	param_page params(int slot, plg_param_desc *out, size_t cap) const;
	int        set_param(int slot, uint32_t id, int32_t value);
	int        get_param(int slot, uint32_t id, int32_t *value) const;

	// ---- one-off checks ----------------------------------------------------

	// The card's own selftest, with nothing else running. Returns what the card
	// returned, or a negative number when there is no card to ask.
	int selftest(int slot);

	// Read a descriptor without inserting anything. This is what a file browser
	// calls, so it must be safe on a file that is not a card at all, and the two
	// strings it hands back are copies that live until the next call on the same
	// thread.
	static bool probe(const std::string &path, plg_card_info &out, std::string &err);

private:
	struct impl;

	// The three things a card reaches through its plg_host. impl holds a
	// back pointer to the host for exactly this, because the service table is
	// given the impl as its ctx and a card may call these from inside create().
	// That is also why they take an impl and not a slot number: during create()
	// the slot is not in m_slot yet, so a slot lookup would find nothing.
	const uint8_t *rom_at(int slot, uint32_t part, uint32_t *len);
	void  *alloc_on(impl *s, size_t bytes);
	void   release_on(impl *s, void *p);
	uint64_t clock_now() const { return m_clock; }

	// What one slot holds. An impl array rather than fields per slot, so slot
	// numbers stay indices and the three never drift apart.
	std::unique_ptr<impl> m_slot[SLOTS];
	rom_fn                 m_rom[SLOTS];
	tx_sink                m_tx_sink;
	log_fn                 m_log;
	load_fn                m_load;
	param_fn               m_param;

	// How long one card's run() may take, in microseconds, before the host stops
	// trusting it. A 44100 Hz sample is 22.7us and the whole emulator is at
	// about 22% of a core with 16 parts sounding (README), so 8us leaves a card
	// real work to do and still leaves headroom. SMU2000_PLG_BUDGET_US moves it
	// for experiments.
	static double budget_us();
	// Consecutive slow samples before giving up on a card. One slow sample is a
	// cache miss or a page fault; a hundred in a row is a card that cannot
	// keep up, and calling it again only makes the next block worse.
	static constexpr int SLOW_RUNS_BEFORE_FAULT = 100;

	// The 44100 Hz counter the cards see. Incremented once per run() on whichever
	// slot is running, so a card cannot keep its own idea of the time and drift
	// from the SWP30 it is sharing a sample with.
	std::atomic<uint64_t> m_clock{0};
};

} // namespace plg

#endif // S_MU2000_PLG_HOST_H
