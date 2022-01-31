MyStorage
---------

This program is a storage-server.
It can listen to NBD and AoE currently.
It can mirror, also asynchronously.
Everything can go through a journal.
Target(s) can be (an) other NBD- or AoE- server, a plain file, a deduplicated store and some others.

The 'examples'-directory contains some sample configuration files.
They are in yaml-format.


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


---
(C) 2022 by Folkert van Heusden <mail@vanheusden.com>

Released under AGPL v2.0
