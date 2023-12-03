SRCDIR := src
OBJDIR := obj

PKGCONFIG = $(shell which pkg-config)
CFLAGS := -g -Wall -Wextra -Wshadow -Wundef

LIBS   :=
CC := cc

$(shell mkdir -p $(OBJDIR))
NAME := $(shell basename $(shell pwd))

# recursive find of source files
SOURCES     := $(shell find $(SRCDIR) -type f -name *.c)

# create object files in separate directory
OBJECTS := $(SOURCES:%.c=$(OBJDIR)/%.o)

all: $(OBJECTS)
	@echo "== LINKING EXECUTABLE: $(NAME)"
	$(CC) $^ $(CFLAGS) $(LIBS) $(LDLIBS) -o $@ -o $(NAME)

$(OBJDIR)/%.o: %.c
	@echo "== COMPILING SOURCE $< --> OBJECT $@"
	@mkdir -p '$(@D)'
	$(CC) -I$(SRCDIR) $(CFLAGS) $(LIBS) $(LDLIBS) -c $< -o $@
