# $Id$
#
# sca module makefile
#
# 
# WARNING: do not run this directly, it should be run by the master Makefile

include ../../Makefile.defs
auto_gen=
NAME=sca.so
LIBS=

DEFS+=-DSER_MOD_INTERFACE

include ../../Makefile.modules
