AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES         = sippulsesynth.la

sippulsesynth_la_SOURCES       = src/sippulse_synth_engine.c
sippulsesynth_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS) -lcurl -lpthread -lcjson

include $(top_srcdir)/build/rules/uniplugin.am