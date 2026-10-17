# clil — `make` builds ./clil. Needs only a C11/C++17 compiler (Xcode Command Line Tools on macOS).
CXX      ?= c++
CC       ?= cc
PREFIX   ?= /usr/local
ARCH     ?=
CXXFLAGS ?= -Oz
CFLAGS   ?= -Oz
LDFLAGS  ?=

# Required flags live apart from CXXFLAGS so `make CXXFLAGS=...` can't drop them.
COMMON := -Wall -Wextra -ffunction-sections -fdata-sections -MMD -MP
FLAGS  := -std=c++17 -fno-exceptions -fno-rtti $(COMMON)
CFLAG  := -std=c11 -D_GNU_SOURCE $(COMMON)

ifeq ($(shell uname -s),Darwin)
  FLAGS += -flto -mmacosx-version-min=11.0
  CFLAG += -flto -mmacosx-version-min=11.0
  LINK  := -flto -mmacosx-version-min=11.0 -Wl,-dead_strip
else
  FLAGS += -flto=auto
  CFLAG += -flto=auto
  LINK  := -flto=auto -Wl,--gc-sections
endif

SRC  := $(wildcard src/*.cpp)
OBJ  := $(SRC:src/%.cpp=build/%.o) $(patsubst src/%.c,build/%.o,$(wildcard src/*.c))
LIB  := $(filter-out build/main.o,$(OBJ))

clil: $(OBJ)
	$(CXX) $(ARCH) $(FLAGS) $(CXXFLAGS) $(LINK) $(LDFLAGS) -o $@ $(OBJ)
	strip $@

build/%.o: src/%.cpp | build
	$(CXX) $(ARCH) $(FLAGS) $(CXXFLAGS) -c -o $@ $<

build/%.o: src/%.c | build
	$(CC) $(ARCH) $(CFLAG) $(CFLAGS) -c -o $@ $<

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
