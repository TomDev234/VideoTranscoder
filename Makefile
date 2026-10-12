#---Makefile for MorphOS-GCC----------------------------------------------------

CC = ppc-morphos-gcc-15
CFLAGS = -pipe \
         -noixemul -Os -mcpu=G4 -mtune=G4 -mmultiple \
         -ffast-math -fomit-frame-pointer
LDFLAGS = -noixemul
INCDIR = -I./include
PREFIX = /usr/local
SRCS = mui_main.c mui_class1.c
OBJS = $(SRCS:.c=.o)
EXE = VideoTranscoder

#-------------------------------------------------------------------------------

.PHONY: all install clean distclean depend

all: $(EXE)

$(EXE): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INCDIR) -c $< -o $@

install: $(EXE)
	strip $(EXE)
	cp $(EXE) $(PREFIX)/bin/

clean:
	$(RM) $(OBJS)

distclean: clean
	$(RM) $(EXE) .depend

depend:
	$(CC) -MM $(CFLAGS) $(INCDIR) $(SRCS) > .depend

# --- include dependency files if they exist ---
ifneq ($(wildcard .depend),)
include .depend
endif