#!/usr/bin/env python3
# license:BSD-3-Clause
"""Regenerate tests/plg/vlreply.h from tests/plg/plg100vl-capture.txt.

The replay table in the answer card is transcribed from a real PLG100-VL talking
to a real S-MU2000. The capture is committed, so this only has to be run when
there is a new capture - and there should not be, because the card it models is
one machine talking to another and the answer does not change.

    python3 tools/mkvlreply.py [capture] [out.h]

Two pairs are kept, both keyed on the bytes between the sub-command and F7:

    F0 43 30 4F <4 bytes> F7    a poll      (46 in the capture)
    F0 43 40 03 <7 bytes> F7    a read      (595 in the capture)

and the stored value is the whole reply message, replayed verbatim. Storing whole
messages rather than a decoded field is deliberate: the reply layout was read off
the capture once and misread once already - the 4f reply echoes three of the
four request bytes, not four, and a table of "address -> value" that assumed four
produced values shifted by one. Replaying bytes cannot have that bug.

The 4e control channel is deliberately not here. That one is a real memory model
in answer.c, because it is the part a card actually owns.
"""

import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_IN = os.path.join(HERE, "tests", "plg", "plg100vl-capture.txt")
DEFAULT_OUT = os.path.join(HERE, "tests", "plg", "vlreply.h")


def read_capture(path):
    """-> (host_messages, card_messages), each a list of byte lists."""
    host, card, side = [], [], None
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                if "host -> card" in line:
                    side = host
                elif "card -> host" in line:
                    side = card
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                side.append([int(p, 16) for p in parts[1:]])
            except ValueError:
                continue
    return host, card


def pairs(host, card, req_env, req_sub, rep_env, rep_sub):
    """Pair each matching request with the next matching reply, in order.

    The envelopes differ, which is the whole reason this takes four arguments
    rather than two: a 4f poll goes out as F0 43 30 4F and comes back as
    F0 43 10 4F. Filtering both sides on one envelope silently yields nothing.
    """
    reqs = [m for m in host
            if len(m) > 5 and m[2] == req_env and m[3] == req_sub]
    reps = [m for m in card
            if len(m) > 5 and m[2] == rep_env and m[3] == rep_sub]
    if len(reqs) != len(reps):
        print("  warning: %d requests but %d replies" % (len(reqs), len(reps)),
              file=sys.stderr)
    out, seen = [], set()
    for r, p in zip(reqs, reps):
        key = tuple(r[4:-1])
        if key in seen:
            continue
        seen.add(key)
        out.append((key, tuple(p)))
    return out


def emit(table, out_path, note):
    maxreq = max(len(k) for k, _ in table)
    maxrep = max(len(v) for _, v in table)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("""/* Generated from tests/plg/plg100vl-capture.txt by tools/mkvlreply.py -
 * do not edit by hand.
 *
 * Every (request payload -> full reply message) pair a real PLG100-VL produced in
 * 120 emulated seconds, keyed on the bytes between the sub-command and F7. The
 * card replays these verbatim, which is what makes it a replay fixture and not a
 * simulator: the values are the VL's.
 *
%s
 */
#ifndef VLREPLY_H
#define VLREPLY_H

struct vlreply {
	const unsigned char *req;
	int                 reqlen;
	const unsigned char *rep;
	int                 replen;
};

""" % note)
        f.write("/* longest request payload %d, longest reply %d */\n\n" % (maxreq, maxrep))
        f.write("static const unsigned char vlreq[][%d] = {\n" % maxreq)
        for k, _ in table:
            f.write("\t{ %s },\n" % ", ".join("0x%02x" % x for x in k))
        f.write("};\n\nstatic const unsigned char vlrep[][%d] = {\n" % maxrep)
        for _, v in table:
            f.write("\t{ %s },\n" % ", ".join("0x%02x" % x for x in v))
        f.write("};\n\nstatic const struct vlreply VLREPLIES[] = {\n")
        for i, (k, v) in enumerate(table):
            f.write("\t{ vlreq[%d], %d, vlrep[%d], %d },\n" % (i, len(k), i, len(v)))
        f.write("};\n#define VLNREPLIES ((int)(sizeof(VLREPLIES) / sizeof(VLREPLIES[0])))\n")
        f.write("\n#endif\n")


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IN
    dst = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT
    host, card = read_capture(src)
    table = pairs(host, card, 0x30, 0x4f, 0x10, 0x4f)
    table += pairs(host, card, 0x40, 0x03, 0x40, 0x43)
    if not table:
        sys.exit("no 4f or 40 pairs in %s - is the capture the current format?" % src)
    emit(table, dst,
         " %d entries: 4f polls and 40 bulk reads, stored as whole messages so the\n"
         " * reply layout cannot be misread the way a decoded field was." % len(table))
    print("wrote %s (%d entries)" % (dst, len(table)))


if __name__ == "__main__":
    main()
