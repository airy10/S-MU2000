// license:BSD-3-Clause
//
// 起動の確認。ROM を読ませて CPU を走らせ、どこまで行くかを見る。
//
//   boot <rom ディレクトリ> [サイクル数] [--trace-swp <出力先>]
//                                  [--trace-sci4 <出力先>] [--lcd-dump <出力先>]
//
// rom ディレクトリには MU2000 リポジトリの roms/ をそのまま渡せる。

#include "mu2000.h"
#include "plg/host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>


int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr,
			"使い方: boot <rom ディレクトリ> [サイクル数] [--trace-swp <出力先>] [-v]\n"
			"       [--trace-sci4 <出力先>] [--lcd-dump <出力先>]\n"
			"  --trace-sci4  0xf00000（PLG ボード用 SCI4）の読み書きを 1 行ずつ書く\n"
			"  --lcd-dump    液晶の 20x4 を 4 行で出す。PLG の画面文字列もここに出る\n");
		return 1;
	}

	const std::string dir = argv[1];
	u64 cycles = 28000000;               // 既定で 1 秒ぶん
	const char *trace = nullptr;
	bool with_reads = false;
	const char *pctrace = nullptr;
	const char *pchash = nullptr;
	const char *porttrace = nullptr;
	const char *updtrace = nullptr;
	const char *sci4trace = nullptr;
	const char *lcddump = nullptr;
	const char *plgcard = nullptr;
	u64 pcskip = 0;
	u64 pccount = 2000000;

	for (int i = 2; i < argc; i++) {
		if (!std::strcmp(argv[i], "--trace-swp") && i + 1 < argc)
			trace = argv[++i];
		else if (!std::strcmp(argv[i], "--trace-sci4") && i + 1 < argc)
			sci4trace = argv[++i];
		else if (!std::strcmp(argv[i], "--lcd-dump") && i + 1 < argc)
			lcddump = argv[++i];
		else if (!std::strcmp(argv[i], "--plg") && i + 1 < argc)
			plgcard = argv[++i];
		else if (!std::strcmp(argv[i], "--trace-pc") && i + 1 < argc)
			pctrace = argv[++i];
		else if (!std::strcmp(argv[i], "--hash-pc") && i + 1 < argc)
			pchash = argv[++i];
		else if (!std::strcmp(argv[i], "--trace-upd") && i + 1 < argc)
			updtrace = argv[++i];
		else if (!std::strcmp(argv[i], "--trace-port") && i + 1 < argc)
			porttrace = argv[++i];
		else if (!std::strcmp(argv[i], "--pc-skip") && i + 1 < argc)
			pcskip = std::strtoull(argv[++i], nullptr, 0);
		else if (!std::strcmp(argv[i], "--pc-count") && i + 1 < argc)
			pccount = std::strtoull(argv[++i], nullptr, 0);
		else if (!std::strcmp(argv[i], "--reads"))
			with_reads = true;
		else if (!std::strcmp(argv[i], "-v"))
			smu2000::g_verbose = true;
		else
			cycles = std::strtoull(argv[i], nullptr, 0);
	}

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str());
		return 1;
	}
	if (!mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str());
		return 1;
	}
	if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
		std::fprintf(stderr, "警告: %s\n", mu.error().c_str());

	std::FILE *tf = nullptr;
	if (trace) {
		tf = std::fopen(trace, "w");
		if (!tf) {
			std::fprintf(stderr, "書けない: %s\n", trace);
			return 1;
		}
		mu.set_swp_trace(tf, with_reads);
	}

	// SCI4 の記録。ボードを挿していないので、これが唯一の_protocol 証拠:
	// firmware が何を送り、何を待ってから諦めるか（doc/plg-cards.md 4）。
	std::FILE *sf = nullptr;
	if (sci4trace) {
		sf = std::fopen(sci4trace, "w");
		if (!sf) {
			std::fprintf(stderr, "書けない: %s\n", sci4trace);
			return 1;
		}
		mu.set_sci4_trace(sf);
	}

	mu.reset();

	std::FILE *pf = nullptr, *hf = nullptr;
	if (pctrace) {
		pf = std::fopen(pctrace, "w");
		smu2000::g_pc_trace = pf;
		smu2000::g_pc_trace_left = pccount;
		smu2000::g_pc_skip = pcskip;
	}
	if (updtrace)
		smu2000::g_upd_trace = std::fopen(updtrace, "w");
	if (porttrace)
		smu2000::g_port_trace = std::fopen(porttrace, "w");
	if (pchash) {
		hf = std::fopen(pchash, "w");
		smu2000::g_pc_hash = hf;
	}
	// PLG カードを入れる（doc/plg-cards.md 5）。**機械を立ち上げる前**に
	// 入れる。カードの読み込みは機械の錠を取り中の別の糸ですることが約束で、
	// boot には糸が 1 本しかないので、立ち上がり前にやってしまうのが素直。
	plg::host cards;
	if (plgcard) {
		std::string cerr;
		mu.set_plg_host(&cards);
		cards.set_log_sink([](const std::string &m) { std::printf("[card] %s\n", m.c_str()); });
		const int slot = mu.plg_slot_for(PLG_MODEL_ANY);
		if (!cards.insert(slot, plgcard, cerr))
			std::fprintf(stderr, "カードを入れられない: %s\n", cerr.c_str());
		else
			std::printf("PLG スロット %d に %s\n", slot + 1, plgcard);
	}

	std::printf("リセット後  PC=%08x\n", mu.cpu().pc());

	// 少しずつ走らせて、進んでいるか見る
	const u64 step = cycles / 10 ? cycles / 10 : cycles;
	for (u64 done = 0; done < cycles; done += step) {
		mu.run_cycles(step);
		std::printf("%10llu サイクル  PC=%08x\n",
		            (unsigned long long)mu.cpu().total_cycles(), mu.cpu().pc());
	}

	// 記録の有無い unconditionally 无关に数える。0 なら、firmware は
	// 起動中に SCI4 を 1 回も触っていないということ（doc/plg-cards.md 4）。
	std::printf("SCI4 0xf00000 への読み書き: %llu 回\n",
	            (unsigned long long)mu.sci4_hits());

	// カードが音道路上に戻した値。meli 10..15 が差し込み口なので、ここが
	// 非 0 なら「カードが選んだ値が音道路まで来た」ことになる
	// （doc/plg-cards.md 5）。聞くため��す���のではなく、値を確か���ため。
	if (plgcard) {
		for (int slot = 0; slot < plg::host::SLOTS; slot++)
			std::printf("PLG%d の戻り L=%d R=%d\n", slot + 1,
			            mu.plg_out(slot, 0), mu.plg_out(slot, 1));
	}

	if (hf)
		std::fclose(hf);
	if (pf)
		std::fclose(pf);
	if (tf)
		std::fclose(tf);
	if (sf)
		std::fclose(sf);

	// 液晶を最後に出す。PLG が見つかったかどうか（`NO BOARD` かどうか）は、
	// この文字列だけで分かる（mu2000_flash.bin の 0x1dda8d にある）。
	if (lcddump) {
		std::FILE *lf = std::fopen(lcddump, "w");
		if (!lf) {
			std::fprintf(stderr, "書けない: %s\n", lcddump);
			return 1;
		}
		const u8 *dd = mu.lcd().ddram();
		for (int row = 0; row < 4; row++) {
			for (int col = 0; col < 20; col++)
				std::fputc(dd[row * 0x20 + col] ? dd[row * 0x20 + col] : ' ', lf);
			std::fputc('\n', lf);
		}
		std::fclose(lf);
		std::printf("\n液晶（--lcd-dump と同じ）\n");
		for (int row = 0; row < 4; row++) {
			std::printf("  |");
			for (int col = 0; col < 20; col++)
				std::fputc(dd[row * 0x20 + col] ? dd[row * 0x20 + col] : ' ', stdout);
			std::printf("|\n");
		}
	}
	return 0;
}
