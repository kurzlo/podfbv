.PHONY: default dep clean all
default: all

TARGET	?= podfbv

GCC	?= gcc
CFLAGS	+= -O0 -g -Wall -fPIC 

ifeq ($(API),win)
CROSS_COMPILE	?= x86_64-w64-mingw32-
LIBS	+= winmm
EXT		?= exe
DEFNS	+= API_WIN
else
CFLAGS	+= -pthread
LFLAGS	+= -pthread
#LIBS	+= usb
endif

FILES	+=

SRCDIR	?= src
OBJDIR	?= obj$(CROSS_COMPILE:%-=/%)

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

$(TARGET:%=%$(EXT:%=.%)): $(OBJFILES)
	$(CROSS_COMPILE)$(GCC) $(LIBDIRS:%=-L%) $(LFLAGS) $^ $(LIBS:%=-l%) -o $@

dep: $(DEPFILES)

clean:
	@rm -rf $(OBJFILES) $(DEPFILES)

all: $(TARGET:%=%$(EXT:%=.%))

run: all
	$(TARGET:%=./%$(EXT:%=.%)) $(ARGS)

debug: all
	gdb --args $(TARGET:%=./%$(EXT:%=.%)) $(ARGS)

