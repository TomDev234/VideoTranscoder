#---Makefile for MorphOS-GCC----------------------------------------------------

CC = ppc-morphos-gcc-15
CFLAGS = -pipe \
         -noixemul -Os -mcpu=G4 -mtune=G4 -mmultiple \
         -ffast-math -fomit-frame-pointer
# Objects first, then the static archives. --start-group lets them
# satisfy each other. The set is the MorphOS build already in lib/.
LDFLAGS = -noixemul
LIBDIR = -L./lib
LIBS = -Wl,--start-group \
    -lavdevice -lavfilter -lavformat -lavcodec -lswscale -lswresample \
    -lavutil -lx264 -lmp3lame -lbz2 -lssl_shared -lcrypto_shared \
    -lz_shared -lpthread -latomic -lfreevec -lm \
    -Wl,--end-group
INCDIR = -I./include
PREFIX = /usr/local

MODEL_SRCS = vt_util.c compat.c profile.c prefs.c job.c
ENGINE_SRCS = vt_avio.c probe.c encoder.c encode_task.c
UI_SRCS = mui_main.c batch_list_class.c main_window_class.c profile_window_class.c \
    job_log_window_class.c
SRCS = $(MODEL_SRCS) $(ENGINE_SRCS) $(UI_SRCS)
OBJS = $(SRCS:.c=.o)
EXE = VideoTranscoder

#-------------------------------------------------------------------------------

.PHONY: all install clean distclean depend

all: $(EXE)

$(EXE): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBDIR) $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INCDIR) -c $< -o $@

$(OBJS): macros.h vt_types.h vt_util.h compat.h profile.h prefs.h job.h vt_ui.h \
    vt_avio.h probe.h encoder.h encode_task.h

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
