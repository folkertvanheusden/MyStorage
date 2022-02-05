MyStorage
---------

This program is a storage-server.
It can listen to NBD and AoE currently.
It can mirror, also asynchronously.
Everything can go through a journal.
Target(s) can be (an) other NBD- or AoE- server, a plain file, a deduplicated store and some others.

The 'examples'-directory contains some sample configuration files.
They are in yaml-format.

NBD is "network block device" and AoE is "ATA over Ethernet".


NOTE: this software is very young, it may still have bugs that can e.g. corrupt data. Also performance is still not optimal.


required packages
-----------------

- liblzo2-dev
- libcrypto++-dev
- libkyotocabinet-dev
- libyaml-cpp-dev


building
--------

* mkdir build
* cd build
* cmake ..
* make


potentially asked questions
---------------------------
Q: can this program corrupt my data?
A: could be; make sure you have backups

Q: it destroyed my data!
A: restore your backup

Q: tiering won't work
A: make sure you're using a meta-file/disk that is entirely 0x00 (use cat/dd/blkdiscard/whatever)

Q: I'm not certain how to use this
A: then don't


---
(C) 2022 by Folkert van Heusden <mail@vanheusden.com>

Released under AGPL v2.0
