CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu99
LDFLAGS = -lm

# Source files
PROG = trex
SRCS = main.c state.c play.c draw.c menu.c sprite.c tui.c config.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:%.o=.%.o.d)

# Default verbosity
VERBOSE ?= 0

# Quiet commands
ifeq ($(VERBOSE), 0)
Q = @
else
Q =
endif

# Build rules
.PHONY: all clean

all: $(PROG)

$(PROG): $(OBJS)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	@echo "  CC      $@"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $< -MMD -MF .$@.d

clean:
	@echo "  CLEAN"
	$(Q)rm -f $(PROG) $(OBJS) $(DEPS)

-include $(DEPS)
