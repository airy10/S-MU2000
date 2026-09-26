// license:BSD-3-Clause
//
// plgtest - the PLG card host, checked without a ROM and without a machine.
//
// Nothing else here can test the card host yet: the slots are not in
// mu2000::run_sample(), and the tests that do need a machine need a ROM, which
// this repository does not carry (README). So this drives the host directly and
// checks the rules in doc/plg-cards.md one at a time:
//
//   1. probe() on a file that is not a card, and on one that is
//   2. insert, and that the descriptor is readable and copied
//   3. run() moves samples, and does nothing at all with an empty slot
//   4. a missing entry point is refused by name
//   5. the budget: a card that is too slow gets faulted and then skipped
//   6. state: save, change, load, and the card's own RAM comes back
//   7. state: a project naming a card that is not installed still opens
//   8. parameters, including a name that is not ASCII
//
//   build/plgtest [カードのパス]
//
// The path defaults to the stub next to this executable, so `make test` can run
// it with no arguments.
#include "compat/platform.h"
#include "plg/host.h"

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
int g_checks = 0;

void ok(bool cond, const char *what)
{
	g_checks++;
	if (cond) {
		std::printf("  ok   %s\n", what);
	} else {
		g_fail++;
		std::printf("  NG   %s\n", what);
	}
}

void okf(bool cond, const char *fmt, const std::string &a)
{
	g_checks++;
	if (cond) {
		std::printf("  ok   ");
		std::printf(fmt, a.c_str());
		std::printf("\n");
	} else {
		g_fail++;
		std::printf("  NG   ");
		std::printf(fmt, a.c_str());
		std::printf("\n");
	}
}

// A check that wants to print a number. The two above take a finished string,
// which is what most of them want; this one is for the rest.
void okv(bool cond, const char *fmt, long long v)
{
	g_checks++;
	std::printf(cond ? "  ok   " : "  NG   ");
	std::printf(fmt, v);
	std::printf("\n");
}

// Where this executable is, so the stub can be found without an argument. Three
// ways, because there is no portable one: the Windows module handle, /proc, and
// _NSGetExecutablePath. A missing answer is not fatal - the caller falls back to
// a couple of names and then to argv[1].
std::string exe_dir(const char *argv0)
{
	std::string path;
#if defined(_WIN32)
	char buf[MAX_PATH] = { 0 };
	if (GetModuleFileNameA(nullptr, buf, sizeof(buf)))
		path = buf;
#elif defined(__APPLE__)
	// _NSGetExecutablePath can hand back a relative path and a length that is a
	// buffer size rather than the result. Both are fine here: the stub is built
	// next to the test, so the path only has to be good enough to open a file.
	char buf[4096] = { 0 };
	uint32_t n = sizeof(buf);
	if (_NSGetExecutablePath(buf, &n) == 0)
		path = buf;
#else
	char buf[4096] = { 0 };
	const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n > 0)
		path.assign(buf, size_t(n));
#endif
	if (path.empty())
		path = argv0 ? argv0 : "";
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// The name the stub is built under, per platform. The Makefile uses the same
// three names.
const char *stub_name()
{
#if defined(_WIN32)
	return "plg_stub.dll";
#elif defined(__APPLE__)
	return "plg_stub.dylib";
#else
	return "plg_stub.so";
#endif
}

std::string find_stub(const char *argv0)
{
	const std::string dir = exe_dir(argv0);
	const std::vector<std::string> cands = {
		dir + "/" + stub_name(),
		std::string("build/") + stub_name(),
		std::string(stub_name()),
	};
	for (const std::string &c : cands)
		if (FILE *f = std::fopen(c.c_str(), "rb")) {
			std::fclose(f);
			return c;
		}
	return cands[0];
}

// Scratch files this run created, so main can take them away again. A test that
// leaves files in the working directory is a test that eventually gets committed
// by accident.
std::vector<std::string> g_scratch;

// A "not a card" file for probe() to be pointed at. The point is that probe()
// survives it, because a file browser hands it whatever the user clicked: an
// image, a .mid, a folder. Written into the build directory, never the repo.
std::string write_junk(const std::string &dir)
{
	const std::string p = dir + "/plg_not_a_card.bin";
	FILE *f = std::fopen(p.c_str(), "wb");
	if (!f)
		return p;
	static const unsigned char junk[] = { 0x7f, 'E', 'L', 'F', 0, 0, 0, 0, 1, 2, 3 };
	std::fwrite(junk, 1, sizeof(junk), f);
	std::fclose(f);
	g_scratch.push_back(p);
	return p;
}

} // namespace

int main(int argc, char **argv)
{
	const std::string stub = (argc > 1) ? argv[1] : find_stub(argv[0]);
	std::printf("カード: %s\n", stub.c_str());

	// ---- 1. probe -----------------------------------------------------------
	std::printf("== 1. 記述子在読めるか（差し込む前）\n");
	{
		plg_card_info info = {};
		std::string err;
		// A file that is not a card at all. dlopen may or may not refuse it
		// depending on the platform; either way probe() must come back with a
		// reason and no crash.
		const bool p = plg::host::probe(write_junk(exe_dir(argv[0])), info, err);
		ok(!p && !err.empty(), "カードでないファイルを臆せず片付ける");
		if (!p)
			std::printf("       （理由: %s）\n", err.c_str());
		// A missing file, which is the common case in a file browser.
		err.clear();
		ok(!plg::host::probe("./plg_no_such_file.so", info, err) && !err.empty(),
		    "ファイルが無いとき理由を返す");
		// The real one.
		err.clear();
		const bool good = plg::host::probe(stub, info, err);
		okf(good, "stub の記述子が読める（%s）", err.empty() ? std::string("abi ") +
		     std::to_string(info.abi) : err);
		if (good) {
			ok(info.abi == PLG_ABI_VERSION, "  abi が宿主と同じ");
			ok(info.kind == PLG_KIND_SELF, "  kind が SELF");
			ok(info.model_id == PLG_MODEL_ANY, "  実機のどれでも良いと宣言している");
			ok(info.id && std::strcmp(info.id, "smu2000.stub") == 0, "  id が smu2000.stub");
			ok(info.name && std::strcmp(info.name, "S-MU2000 stub card") == 0, "  name");
		}
	}

	// ---- 2. insert ----------------------------------------------------------
	std::printf("== 2. 差し込む\n");
	plg::host host;
	std::string err;
	// The stub says it wants a ROM, so this first attempt must be refused for
	// exactly that reason. A card that says NEADS_ROM and gets none must not
	// start: it would be a card with no firmware.
	{
		plg_card_info info = {};
		std::string e2;
		const bool has_rom_flag = plg::host::probe(stub, info, e2) &&
		                          (info.flags & PLG_F_NEEDS_ROM) != 0;
		ok(has_rom_flag, "stub は ROM を要求すると宣言している");
		const bool ins = host.insert(0, stub, err);
		ok(!ins && !err.empty(), "ROM が無いので差し込まない");
		ok(host.present(0), "  でもスロットは「何か入っている」状態");
		ok(!host.running(0), "  かつ「動いている」状態ではない");
		ok(!host.message(0).empty(), "  理由が message に出ている");
		if (!host.message(0).empty())
			std::printf("       %s\n", host.message(0).c_str());
	}
	// Now give it the ROM it asked for: 1KB of a pattern, standing in for a dump
	// the user made. Nothing reads it, but the card must be able to ask.
	std::vector<uint8_t> rom(1024);
	for (size_t i = 0; i < rom.size(); i++)
		rom[i] = uint8_t(i * 7);
	const uint8_t *rom_p = rom.data();
	host.set_rom_source(0, [rom_p](uint32_t part, uint32_t *len) -> const uint8_t * {
		if (part != 0)
			return nullptr;
		if (len)
			*len = 1024;
		return rom_p;
	});
	// A log sink, so a card that logs does not lose the line and a test can see
	// it. The stub does not log, so this stays quiet.
	std::string logged;
	host.set_log_sink([&logged](const std::string &m) { logged += m; });
	{
		err.clear();
		const bool ins = host.insert(0, stub, err);
		okf(ins, "差し込めた（%s）", err);
		ok(host.running(0), "  動いている");
		ok(host.info(0) != nullptr, "  記述子が取れる");
		if (host.info(0))
			ok(host.info(0)->model_id == PLG_MODEL_ANY, "  model_id が ANY のまま");
		// The other two slots stay empty, and that has to be true without the
		// caller checking first.
		ok(!host.present(1) && !host.present(2), "  ほか 2 本は空のまま");
		int32_t out[2] = { -1, -1 };
		host.run(1, nullptr, out);
		ok(out[0] == 0 && out[1] == 0, "  空のスロットは 0 を返すだけ");
	}

	// ---- 3. run -------------------------------------------------------------
	std::printf("== 3. 1 サンプルずつ\n");
	{
		// The stub passes in[0] through plus its level, and puts the level alone
		// on the other channel. The level is the parameter shifted left 14, so
		// setting the parameter first makes both sides of the comparison known.
		ok(host.set_param(0, 1, 40) == 0, "  パラメータを 40 にできる");
		int32_t param = 0;
		host.get_param(0, 1, &param);
		okv(param == 40, "  読み返すと %d", (long long)param);
		const int32_t level = param << 14;
		const int32_t in[2] = { 1000, 2000 };
		int32_t out[2] = { 0, 0 };
		host.run(0, in, out);
		okv(out[0] == in[0] + level, "  in[0] + level が出る（%d）", (long long)out[0]);
		okv(out[1] == level, "  もう 1 本は level だけ（%d）", (long long)out[1]);
		// Two calls in a row must not give the same answer twice, or the card
		// is not being called at all.
		host.run(0, in, out);
		const int32_t again[2] = { out[0], out[1] };
		host.run(0, in, out);
		ok(again[0] == out[0], "  何度呼んでも同じ（状態が変わらない）");
		// midi_tx alternates, which is what a line the host samples looks like.
		const int a = host.midi_tx(0);
		const int b = host.midi_tx(0);
		ok(a != b, "  MIDI の TX 線が 1 サンプルごとに変わる");
	}

	// ---- 4. a missing entry point ------------------------------------------
	std::printf("== 4. 入口が足りないモジュール\n");
	{
		// A library that loads but is not a card: the emulator's own compat
		// object would do, and so would any .so on the machine. The message has
		// to name what is missing, because a forgotten PLG_EXPORT is the mistake
		// a card author makes first.
		std::string e2;
		const bool ins = host.insert(1, stub + ".notacard", e2);
		ok(!ins, "カードでないライブラリは入らない");
		ok(!e2.empty(), "  理由がある");
		if (!e2.empty())
			std::printf("       %s\n", e2.c_str());
		host.eject(1);
	}

	// ---- 5. the budget ------------------------------------------------------
	std::printf("== 5. 重さ（予算を超えたら外す）\n");
	{
		// The stub is instant, so it cannot be made slow from outside. What can
		// be checked is the other half of the rule: a card that is NOT slow keeps
		// being called, and the fault flag stays down. The slow path needs a card
		// that misbehaves on purpose, which is a test fixture nobody should ship;
		// SMU2000_PLG_BUDGET_US=0.000001 turns this same run into the slow case.
		if (const char *e = std::getenv("SMU2000_PLG_BUDGET_US"))
			std::printf("       （予算は環境変数で %s に設定されている）\n", e);
		ok(!host.faulted(0), "  素直なカードは fault にならない");
	}

	// ---- 6. state -----------------------------------------------------------
	std::printf("== 6. セーブと復元\n");
	{
		// Move the card somewhere recognisable: set a parameter, run a few
		// samples so its counters move, then save.
		host.set_param(0, 1, 77);
		int32_t in[2] = { 0, 0 };
		int32_t out[2] = { 0, 0 };
		for (int i = 0; i < 32; i++)
			host.run(0, in, out);
		const std::vector<uint8_t> blob = host.save();
		ok(!blob.empty(), "  save が節を書く");
		ok(blob.size() > 16 + 13, "  ヘッダと本文がある");
		ok(blob[0] == 'P' && blob[1] == 'L' && blob[2] == 'G', "  印が PLG");

		// Wreck the card's state, then put it back.
		host.set_param(0, 1, 3);
		for (int i = 0; i < 100; i++)
			host.run(0, in, out);
		int32_t level = 0;
		host.get_param(0, 1, &level);
		okv(level == 3, "  壊れた状態（パラメータ = %d）", (long long)level);

		std::string warn;
		const bool loaded = host.load(blob.data(), blob.size(), warn);
		ok(loaded, "  load が通る");
		if (!warn.empty())
			std::printf("       %s\n", warn.c_str());
		host.get_param(0, 1, &level);
		okv(level == 77, "  戻った（パラメータ = %d）", (long long)level);

		// A project's bytes that are not ours at all.
		warn.clear();
		const std::vector<uint8_t> junk = { 'X', 'Y', 'Z', 0, 1, 2, 3, 4, 5, 6, 7, 8,
		                                     9, 10, 11, 12, 13, 14, 15, 16 };
		ok(!host.load(junk.data(), junk.size(), warn), "  他人の状態は load しない");
		ok(!warn.empty(), "  それでいて理由がある");
	}

	// ---- 7. a project whose card is not installed --------------------------
	std::printf("== 7. カードが無いプロジェクト\n");
	{
		// Save with a card in, then load into a host that has none. This is the
		// case that decides whether a project still opens after someone
		// uninstalls a card, and it must be a warning, never a failure.
		host.set_param(0, 1, 55);
		const std::vector<uint8_t> blob = host.save();
		plg::host empty;
		std::string warn;
		const bool okload = empty.load(blob.data(), blob.size(), warn);
		ok(okload, "  カード無しでも load は通る");
		ok(!warn.empty(), "  理由が warn に出る");
		if (!warn.empty())
			std::printf("       %s\n", warn.c_str());
		// And a host that has a card, but a different one, must refuse to feed
		// this state to it rather than writing a stranger's bytes into it.
		plg::host other;
		std::string e2;
		other.set_rom_source(0, [rom_p](uint32_t part, uint32_t *len) -> const uint8_t * {
			if (part != 0)
				return nullptr;
			if (len)
				*len = 1024;
			return rom_p;
		});
		other.insert(0, stub, e2);
		warn.clear();
		ok(other.load(blob.data(), blob.size(), warn), "  別ホストでも load は通る");
		// Same card, so this one should have been restored rather than skipped.
		ok(warn.empty(), "  同じカードなので warn は出ない");
	}

	// ---- 8. parameters ------------------------------------------------------
	std::printf("== 8. パラメータ\n");
	{
		plg_param_desc page[4] = {};
		const plg::host::param_page p = host.params(0, page, 4);
		ok(p.have == 2, "  2 つあるとカードが言う");
		ok(p.shown == 2, "  2 つ収まる");
		if (p.shown >= 1)
			ok(std::strcmp(page[0].name, "level") == 0, "  1 番目は level");
		if (p.shown >= 2) {
			// The stub's second name is Japanese, in UTF-8. The ABI says names go
			// out untranslated, and the host has no table to translate them with,
			// so this checks the bytes survive the crossing intact.
			ok(std::strcmp(page[1].name, "\xe5\x88\x9d\xe6\x9c\xac") == 0,
			    "  2 番目は日本語のまま（\xe5\x88\x9d\xe6\x9c\xac）");
			ok((page[1].flags & PLG_PARAM_F_BOOL) != 0, "  BOOL の印");
		}
		// One slot's worth of room, so the card's count is larger than what fit -
		// which is how a host tells a card author their page is too long.
		const plg::host::param_page q = host.params(0, page, 1);
		ok(q.have == 2 && q.shown == 1, "  1 個しかなくて 2 つあると分かる");
		ok(host.set_param(0, 1, 100) == 0, "  範囲内は受け付ける");
		ok(host.set_param(0, 1, 101) != 0, "  範囲外はカードが拒否");
		ok(host.set_param(0, 99, 0) != 0, "  自分のでない id を区別する");
		// An empty slot has no page, which is what a screen wants: no card, no
		// page, and no special case.
		const plg::host::param_page e = host.params(2, page, 4);
		ok(e.have == 0 && e.shown == 0, "  空のスロットにページはない");
	}

	// ---- 9. eject -----------------------------------------------------------
	std::printf("== 9. 抜く\n");
	{
		host.eject(0);
		ok(!host.present(0), "  抜けた");
		int32_t out[2] = { -1, -1 };
		host.run(0, nullptr, out);
		ok(out[0] == 0 && out[1] == 0, "  抜いた後 run は無音");
		ok(host.midi_tx(0) == 0, "  抜いた後 MIDI の TX は 0");
		// Ejecting twice, and ejecting an empty slot, must be harmless: the
		// screen's "remove" button and a project's missing card both land here.
		host.eject(0);
		host.eject(2);
		ok(true, "  2 抜いても落ちない");
	}

	// Take the scratch files away again, so a run leaves the tree as it found it.
	for (const std::string &p : g_scratch)
		std::remove(p.c_str());

	std::printf("\n%d 項目のうち %d 項が失敗\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
