# Cross compile support - create a Makefile which defines these three variables and then includes this Makefile...
CFLAGS  ?= -Wall -fPIC -O2 $(OPTS)
LDFLAGS ?= -lasound -lpthread -ldl -lrt
EXECUTABLE ?= squeezelite

SOURCES = main.c slimproto.c utils.c output.c buffer.c stream.c decode.c flac.c pcm.c mad.c vorbis.c faad.c
DEPS    = squeezelite.h

OBJECTS = $(SOURCES:.c=.o)

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(OBJECTS): $(DEPS)

.c.o:
	$(CC) $(CFLAGS) $< -c -o $@

clean:
	rm -f $(OBJECTS) $(EXECUTABLE)
