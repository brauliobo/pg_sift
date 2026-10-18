# Opt-in compiler hardening of the PostgreSQL build (docs/SAFETY.md); included by the Makefile after PG_CONFIG is known.
#
#   make SIFT_HARDEN=pkg     the flags that cost no measurable instructions on the hot loops: stack protector, stack clash protection,
#                            control-flow protection, _FORTIFY_SOURCE=3, strict flexible arrays, full RELRO and BIND_NOW,
#                            and the warnings that find overruns and misuse (-Wextra, -Wvla, -Warray-bounds=2, -Wstringop-overflow=4,
#                            -Wformat=2; no warning at the current head, so a new one is a regression)
#   make SIFT_HARDEN=init    pkg + -ftrivial-auto-var-init=zero (uninitialized locals read as 0, not as a previous frame)
#   make SIFT_HARDEN=1       init + trap-mode UBSan (bounds, shift, nonnull, returns-nonnull, pointer-overflow, object-size, vla-bound):
#                            a violation executes a trap instruction, no sanitizer runtime is linked, so it loads into any server
#
# A flag the compiler does not know is dropped (the probe compiles and links an empty program with it), so the same command line
# works on every builder. SANITIZE=1 replaces the trap set (the ASan/UBSan runtime reports instead). Switching SIFT_HARDEN needs
# `make clean`; scripts/pgtest.sh does it (the value is part of its build flavor).
ifdef SIFT_HARDEN
comma := ,
empty :=
space := $(empty) $(empty)
# the compiler of the PostgreSQL build, unless CC is given on the command line
SIFT_HCC := $(if $(filter command line,$(origin CC)),$(CC),$(shell $(PG_CONFIG) --cc 2>/dev/null || echo cc))
# sift_cc_opts FLAGS [EXTRA]: the words of FLAGS that $(SIFT_HCC) accepts (compile and link of an empty program, with EXTRA)
sift_cc_opts = $(foreach f,$(1),$(if $(shell echo 'int main(void) { return 0; }' | $(SIFT_HCC) $(f) $(2) -Werror -x c - -o /dev/null 2>/dev/null && echo ok),$(f)))

SIFT_HARDEN_CFLAGS := -fstack-protector-strong -fstack-clash-protection -fstrict-flex-arrays=3 -Wvla -Walloca -Warray-bounds=2 -Wstringop-overflow=4 \
	-Wformat=2 -Werror=format-security -Wundef -Wimplicit-fallthrough -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
ifeq ($(shell uname -m),x86_64)
SIFT_HARDEN_CFLAGS += -fcf-protection=full
else ifeq ($(shell uname -m),aarch64)
SIFT_HARDEN_CFLAGS += -mbranch-protection=standard
endif
SIFT_HARDEN_CFLAGS := $(call sift_cc_opts,$(SIFT_HARDEN_CFLAGS))
# _FORTIFY_SOURCE=3 needs optimization; a distribution's own definition is replaced, not doubled
SIFT_HARDEN_CFLAGS += $(if $(call sift_cc_opts,-O2,-D_FORTIFY_SOURCE=3),-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
SIFT_HARDEN_LINK := $(call sift_cc_opts,-Wl$(comma)-z$(comma)relro$(comma)-z$(comma)now -Wl$(comma)-z$(comma)noexecstack)

ifneq ($(filter init 1,$(SIFT_HARDEN)),)
SIFT_HARDEN_CFLAGS += $(call sift_cc_opts,-ftrivial-auto-var-init=zero)
endif
# the undefined-behavior checks of the trap set (SIFT_UBSAN_SET=bounds,shift,.. overrides the list: the cost of each is in docs/SAFETY.md)
SIFT_UBSAN_WANT := $(or $(strip $(subst $(comma),$(space),$(SIFT_UBSAN_SET))),bounds shift nonnull-attribute returns-nonnull-attribute pointer-overflow object-size vla-bound)
ifeq ($(SIFT_HARDEN),1)
ifndef SANITIZE
SIFT_UBSAN_KINDS := $(patsubst -fsanitize=%,%,$(call sift_cc_opts,$(addprefix -fsanitize=,$(SIFT_UBSAN_WANT)),-fsanitize-undefined-trap-on-error))
ifneq ($(SIFT_UBSAN_KINDS),)
SIFT_HARDEN_CFLAGS += -fsanitize=$(subst $(space),$(comma),$(strip $(SIFT_UBSAN_KINDS))) -fsanitize-undefined-trap-on-error
endif
endif
endif
PG_CFLAGS += $(SIFT_HARDEN_CFLAGS)
SHLIB_LINK += $(SIFT_HARDEN_LINK)
endif
