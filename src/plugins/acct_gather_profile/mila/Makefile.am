# Makefile for acct_gather_profile/mila plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common $(LIBCURL_CPPFLAGS)

pkglib_LTLIBRARIES = acct_gather_profile_mila.la

acct_gather_profile_mila_la_SOURCES = acct_gather_profile_mila.c
acct_gather_profile_mila_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS)
acct_gather_profile_mila_la_LIBADD = $(LIBCURL)
