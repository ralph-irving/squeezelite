#Cross compile support - create a Makefile which defines these three variables and then includes this Makefile...
CFLAGS	?= -Wall -fPIC -O2
CFLAGS	+= -fcommon
LDADD	?= -lpthread -lm -lrt
EXECUTABLE ?= squeezelite

# passing one or more of these in $(OPTS) enables optional feature inclusion
OPT_DSD     = -DDSD
OPT_FF      = -DFFMPEG
OPT_ALAC    = -DALAC
OPT_LINKALL = -DLINKALL
OPT_RESAMPLE= -DRESAMPLE
OPT_VIS     = -DVISEXPORT
OPT_IR      = -DIR
OPT_GPIO    = -DGPIO
OPT_RPI     = -DRPI
OPT_NO_FAAD = -DNO_FAAD
OPT_SSL	    = -DUSE_SSL
OPT_NOSSLSYM= -DNO_SSLSYM
OPT_OPUS    = -DOPUS
OPT_PORTAUDIO = -DPORTAUDIO
OPT_PULSEAUDIO = -DPULSEAUDIO

SOURCES = \
	main.c slimproto.c buffer.c stream.c utils.c \
	output.c output_alsa.c output_pa.c output_stdout.c output_pack.c output_pulse.c decode.c \
	flac.c pcm.c mad.c vorbis.c mpg.c

SOURCES_DSD      = dsd.c dop.c dsd2pcm/dsd2pcm.c
SOURCES_FF       = ffmpeg.c
SOURCES_ALAC     = alac.c alac_wrapper.cpp
SOURCES_RESAMPLE = process.c resample.c
SOURCES_VIS      = output_vis.c
SOURCES_IR       = ir.c
SOURCES_GPIO     = gpio.c
SOURCES_RPI      = minimal_gpio.c
SOURCES_FAAD     = faad.c
SOURCES_SSL      = sslsym.c
SOURCES_OPUS     = opus.c

LINK_LINUX       = -ldl
LINK_ALSA        = -lasound
LINK_PORTAUDIO   = -lportaudio
LINK_PULSEAUDIO  = -lpulse
LINK_SSL         = -lssl -lcrypto
LINK_ALAC        = -lalac

LINKALL          = -lmad -lmpg123 -lFLAC -lvorbisfile -lvorbis -logg
LINKALL_FF       = -lavformat -lavcodec -lavutil
LINKALL_RESAMPLE = -lsoxr
LINKALL_IR       = -llirc_client
LINKALL_FAAD     = -lfaad
LINKALL_OPUS     = -lopusfile -lopus

DEPS             = squeezelite.h slimproto.h

UNAME            = $(shell uname -s)

# add optional sources
ifneq (,$(findstring $(OPT_DSD), $(OPTS)))
	SOURCES += $(SOURCES_DSD)
endif
ifneq (,$(findstring $(OPT_FF), $(OPTS)))
	SOURCES += $(SOURCES_FF)
endif
ifneq (,$(findstring $(OPT_ALAC), $(OPTS)))
	SOURCES += $(SOURCES_ALAC)
	DEPS += alac_wrapper.h
endif
ifneq (,$(findstring $(OPT_OPUS), $(OPTS)))
	SOURCES += $(SOURCES_OPUS)
endif
ifneq (,$(findstring $(OPT_RESAMPLE), $(OPTS)))
	SOURCES += $(SOURCES_RESAMPLE)
endif
ifneq (,$(findstring $(OPT_VIS), $(OPTS)))
	SOURCES += $(SOURCES_VIS)
endif
ifneq (,$(findstring $(OPT_IR), $(OPTS)))
	SOURCES += $(SOURCES_IR)
endif
ifneq (,$(findstring $(OPT_GPIO), $(OPTS)))
	SOURCES += $(SOURCES_GPIO)
endif
ifneq (,$(findstring $(OPT_RPI), $(OPTS)))
	SOURCES += $(SOURCES_RPI)
endif
# ensure GPIO is enabled with RPI
ifneq (,$(findstring $(OPT_RPI), $(OPTS)))
ifeq (,$(findstring $(SOURCES_GPIO), $(SOURCES)))
	CFLAGS += $(OPT_GPIO)
	SOURCES += $(SOURCES_GPIO)
endif
endif
ifeq (,$(findstring $(OPT_NO_FAAD), $(OPTS)))
	SOURCES += $(SOURCES_FAAD)
endif
ifneq (,$(findstring $(OPT_SSL), $(OPTS)))
	SOURCES += $(SOURCES_SSL)
endif

# add optional link options
ifneq (,$(findstring $(OPT_LINKALL), $(OPTS)))
	LDADD += $(LINKALL)
ifneq (,$(findstring $(OPT_FF), $(OPTS)))
	LDADD += $(LINKALL_FF)
endif
ifneq (,$(findstring $(OPT_OPUS), $(OPTS)))
	LDADD += $(LINKALL_OPUS)
endif
ifneq (,$(findstring $(OPT_RESAMPLE), $(OPTS)))
	LDADD += $(LINKALL_RESAMPLE)
endif
ifneq (,$(findstring $(OPT_IR), $(OPTS)))
	LDADD += $(LINKALL_IR)
endif
ifeq (,$(findstring $(OPT_NO_FAAD), $(OPTS)))
	LDADD += $(LINKALL_FAAD)
endif	
ifneq (,$(findstring $(OPT_SSL), $(OPTS)))
	LDADD += $(LINK_SSL)
endif
else
# if not LINKALL and linux add LINK_LINUX
ifeq ($(UNAME), Linux)
	LDADD += $(LINK_LINUX)
endif
ifneq (,$(findstring $(OPT_NOSSLSYM), $(OPTS)))
	LDADD += $(LINK_SSL)
endif
endif

ifneq (,$(findstring $(OPT_PULSEAUDIO), $(OPTS)))
	LDADD += $(LINK_PULSEAUDIO)
else ifneq (,$(findstring $(OPT_PORTAUDIO), $(OPTS)))
	LDADD += $(LINK_PORTAUDIO)
else
	LDADD += $(LINK_ALSA)
endif

ifneq (,$(findstring $(OPT_ALAC), $(OPTS)))
	LDADD += $(LINK_ALAC)
endif

OBJECTS = $(addsuffix .o,$(basename $(SOURCES)))

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
ifneq (,$(findstring $(OPT_ALAC), $(OPTS)))
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LDADD) -o $@
else
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDADD) -o $@
endif

$(OBJECTS): $(DEPS)

.cpp.o:
	$(CXX) $(CXXFLAGS) $(CFLAGS) $(CPPFLAGS) $(OPTS) -Wno-multichar $< -c -o $@

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) $(OPTS) $< -c -o $@

clean:
	rm -f $(OBJECTS) $(EXECUTABLE)

print-%:
	@echo $* = $($*)
