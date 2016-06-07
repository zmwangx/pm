CXXFLAGS = -O3 -std=c++0x

.PHONY: all clean distclean

all: pm

pm: pm.o
	$(LINK.cc) -o $@ $^

clean:
	@- $(RM) pm pm.o

distclean: clean
