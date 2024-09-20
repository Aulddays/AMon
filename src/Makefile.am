include $(top_srcdir)/common.mk

bin_PROGRAMS = amon
amon_SOURCES = main.cpp CollectdReceiver.cpp CollectdReceiver.h GrafanaReader.cpp GrafanaReader.h AMon.h AMon.cpp Alog.h Alog.cpp AUint.h ap_dirent.h pe_log.h pe_log.cpp fp16/*.h
#amon_SOURCES += libconfig/grammar.c libconfig/grammar.h libconfig/libconfig.c libconfig/libconfig.h libconfig/parsectx.h libconfig/scanctx.c libconfig/scanctx.h libconfig/scanner.c libconfig/scanner.h libconfig/strbuf.c libconfig/strbuf.h libconfig/strvec.c libconfig/strvec.h libconfig/util.c libconfig/util.h libconfig/wincompat.c libconfig/wincompat.h
amon_CXXFLAGS = $(AM_CXXFLAGS) -DASIO_STANDALONE -Winvalid-pch
amon_LDADD = -lpthread
