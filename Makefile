#---Makefile for MorphOS-GCC----------------------------------------------------

CC = ppc-morphos-gcc-15
CFLAGS = -pipe \
         -noixemul -Os -mcpu=G4 -mtune=G4 -mmultiple \
         -ffast-math -fomit-frame-pointer
LDFLAGS = -noixemul -L./lib \
    -lavdevice -lavfilter -lavformat -lavcodec -lswscale -lswresample \
    -lavutil -lx264 -lmp3lame -lbz2 -lssl_shared -lcrypto_shared \
    -lz_shared -lpthread -latomic -lmotovec
INCDIR = -I./include
PREFIX = /usr/local

# Phase 2 links the MUI shell. libav is linked when the encoder arrives.
MODEL_SRCS = vt_util.c compat.c profile.c prefs.c job.c
UI_SRCS = mui_main.c batch_list_class.c main_window_class.c profile_window_class.c
SRCS = $(MODEL_SRCS) $(UI_SRCS)
OBJS = $(SRCS:.c=.o)
EXE = VideoTranscoder

#-------------------------------------------------------------------------------

.PHONY: all install clean distclean depend

all: $(EXE)

$(EXE): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INCDIR) -c $< -o $@

$(OBJS): macros.h vt_types.h vt_util.h compat.h profile.h prefs.h job.h vt_ui.h

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
