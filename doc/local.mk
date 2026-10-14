# SPDX-License-Identifier: MIT
# doc/local.mk: the Texinfo manual.
#
# info_TEXINFOS builds and installs libdtfl.info and ships libdtfl.texi
# in the distribution.  libdtfl_TEXINFOS lists the @include files so
# distcheck ships them too.  PDF and HTML are not gated (the pdf
# toolchain may be absent); info is.
#
# The doc/snippets directory is already shipped through EXTRA_DIST in
# Makefile.am; tests/dist/test_docs.sh compiles each snippet.

info_TEXINFOS = doc/libdtfl.texi

libdtfl_TEXINFOS = \
	doc/installation.texi \
	doc/concepts.texi \
	doc/threads.texi \
	doc/errors.texi \
	doc/embedding.texi \
	doc/diagnostics.texi \
	doc/ddir.texi \
	doc/apiref.texi \
	doc/porting.texi \
	doc/timely/manual.texi \
	doc/timely/introduction.texi \
	doc/timely/chapter_0.texi \
	doc/timely/chapter_1.texi \
	doc/timely/chapter_2.texi \
	doc/timely/chapter_3.texi \
	doc/timely/chapter_4.texi \
	doc/timely/chapter_5.texi \
	doc/differential/manual.texi \
	doc/differential/introduction.texi \
	doc/differential/chapter_0.texi \
	doc/differential/chapter_a.texi \
	doc/differential/chapter_1.texi \
	doc/differential/chapter_2.texi \
	doc/differential/chapter_3.texi \
	doc/differential/chapter_4.texi \
	doc/differential/chapter_5.texi \
	doc/differential/chapter_6.texi
