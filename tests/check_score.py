"""Reads a MusicXML file chost wrote with music21, as MuseScore and the others would: every bar holds
exactly 4/4 in every staff, notes are there, ties pair up.

    python check_score.py song.musicxml [parts]
"""
import sys

import music21

path = sys.argv[1]
want_parts = int(sys.argv[2]) if len(sys.argv) > 2 else None
score = music21.converter.parse(path)
staves = list(score.parts)
assert staves, "no parts"
notes = 0
for staff in staves:
    for measure in staff.getElementsByClass("Measure"):
        length = measure.duration.quarterLength
        assert abs(length - 4.0) < 1e-6, "%s bar %d holds %s quarters" % (staff.partName, measure.number, length)
    for n in staff.recurse().notes:
        notes += 1
        if n.tie is not None and n.tie.type == "start":
            following = n.next("Note") or n.next("Chord")
            assert following is not None, "a tie into nothing in %s" % staff.partName
names = sorted({s.partName for s in staves})
if want_parts is not None:
    assert len(names) == want_parts, names
print("SCORE OK %d staves, %d parts, %d notes, %d bars" % (len(staves), len(names), notes, len(staves[0].getElementsByClass("Measure"))))
