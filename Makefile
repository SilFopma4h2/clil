# clil — `make` builds ./clil. Needs only a C++17 compiler (Xcode Command Line Tools on macOS).
CXX      ?= c++
PREFIX   ?= /usr/local
ARCH     ?=
CXXFLAGS ?= -Os
LDFLAGS  ?=

# Required flags live apart from CXXFLAGS so `make CXXFLAGS=...` can't drop them.
FLAGS := -std=c++17 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -MMD -MP

ifeq ($(shell uname -s),Darwin)
  FLAGS += -flto -mmacosx-version-min=11.0
  LINK  := -flto -mmacosx-version-min=11.0 -Wl,-dead_strip
else
  FLAGS += -flto=auto
  LINK  := -flto=auto -Wl,--gc-sections
endif

SRC  := $(wildcard src/*.cpp)
OBJ  := $(SRC:src/%.cpp=build/%.o)
LIB  := $(filter-out build/main.o,$(OBJ))

clil: $(OBJ)
	$(CXX) $(ARCH) $(FLAGS) $(CXXFLAGS) $(LINK) $(LDFLAGS) -o $@ $(OBJ)
	strip $@

build/%.o: src/%.cpp | build
	$(CXX) $(ARCH) $(FLAGS) $(CXXFLAGS) -c -o $@ $<

build/test.o: tests/test.cpp | build
	$(CXX) $(ARCH) $(FLAGS) $(CXXFLAGS) -Isrc -c -o $@ $<

build/clil-test: $(LIB) build/test.o
	$(CXX) $(ARCH) $(FLAGS) $(CXXFLAGS) $(LINK) $(LDFLAGS) -o $@ $^

build:
	mkdir -p build

test: build/clil-test
	./build/clil-test

# Apple Silicon + Intel in one binary
universal:
	$(MAKE) clean
	$(MAKE) ARCH="-arch arm64 -arch x86_64"

install: clil
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 clil $(DESTDIR)$(PREFIX)/bin/clil

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/clil

clean:
	rm -rf build clil

.PHONY: test universal install uninstall clean

-include $(OBJ:.o=.d) build/test.d
