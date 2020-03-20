MAKEFLAGS += --no-builtin-rules

TOPDIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
DIR := $(notdir $(patsubst $(TOPDIR)/%,%,$(CURDIR)))/

# Compiler tools & flags definitions
CC	:= m68k-amigaos-gcc -noixemul -g
AS	:= vasm -quiet
CFLAGS	= $(LDFLAGS) $(OFLAGS) $(WFLAGS) $(DFLAGS)

ASFLAGS	:= -x -m68010
LDFLAGS	:= -g -m68000 -msmall-code -nostartfiles -nostdlib
# The '-O2' option does not turn on optimizations '-funroll-loops',
# '-funroll-all-loops' and `-fstrict-aliasing'.
OFLAGS	:= -O2 -fomit-frame-pointer -fstrength-reduce
WFLAGS	:= -Wall -W -Werror -Wundef -Wsign-compare -Wredundant-decls
WFLAGS  += -Wnested-externs -Wwrite-strings -Wstrict-prototypes
 
CRT0	:= $(TOPDIR)/base/crt0.o

# Pass "VERBOSE=1" at command line to display command being invoked by GNU Make
ifneq ($(VERBOSE), 1)
.SILENT:
QUIET := --quiet
endif

# Don't reload library base for each call.
DFLAGS := -D__CONSTLIBBASEDECL__=const -DUSE_IO_DOS=0

LDLIBS	=
CPPFLAGS += -I$(TOPDIR)/base/include

# Common tools definition
CP := cp -a
RM := rm -v -f
PYTHON3 := PYTHONPATH="$(TOPDIR)/pylib:$$PYTHONPATH" python3
FSUTIL := $(TOPDIR)/tools/fsutil.py
BINPATCH := $(TOPDIR)/tools/binpatch.py
RUNINUAE := $(PYTHON3) $(TOPDIR)/effects/RunInUAE
ILBMCONV := $(TOPDIR)/tools/ilbmconv.py
ILBMPACK := $(TOPDIR)/tools/ilbmpack.py $(QUIET)
DUMPLWO := $(TOPDIR)/tools/dumplwo.py $(QUIET)
PSFTOPNG := $(TOPDIR)/tools/psftopng.py
TMXCONV := $(TOPDIR)/tools/tmxconv.py
OPTIPNG := optipng $(QUIET)
STRIP := m68k-amigaos-strip -s

# Generate dependencies automatically
SOURCES_C = $(filter %.c,$(SOURCES))
SOURCES_ASM = $(filter %.s,$(SOURCES))
OBJECTS = $(SOURCES_C:.c=.o) $(SOURCES_ASM:.s=.o)
DEPFILES = $(SOURCES_C:%.c=.%.P)

$(DEPFILES): $(SOURCES_GEN)

ifeq ($(words $(findstring $(MAKECMDGOALS), clean)), 0)
  -include $(DEPFILES)
endif

CLEAN-FILES += $(DEPFILES) $(SOURCES_GEN) $(DATA_GEN)

# Disable all built-in recipes and define our own
.SUFFIXES:

.%.P: %.c
	@echo "[DEP] $(DIR)$< -> $(DIR)$@"
	$(CC) $(CPPFLAGS) -MM -MG -o $@ $<

%.o: %.c .%.P
	@echo "[CC] $(DIR)$< -> $(DIR)$@"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%: %.o
	@echo "[LD] $(DIR)$^ -> $(DIR)$@"
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.s
	@echo "[AS] $(DIR)$< -> $(DIR)$@"
	$(AS) -Fhunk $(ASFLAGS) -o $@ $<

%.bin: %.s
	@echo "[AS] $(DIR)$< -> $(DIR)$@"
	$(AS) -Fbin $(ASFLAGS) -o $@ $<

%.s: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -S -fverbose-asm -o $@ $<

# Rules for recursive build
build-%: FORCE
	@echo "[MAKE] build $*"
	$(MAKE) -C $(@:build-%=%)

clean-%: FORCE
	@echo "[MAKE] clean $*"
	$(MAKE) -C $(@:clean-%=%) clean

# Rules for build
build: $(foreach dir,$(SUBDIRS),build-$(dir)) $(BUILD-FILES)

clean: $(foreach dir,$(SUBDIRS),clean-$(dir)) 
	$(RM) $(BUILD-FILES) $(CLEAN-FILES) .*.P *.a *.o *~ *.taghl

.PRECIOUS: %.o
.PHONY: all clean FORCE
