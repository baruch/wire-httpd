wire-httpd
==========

A web server based on [libwire][libwire].

[![Build Status](https://travis-ci.org/baruch/wire-httpd.png?branch=master)](https://travis-ci.org/baruch/wire-httpd)

Why?
----

To have a test-bed for a real network application based on libwire, otherwise
libwire will remain a toy. Also an attempt to be able to have a real measure of
performance for libwire.

Build
-----

To build you need to have ninja-build and gperf installed:

Debian/Ubuntu:

    sudo apt-get install ninja-build gperf

Run:

    git submodule update
    ./configure
    ninja

How to use
----------

    ./wire-httpd

It will serve any and all files from the local directory.

Author
------

Baruch Even <baruch@ev-en.org>

  [libwire]: https://github.com/baruch/libwire
