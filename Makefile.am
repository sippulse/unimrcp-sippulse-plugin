macrodir		= @ac_macro_dir@
auxdir			= @ac_aux_dir@
AUX_DIST		= $(auxdir)/config.guess \
                          $(auxdir)/config.sub \
                          $(auxdir)/install-sh \
                          $(auxdir)/ltconfig \
                          $(auxdir)/ltmain.sh \
                          $(auxdir)/depcomp \
                          $(auxdir)/missing
EXTRA_DIST		= bootstrap

AUTOMAKE_OPTIONS 	= foreign
MAINTAINERCLEANFILES 	= Makefile.in aclocal.m4 configure $(AUX_DIST)

ACLOCAL			= aclocal -I $(macrodir)

SUBDIRS                 = libs tests