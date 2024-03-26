
AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES         = sippulserecog.la

sippulserecog_la_SOURCES       = src/sippulse_recog_engine.c 
sippulserecog_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS) -lcurl -lsamplerate

include $(top_srcdir)/build/rules/uniplugin.am
