// license:BSD-3-Clause
//
// plg/cards.h - the cards built into this program.
//
// A card is normally a shared library somebody else wrote. A card in here is the
// same four entry points, found by calling them instead of by dlsym
// (plg::host::insert_builtin), so one implementation can be shipped either way -
// which matters here for a reason beyond tidiness: the reference card below needs
// no shared library and no data of any kind, so it can be part of `make check`
// and of CI. A real card cannot, because it needs a firmware dump this
// repository does not carry.
//
// **A card in this list is a reference, not a product.** `answer` speaks the two
// replies a real PLG150-AP speaks and nothing else; it is here to prove the host
// end, not to be a synthesiser. See doc/plg-cards.md.
#ifndef S_MU2000_PLG_CARDS_H
#define S_MU2000_PLG_CARDS_H

#include "plg/host.h"

namespace plg {

// The number of built-in cards, and one of them by index. Both are small and
// fixed: a list that changes at runtime is a list somebody will forget to
// rebuild around.
int builtin_count();
const host::builtin_card &builtin_card(int i);

} // namespace plg

#endif // S_MU2000_PLG_CARDS_H
