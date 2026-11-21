<!-- SPDX-License-Identifier: MIT -->
# Extra HaikuPorts packages for Beacon images

Binary packages from the official HaikuPorts riscv64 repository
(https://eu.hpkg.haiku-os.org/haikuports/master/riscv64/current), unmodified.
They are not in Haiku's riscv64 *build-packages* set, so the image build cannot
fetch them; `libmidi` (Media preferences, MidiPlayer) needs `libvorbisfile`.

| File | SHA-256 (matches the repository index) | Licence |
|---|---|---|
| libogg-1.3.6-2-riscv64.hpkg | ae39fb57547126d39af1823a305cbd739ecf8f1410ac14037fde811fabc26ae3 | BSD-3-Clause (Xiph.Org Foundation) |
| libvorbis-1.3.7-1-riscv64.hpkg | 3208ed14f0e0e36fc41ec854547f07bb94962c39ed487edda758d72130773a71 | BSD-3-Clause (Xiph.Org Foundation) |

The packages declare "BSD (3-clause)" with the Xiph.Org copyright; the licence
text ships in every image as `/system/data/licenses/BSD (3-clause)`. Sources:
https://downloads.xiph.org/releases/ogg/ and https://downloads.xiph.org/releases/vorbis/
