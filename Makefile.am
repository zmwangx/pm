AUTOMAKE_OPTIONS = foreign

man1_MANS = pm.1
doc_DATA = COPYING README.md

CLEANFILES = pm.o bin/pm libexec/pm/server.py
DISTCLEANFILES = aclocal.m4 config.h config-paths.h

CXXFLAGS = -O3 -std=c++0x
LDLIBS =

if LINUX
CXXFLAGS += -Wno-unused-result
LDLIBS += -lpthread -lutil
endif

all: bin/pm libexec/pm/server.py

pm.o: pm.cc config-paths.h
	$(COMPILE.cc) $< -o $@

bin/pm: pm.o
	mkdir -p bin
	$(LINK.cc) $^ $(LDLIBS) -o $@

libexec/pm/server.py: server.py
	mkdir -p libexec/pm
	cp $^ $@

config-paths.h:
	echo '#define BINDIR "$(bindir)"' >$@

install-exec-local:
	mkdir -p $(DESTDIR)$(bindir) $(DESTDIR)$(pkglibexecdir)
	install bin/pm $(DESTDIR)$(bindir)/pm
	install libexec/pm/server.py $(DESTDIR)$(pkglibexecdir)/server.py

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/pm
	rm -rf $(DESTDIR)$(pkglibexecdir)

distclean-local:
	-rm -rf autom4te.cache
	-rmdir -p bin libexec/pm 2>/dev/null; :
