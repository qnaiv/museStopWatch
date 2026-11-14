# SanoTTS model and dictionary (not in git)

The firmware flashes two files from this folder into their own partitions
(see ../CMakeLists.txt):

- `voice-v4-int8.bin` (654,032 bytes)
- `k1-dict-44000-2mb.bin` (977,456 bytes)

Both come from the official sanoTTS-jp v1.2.0 release:
https://github.com/ayutaz/sanoTTS-jp/releases

The model is not under the MIT license (see ../upstream/LICENSE-MODEL.md),
so this repository does not carry the files. Download them from the release
and put them here before you build the firmware. The NOTICE files here give
the dictionary and Open JTalk notices.

This repository is public. The two files are never added to git, to Releases or
to an Actions artifact (the model has its own license: attribution and notices
go with every redistribution, see ../upstream/LICENSE-MODEL.md). The CI builds
with them, and its public images leave out the `sano_model` and `sano_dict`
partitions; see docs/first-flash.md at the top of this repository.
