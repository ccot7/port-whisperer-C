# ──────────────────────────────────────────────────────────────
#  port-whisperer — bare-metal C build
#  Supports: Linux (Fedora/Wayland) · macOS
# ──────────────────────────────────────────────────────────────

APP     := ports
SRC     := src/main.c
PREFIX  ?= $(HOME)/.local
BINDIR  := $(PREFIX)/bin

# ── detect platform ────────────────────────────────────────────
UNAME := $(shell uname -s)

ifeq ($(UNAME), Darwin)
  CC      ?= clang
  CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wpedantic \
             -DPLATFORM_MACOS
  LDFLAGS :=
else
  CC      ?= gcc
  CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wpedantic \
             -D_GNU_SOURCE -DPLATFORM_LINUX
  LDFLAGS :=
endif

# ── build dir (hidden to avoid indexing noise) ─────────────────
BUILDDIR := .build

.PHONY: all clean install uninstall

all: $(BUILDDIR)/$(APP)

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

$(BUILDDIR)/$(APP): $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  Built: $@"

clean:
	rm -rf $(BUILDDIR)

install: $(BUILDDIR)/$(APP)
	@mkdir -p $(BINDIR)
	cp $(BUILDDIR)/$(APP) $(BINDIR)/$(APP)
	@echo "  Installed → $(BINDIR)/$(APP)"
	@echo "  Make sure $(BINDIR) is in your PATH."

uninstall:
	rm -f $(BINDIR)/$(APP)
	@echo "  Removed $(BINDIR)/$(APP)"
