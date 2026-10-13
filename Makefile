#---Makefile for MorphOS-GCC----------------------------------------------------

CC = ppc-morphos-gcc-15
CFLAGS = -pipe \
         -noixemul -Os -mcpu=G4 -mtune=G4 -mmultiple \
         -ffast-math -fomit-frame-pointer
LDFLAGS = -noixemul
INCDIR = -I./include
PREFIX = /usr/local

# Phase 1 is the model layer. The MUI sources and the application link
# are added in later phases, when mui_main.c exists.
MODEL_SRCS = vt_util.c compat.c profile.c prefs.c job.c
MODEL_OBJS = $(MODEL_SRCS:.c=.o)
EXE = VideoTranscoder

#-------------------------------------------------------------------------------

.PHONY: all models install clean distclean depend

all: models

models: $(MODEL_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCDIR) -c $< -o $@

$(MODEL_OBJS): vt_types.h vt_util.h compat.h profile.h prefs.h job.h

install: $(EXE)
	strip $(EXE)
	cp $(EXE) $(PREFIX)/bin/

clean:
	$(RM) $(MODEL_OBJS)

distclean: clean
	$(RM) $(EXE) .depend

depend:
	$(CC) -MM $(CFLAGS) $(INCDIR) $(MODEL_SRCS) > .depend

# --- include dependency files if they exist ---
ifneq ($(wildcard .depend),)
include .depend
endif
