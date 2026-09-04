# NX-Nexus -- host-side unit tests for the container parsers and the installer.
#
# These build with the system compiler, not devkitA64: the code under test
# depends only on nexus/nx_types.h and never on libnx, which is what makes it
# testable off-console. Run with `make -C tests run` or `./scripts/test.sh`.
#
# Object files go under obj/ rather than beside their sources. That matters:
# the devkitPro Makefile locates sources through VPATH, and a stray host .o
# sitting next to a .c in src/format/ would be picked up as an already-built
# object for the ARM link, which fails in a thoroughly confusing way.

CC      ?= cc
CFLAGS  := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -I../include -I.
LDFLAGS :=

# Address and UB sanitizers are the point of running these on the host: this
# code exists to survive hostile input, and ASan turns a missed bounds check
# into a hard failure instead of a silent read. Disable with SANITIZE=0 on
# toolchains that lack them.
SANITIZE ?= 1
ifeq ($(SANITIZE),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

OBJDIR := obj
TARGET := nexus-tests

TEST_SRCS := test_main.c fixtures.c test_partition_fs.c test_cnmt.c \
             test_ticket.c test_installer.c test_nsp_builder.c test_json.c test_sanitise.c

FMT_SRCS  := format.c partition_fs.c cnmt.c ticket.c nsp_builder.c json.c
INST_SRCS := installer.c

OBJS := $(addprefix $(OBJDIR)/,$(TEST_SRCS:.c=.o)) \
        $(addprefix $(OBJDIR)/format_,$(FMT_SRCS:.c=.o)) \
        $(addprefix $(OBJDIR)/installer_,$(INST_SRCS:.c=.o))

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/format_%.o: ../src/format/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/installer_%.o: ../src/installer/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET)
