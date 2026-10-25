"""Reads a MusicXML file chost wrote with music21, as MuseScore and the others would: every bar holds
exactly its time signature in every staff, notes are there, ties pair up, triplets come in threes.

    python check_score.py song.musicxml [parts] [triplets]
"""
import sys

import music21

path = sys.argv[1]
want_parts = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] != "-" else None
want_triplets = int(sys.argv[3]) if len(sys.argv) > 3 else None
score = music21.converter.parse(path)
staves = list(score.parts)
assert staves, "no parts"
notes = 0
triplets = 0
for staff in staves:
    meter = staff.recurse().getElementsByClass("TimeSignature")[0]
    bar = meter.barDuration.quarterLength
    for measure in staff.getElementsByClass("Measure"):
        length = measure.duration.quarterLength
        assert abs(length - bar) < 1e-6, "%s bar %d holds %s quarters, not %s" % (staff.partName, measure.number, length, meter.ratioString)
        # the members of each triplet add up to two of their kind: three eighths of a triplet to a quarter
        inside = 0.0
        for n in measure.notesAndRests:
            if n.duration.tuplets:
                inside += n.duration.quarterLength
                if abs(inside - round(inside)) < 1e-6 and round(inside) >= 1:
                    triplets += 1
                    inside = 0.0
        assert inside < 1e-6, "%s bar %d has an unfinished triplet" % (staff.partName, measure.number)
    for n in staff.recurse().notes:
        notes += 1
        if n.tie is not None and n.tie.type == "start":
            following = n.next("Note") or n.next("Chord")
            assert following is not None, "a tie into nothing in %s" % staff.partName
names = sorted({s.partName for s in staves})
if want_parts is not None:
    assert len(names) == want_parts, names
if want_triplets is not None:
    assert triplets == want_triplets, "%d triplets, not %d" % (triplets, want_triplets)
print("SCORE OK %s, %d staves, %d parts, %d notes, %d bars, %d triplets" % (meter.ratioString, len(staves), len(names), notes,
                                                                            len(staves[0].getElementsByClass("Measure")), triplets))
