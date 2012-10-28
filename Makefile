CFLAGS32  = -Wall -fPIC -O2
CFLAGS64  = -Wall -fPIC -m64

LDFLAGS32 = -lasound -lpthread

all: squeezelite

squeezelite: main.o slimproto.o utils.o output.o buffer.o stream.o decode.o flac.o pcm.o mad.o
	$(CC) $(CFLAGS32) main.o slimproto.o utils.o output.o buffer.o stream.o decode.o flac.o pcm.o mad.o -o squeezelite $(LDFLAGS32) 

main.o: main.c squeezelite.h
	$(CC) $(CFLAGS32) -c main.c -o main.o

slimproto.o: slimproto.c squeezelite.h slimproto.h
	$(CC) $(CFLAGS32) -c slimproto.c -o slimproto.o

utils.o: utils.c squeezelite.h
	$(CC) $(CFLAGS32) -c utils.c -o utils.o

output.o: output.c squeezelite.h
	$(CC) $(CFLAGS32) -c output.c -o output.o

buffer.o: buffer.c squeezelite.h
	$(CC) $(CFLAGS32) -c buffer.c -o buffer.o

stream.o: stream.c squeezelite.h
	$(CC) $(CFLAGS32) -c stream.c -o stream.o

decode.o: decode.c squeezelite.h
	$(CC) $(CFLAGS32) -c decode.c -o decode.o

flac.o: flac.c squeezelite.h
	$(CC) $(CFLAGS32) -c flac.c -o flac.o

pcm.o: pcm.c squeezelite.h
	$(CC) $(CFLAGS32) -c pcm.c -o pcm.o

mad.o: mad.c squeezelite.h
	$(CC) $(CFLAGS32) -c mad.c -o mad.o

