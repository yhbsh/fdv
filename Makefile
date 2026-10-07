NAME    = fdv
PLAYER_NAME = fpl
CC      = clang
BUILD   = build

# Install prefix. `make install` puts $(NAME) in $(PREFIX)/bin and the header in
# $(PREFIX)/include; override with `make install PREFIX=/usr/local` (needs sudo).
PREFIX  ?= $(HOME)/.local
BINDIR   = $(PREFIX)/bin
INCDIR   = $(PREFIX)/include
SCENEDIR = $(PREFIX)/share/$(NAME)/scenes

# -g lives in CFLAGS (compile) and deliberately NOT in LDFLAGS (link).
#
# On macOS, clang runs dsymutil automatically when a single invocation both
# compiles and links with -g, which is what litters the tree with .dSYM
# bundles.  Compiling to .o first and linking separately keeps full debug info
# (in the .o files, reached through the linker's debug map — lldb works) and
# produces no .dSYM at all.  So: never put -g on a line that also links.
CFLAGS  = -std=c11 -g -O2 -Wall -Wextra -Wshadow -pthread -Iinclude \
          -DFDV_SCENE_DIR='"$(SCENEDIR)"'
LDFLAGS = -pthread
LDLIBS  = -lm

# The library is one header. Each program defines FDV_IMPLEMENTATION and
# compiles its own copy, so there is no shared object to keep in step -- which
# is the point of shipping it this way.
HEADER  = include/$(NAME).h include/$(NAME)_scene.h
BIN     = $(BUILD)/$(NAME)
TESTS   = $(BUILD)/test_enc $(BUILD)/test_fuzz

.PHONY: all test clean install uninstall demo demo-y4m demo-vtile bench \
        fuzz tsan profile trace pipeline scenes help player play check-player capture

all: $(TESTS) $(BIN) player capture

$(BUILD):
	@mkdir -p $(BUILD)

# --- compile ---------------------------------------------------------------
$(BUILD)/fdv.o:       src/fdv.c         $(HEADER) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/test_enc.o:  tests/test_enc.c  $(HEADER) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/test_fuzz.o: tests/test_fuzz.c $(HEADER) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- link (no -g here: that is what keeps .dSYM bundles from appearing) -----
$(BIN):             $(BUILD)/fdv.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_enc:  $(BUILD)/test_enc.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_fuzz: $(BUILD)/test_fuzz.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# --- player ----------------------------------------------------------------
#
# Nothing but the header: fpl.c defines FDV_IMPLEMENTATION like everything
# else. Built only when raylib is present, so the codec still builds without it.
RAYLIB_PREFIX ?= $(HOME)/.local
RAYLIB_LIB     = $(RAYLIB_PREFIX)/lib/libraylib.a
RAYLIB_CFLAGS  = -I$(RAYLIB_PREFIX)/include
RAYLIB_LDLIBS  = $(RAYLIB_LIB) -framework Cocoa -framework OpenGL \
                 -framework IOKit -framework CoreVideo
HAVE_RAYLIB   := $(wildcard $(RAYLIB_LIB))

PLAYER = $(BUILD)/$(PLAYER_NAME)

$(BUILD)/fpl.o: src/fpl.c $(HEADER) | $(BUILD)
	$(CC) $(CFLAGS) $(RAYLIB_CFLAGS) -c -o $@ $<

$(PLAYER): $(BUILD)/fpl.o
	$(CC) $(LDFLAGS) -o $@ $^ $(RAYLIB_LDLIBS) $(LDLIBS)

player:
ifeq ($(HAVE_RAYLIB),)
	@echo "raylib not found at $(RAYLIB_LIB) — skipping $(PLAYER_NAME)"
	@echo "  (override with: make player RAYLIB_PREFIX=/path/to/raylib)"
else
	@$(MAKE) --no-print-directory $(PLAYER)
	@echo "built $(PLAYER)"
endif

# --- camera capture (macOS) -------------------------------------------------
#
# The only platform-dependent thing here. fdv.h stays plain C11 with no
# operating system in it; everything AVFoundation touches lives in src/fcap.m,
# and the boundary between them is an I420 frame. Built only on Darwin.
CAPTURE_NAME = fcap
CAPTURE      = $(BUILD)/$(CAPTURE_NAME)
UNAME_S     := $(shell uname -s)
CAPTURE_LDLIBS = -framework AVFoundation -framework CoreMedia \
                 -framework CoreVideo -framework Foundation

$(BUILD)/fcap.o: src/fcap.m $(HEADER) | $(BUILD)
	$(CC) $(CFLAGS) -fobjc-arc -c -o $@ $<

$(CAPTURE): $(BUILD)/fcap.o
	$(CC) $(LDFLAGS) -o $@ $^ $(CAPTURE_LDLIBS) $(LDLIBS)

capture:
ifeq ($(UNAME_S),Darwin)
	@$(MAKE) --no-print-directory $(CAPTURE)
	@echo "built $(CAPTURE)"
else
	@echo "$(CAPTURE_NAME) is macOS-only (AVFoundation) — skipping on $(UNAME_S)"
endif

# --- install ---------------------------------------------------------------

install: $(BIN) player
	@mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(INCDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(NAME)
	@echo "installed $(DESTDIR)$(BINDIR)/$(NAME)"
	@install -m 644 include/$(NAME).h $(DESTDIR)$(INCDIR)/$(NAME).h
	@install -m 644 include/$(NAME)_scene.h $(DESTDIR)$(INCDIR)/$(NAME)_scene.h
	@echo "installed $(DESTDIR)$(INCDIR)/$(NAME)_scene.h"
	@echo "installed $(DESTDIR)$(INCDIR)/$(NAME).h"
	@if [ -x $(PLAYER) ]; then \
	    install -m 755 $(PLAYER) $(DESTDIR)$(BINDIR)/$(PLAYER_NAME); \
	    echo "installed $(DESTDIR)$(BINDIR)/$(PLAYER_NAME)"; \
	fi
	@if [ -x $(CAPTURE) ]; then \
	    install -m 755 $(CAPTURE) $(DESTDIR)$(BINDIR)/$(CAPTURE_NAME); \
	    echo "installed $(DESTDIR)$(BINDIR)/$(CAPTURE_NAME)"; \
	fi
	@mkdir -p $(DESTDIR)$(SCENEDIR)
	@install -m 644 scenes/*.scn $(DESTDIR)$(SCENEDIR)/
	@echo "installed $$(ls scenes/*.scn | wc -l | tr -d ' ') scenes in $(DESTDIR)$(SCENEDIR)"
	@case ":$$PATH:" in \
	  *":$(BINDIR):"*) ;; \
	  *) echo "note: $(BINDIR) is not on your PATH — add"; \
	     echo "        export PATH=\"$(BINDIR):\$$PATH\""; \
	     echo "      to your shell profile" ;; \
	esac

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(NAME) $(DESTDIR)$(BINDIR)/$(PLAYER_NAME) \
	      $(DESTDIR)$(BINDIR)/$(CAPTURE_NAME)
	rm -f $(DESTDIR)$(INCDIR)/$(NAME).h
	rm -rf $(DESTDIR)$(PREFIX)/share/$(NAME)
	@echo "removed $(NAME), $(PLAYER_NAME), $(NAME).h and the scenes"

# --- run -------------------------------------------------------------------

test: $(TESTS)
	@./$(BUILD)/test_enc
	@echo "=== test_fuzz ==="
	@./$(BUILD)/test_fuzz

# End-to-end: synthesize a clip, encode it, decode it, verify it, logging every
# stage.  Override the scene with SCENE=, extra flags with ARGS=.
SCENE ?= motion
pipeline: $(BIN)
	./$(BIN) pipeline $(SCENE) $(ARGS)

scenes: $(BIN)
	@./$(BIN) scenes

# Encode a scene and open it in the player.
play: $(BIN) player
	./$(BIN) encode $(SCENE) $(BUILD)/$(SCENE).$(NAME) $(ARGS)
	./$(PLAYER) $(BUILD)/$(SCENE).$(NAME)

demo: $(BIN)
	./$(BIN) selftest 64 64 4 18

bench: $(BIN)
	./$(BIN) bench 320 192 8 20 20

demo-y4m: $(BIN)
	./$(BIN) gen tiny $(BUILD)/_y4m.y4m 2
	./$(BIN) encode $(BUILD)/_y4m.y4m $(BUILD)/_y4m.fdv -q 18
	./$(BIN) decode $(BUILD)/_y4m.fdv $(BUILD)/_y4m_out.y4m
	@rm -f $(BUILD)/_y4m.y4m $(BUILD)/_y4m.fdv $(BUILD)/_y4m_out.y4m

demo-vtile: $(BIN)
	./$(BIN) pipeline motion -t 2 -j 4 -o $(BUILD)/vtile

# --- checking tools --------------------------------------------------------

# -fno-sanitize-recover makes undefined behaviour abort rather than print a line
# and carry on. The unit suite is included because it reaches the scalar
# reference paths that the decoder fuzz never does.
SANFLAGS = -std=c11 -O1 -Wall -Wextra -Wshadow -pthread -Iinclude \
           -DFDV_SCENE_DIR='"$(SCENEDIR)"' \
           -fsanitize=address,undefined -fno-sanitize-recover=undefined

fuzz: | $(BUILD)
	$(CC) $(SANFLAGS) -o $(BUILD)/test_fuzz_asan tests/test_fuzz.c $(LDLIBS)
	./$(BUILD)/test_fuzz_asan
	$(CC) $(SANFLAGS) -o $(BUILD)/test_enc_asan  tests/test_enc.c  $(LDLIBS)
	./$(BUILD)/test_enc_asan > /dev/null
	@echo "unit suite clean under ASan + UBSan"
	@rm -rf $(BUILD)/test_fuzz_asan $(BUILD)/test_fuzz_asan.dSYM \
	        $(BUILD)/test_enc_asan  $(BUILD)/test_enc_asan.dSYM

# ThreadSanitizer over the tile-parallel encode and decode paths. Both run bands
# concurrently, so an unsynchronised lazy init or a shared counter is a real bug
# here rather than a theoretical one.
tsan: | $(BUILD)
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Wshadow -pthread -Iinclude \
	    -DFDV_SCENE_DIR='"$(SCENEDIR)"' \
	    -fsanitize=thread -o $(BUILD)/tsan_bin src/fdv.c $(LDLIBS)
	./$(BUILD)/tsan_bin pipeline motion -n 4 -t 2 -j 4 -o $(BUILD)/tsan >/dev/null
	./$(BUILD)/tsan_bin pipeline tiny -t 1 -j 4 -o $(BUILD)/tsan >/dev/null
	@rm -rf $(BUILD)/tsan_bin $(BUILD)/tsan_bin.dSYM $(BUILD)/tsan
	@echo "tsan: no data races reported"

# The zone profiler and per-macroblock tracing both read a clock or emit a line
# inside block loops, so they are compiled out of every other target.
profile: | $(BUILD)
	$(CC) $(CFLAGS) -DFDV_PROFILE -c -o $(BUILD)/fdv_prof.o src/fdv.c
	$(CC) $(LDFLAGS) -o $(BUILD)/$(NAME)-profile $(BUILD)/fdv_prof.o $(LDLIBS)
	@echo "built $(BUILD)/$(NAME)-profile — add --profile to any command"

trace: | $(BUILD)
	$(CC) $(CFLAGS) -DFDV_TRACE -c -o $(BUILD)/fdv_trace.o src/fdv.c
	$(CC) $(LDFLAGS) -o $(BUILD)/$(NAME)-trace $(BUILD)/fdv_trace.o $(LDLIBS)
	@echo "built $(BUILD)/$(NAME)-trace — run with -vvv for per-macroblock lines"

# Drive the real player and check what the unit suite cannot reach: the colour
# conversion, the GPU readback, the playback loop and the window placement.
check-player: $(BIN) player
	@bash tools/check-player.sh

help:
	@echo "make            build $(BIN), the tests and the player"
	@echo "make install    install $(NAME), $(PLAYER_NAME), $(CAPTURE_NAME), $(NAME).h and the scenes"
	@echo "make uninstall  remove them again"
	@echo "make test       unit suite + decoder fuzz harness"
	@echo "make pipeline   render/encode/decode/verify  (SCENE=motion ARGS='-v')"
	@echo "make play       encode a scene and open it   (SCENE=swarm)"
	@echo "make capture    build $(CAPTURE) (macOS camera recorder)"
	@echo "make scenes     list the scene files"
	@echo "make bench      encode/decode throughput"
	@echo "make fuzz       ASan + UBSan over the fuzz harness and the unit suite"
	@echo "make tsan       data-race check on the tile-parallel paths"
	@echo "make profile    $(BUILD)/$(NAME)-profile, with the zone profiler"
	@echo "make trace      $(BUILD)/$(NAME)-trace, with per-macroblock logging"
	@echo "make check-player  colour/GPU/playback/placement (needs a display)"
	@echo "make clean      remove $(BUILD)/"

clean:
	rm -rf $(BUILD)
