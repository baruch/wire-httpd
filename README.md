wire-httpd
==========

A web server based on [libwire][libwire].

Why?
----

To have a test-bed for a real network application based on libwire, otherwise
libwire will remain a toy. Also an attempt to be able to have a real measure of
performance for libwire.

Build
-----

To build you need to have ninja-build installed:

Debian/Ubuntu:

    sudo apt-get install ninja-build

Run:

    ./configure
    ninja

How to use
----------

    ./wire-httpd

It will serve any and all files from the local directory.

Author
------

Baruch Even <baruch@ev-en.org>
