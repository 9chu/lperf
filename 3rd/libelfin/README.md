[Libelfin](https://github.com/aclements/libelfin/) is a from-scratch
C++11 library for reading ELF binaries and DWARFv4 debug information.

Quick start
-----------

`make`, and optionally `make install`.  You'll need GCC 4.7 or later.

Features
--------

* Native C++11 code and interface, designed from scratch to interact
  well with C++11 features, from range-based for loops to move
  semantics to enum classes.

* Libelfin fully implements parsing for Debugging Information Entries
  (DIEs), the core data structure used by the DWARF format, as well as
  most DWARFv4 tables.

* Supports all DWARFv4 DIE value types except location lists and
  macros.

* Nearly complete evaluator for DWARFv4 expressions and location
  descriptions.

* Complete interpreter for DWARFv4 line tables.

* Iterators for easily and naturally traversing compilation units,
  type units, DIE trees, and DIE attribute lists.

* Every enum value can be pretty-printed.

* Large collection of type-safe DIE attribute fetchers.

Non-features
------------

Libelfin implements a *syntactic* layer for DWARF and ELF, but not a
*semantic* layer.  Interpreting the information stored in DWARF DIE
trees still requires a great deal of understanding of DWARF, but
libelfin will make sense of the bytes for you.

Using libelfin
--------------

To build against `libdwarf++`, use, for example

    g++ -std=c++11 a.cc $(pkg-config --cflags --libs libdwarf++)

To use a local build of libelfin, set `PKG_CONFIG_PATH`.  For example,

    export PKG_CONFIG_PATH=$PWD/elf:$PWD/dwarf

There are various example programs in `examples/`.

Status
------

Libelfin is a good start.  It's not production-ready and there are
many parts of the DWARF specification it does not yet implement, but
it's complete enough to be useful for many things and is a good deal
more pleasant to use than every other debug info library I've tried.
