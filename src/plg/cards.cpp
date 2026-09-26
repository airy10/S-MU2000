// license:BSD-3-Clause
//
// plg/cards.cpp - the list in plg/cards.h.
//
// The four entry points of the answer card, declared here rather than including
// its C header: it is compiled as C into this C++ program, so the symbols are
// extern "C" and nothing else is shared. That is the whole contract between a
// built-in card and the host, and it is the same contract a shared library meets.
#include "plg/cards.h"

// tests/plg/answer.c, compiled as C.
extern "C" {
const plg_card_info *plg1500_get_info(void);
plg_card            *plg1500_create(const plg_host *, void *, char *, size_t);
void                plg1500_destroy(plg_card *);
const plg_card_ops  *plg1500_ops(const plg_card *);
}

namespace plg {

namespace {

const host::builtin_card g_cards[] = {
	{ plg1500_get_info, plg1500_create, plg1500_destroy, plg1500_ops,
	  "built in: answer" },
};

} // namespace

int builtin_count()
{
	return int(sizeof(g_cards) / sizeof(g_cards[0]));
}

const host::builtin_card &builtin_card(int i)
{
	return g_cards[i];
}

} // namespace plg
