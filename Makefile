prefix ?= /usr/local
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin
libexecdir = $(exec_prefix)/libexec
datarootdir = $(prefix)/share
mandir = $(datarootdir)/man
man1dir = $(mandir)/man1
docdir = $(datarootdir)/doc/pm
CXXFLAGS += -O3 -std=c++0x -Wno-unused-result

.PHONY: all clean distclean install uninstall

all: bin/pm libexec/pm/server.py

bin/pm: pm.o
	mkdir -p bin
	$(LINK.cc) $^ -lpthread -o $@

libexec/pm/server.py: server.py
	mkdir -p libexec/pm
	cp $^ $@

install: all
	mkdir -p $(DESTDIR)$(bindir) $(DESTDIR)$(libexecdir)/pm $(DESTDIR)$(man1dir) $(DESTDIR)$(docdir)
	install bin/pm $(DESTDIR)$(bindir)/pm
	install libexec/pm/server.py $(DESTDIR)$(libexecdir)/pm/server.py
	install pm.1 $(DESTDIR)$(man1dir)/pm.1
	install COPYING README.md $(DESTDIR)$(docdir)

uninstall:
	@- $(RM) $(DESTDIR)$(bindir)/pm $(DESTDIR)$(man1dir)/pm.1
	@- $(RM) -r $(DESTDIR)$(libexecdir)/pm $(DESTDIR)$(docdir)

clean:
	@- $(RM) pm.o bin/pm libexec/pm/server.py

distclean: clean
	@- $(RM) -r autom4te.cache
	@- $(RM) config.h config.log
	@- rmdir -p bin libexec/pm
