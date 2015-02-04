# Cross compile support - create a Makefile which defines these three variables and then includes this Makefile...
CFLAGS  ?= -Wall -fPIC -O2 $(OPTS)
LDFLAGS ?= -lasound -lpthread -lm -lrt
EXECUTABLE ?= squeezelite

# passing one or more of these in $(OPTS) enables optional feature inclusion
OPT_DSD     = -DDSD
OPT_FF      = -DFFMPEG
OPT_LINKALL = -DLINKALL
OPT_RESAMPLE= -DRESAMPLE
OPT_VIS     = -DVISEXPORT
OPT_IR      = -DIR

SOURCES = \
	main.c slimproto.c buffer.c stream.c utils.c \
	output.c output_alsa.c output_pa.c output_stdout.c output_pack.c decode.c \
	flac.c pcm.c mad.c vorbis.c faad.c mpg.c

SOURCES_DSD      = dsd.c dop.c dsd2pcm/dsd2pcm.c
SOURCES_FF       = ffmpeg.c
SOURCES_RESAMPLE = process.c resample.c
SOURCES_VIS      = output_vis.c
SOURCES_IR       = ir.c

LINK_LINUX       = -ldl

LINKALL          = -lFLAC -lmad -lvorbisfile -lfaad -lmpg123
LINKALL_FF       = -lavcodec -lavformat -lavutil
LINKALL_RESAMPLE = -lsoxr
LINKALL_IR       = -llirc_client

DEPS             = squeezelite.h slimproto.h

UNAME            = $(shell uname -s)

# add optional sources
ifneq (,$(findstring $(OPT_DSD), $(CFLAGS)))
	SOURCES += $(SOURCES_DSD)
endif
ifneq (,$(findstring $(OPT_FF), $(CFLAGS)))
	SOURCES += $(SOURCES_FF)
endif
ifneq (,$(findstring $(OPT_RESAMPLE), $(CFLAGS)))
	SOURCES += $(SOURCES_RESAMPLE)
endif
ifneq (,$(findstring $(OPT_VIS), $(CFLAGS)))
	SOURCES += $(SOURCES_VIS)
endif
ifneq (,$(findstring $(OPT_IR), $(CFLAGS)))
	SOURCES += $(SOURCES_IR)
endif

# add optional link options
ifneq (,$(findstring $(OPT_LINKALL), $(CFLAGS)))
	LDFLAGS += $(LINKALL)
ifneq (,$(findstring $(OPT_FF), $(CFLAGS)))
	LDFLAGS += $(LINKALL_FF)
endif
ifneq (,$(findstring $(OPT_RESAMPLE), $(CFLAGS)))
	LDFLAGS += $(LINKALL_RESAMPLE)
endif
ifneq (,$(findstring $(OPT_IR), $(CFLAGS)))
	LDFLAGS += $(LINKALL_IR)
endif
else
# if not LINKALL and linux add LINK_LINUX
ifeq ($(UNAME), Linux)
	LDFLAGS += $(LINK_LINUX)
endif
endif

OBJECTS = $(SOURCES:.c=.o)

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(OBJECTS): $(DEPS)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -c -o $@

clean:
	rm -f $(OBJECTS) $(EXECUTABLE)
