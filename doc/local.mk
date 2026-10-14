# SPDX-License-Identifier: MIT
# doc/local.mk: the Texinfo manual.
#
# info_TEXINFOS builds and installs libdtfl.info and ships libdtfl.texi
# in the distribution.  libdtfl_TEXINFOS lists the @include files (added
# as chapters land) so distcheck ships them too.  PDF and HTML are not
# gated (the pdf toolchain may be absent); info is.
#
# The doc/snippets directory is already shipped through EXTRA_DIST in
# Makefile.am; tests/dist/test_docs.sh compiles each snippet.

info_TEXINFOS = doc/libdtfl.texi
