CXXFLAGS = -O3 -std=c++0x

.PHONY: all clean distclean

all: bin/pm libexec/pm/server.py

bin/pm: pm.o
	mkdir -p bin
	$(LINK.cc) -o $@ $^

libexec/pm/server.py: server.py
	mkdir -p libexec/pm
	cp $^ $@

clean:
	@- $(RM) pm pm.o

distclean: clean
	@- rmdir -p bin libexec/pm
