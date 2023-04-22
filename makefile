.PHONY: default dep clean all
default: all

TARGET	?= podfbv

GCC	?= gcc
CFLAGS	+= -O0 -g -Wall -fPIC -pthread
LFLAGS	+= -pthread

FILES	+=

LIBS	+= usb

SRCDIR	?= src
OBJDIR	?= obj

OBJFILES	 = $(FILES:%=$(OBJDIR:%=%/)%.o) $(TARGET:%=$(OBJDIR:%=%/)%.o)
DEPFILES	 = $(OBJFILES:%.o=%.d)

-include	$(DEPFILES)

DIRS	+= $(OBJDIR)

$(DIRS):
	@mkdir -p $@

$(OBJDIR:%=%/)%.d: $(SRCDIR:%=%/)%.c | $(DIRS)
	$(CROSS_COMPILE)$(GCC) $(INCDIRS:%=-I%) $(DEFNS:%=-D%) $(CFLAGS) -M $< -MT $(@:%.d=%.o) -MF $@

$(OBJDIR:%=%/)%.o: $(SRCDIR:%=%/)%.c | $(DIRS)
	$(CROSS_COMPILE)$(GCC) $(INCDIRS:%=-I%) $(DEFNS:%=-D%) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJFILES)
	$(CROSS_COMPILE)$(GCC) $(LIBDIRS:%=-L%) $(LFLAGS) $^ $(LIBS:%=-l%) -o $@

dep: $(DEPFILES)

clean:
	@rm -rf $(OBJFILES) $(DEPFILES)

all: $(TARGET)

run: all
	$(TARGET:%=./%) $(ARGS)

debug: all
	gdb --args $(TARGET:%=./%) $(ARGS)

