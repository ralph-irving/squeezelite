CFLAGS  = -Wall -fPIC -O2
LDFLAGS = -lasound -lpthread

all: squeezelite

squeezelite: main.o slimproto.o utils.o output.o buffer.o stream.o decode.o flac.o pcm.o mad.o
	$(CC) $(CFLAGS) main.o slimproto.o utils.o output.o buffer.o stream.o decode.o flac.o pcm.o mad.o -o squeezelite $(LDFLAGS) 

main.o: main.c squeezelite.h
	$(CC) $(CFLAGS) -c main.c -o main.o

slimproto.o: slimproto.c squeezelite.h slimproto.h
	$(CC) $(CFLAGS) -c slimproto.c -o slimproto.o

utils.o: utils.c squeezelite.h
	$(CC) $(CFLAGS) -c utils.c -o utils.o

output.o: output.c squeezelite.h
	$(CC) $(CFLAGS) -c output.c -o output.o

buffer.o: buffer.c squeezelite.h
	$(CC) $(CFLAGS) -c buffer.c -o buffer.o

stream.o: stream.c squeezelite.h
	$(CC) $(CFLAGS) -c stream.c -o stream.o

decode.o: decode.c squeezelite.h
	$(CC) $(CFLAGS) -c decode.c -o decode.o

flac.o: flac.c squeezelite.h
	$(CC) $(CFLAGS) -c flac.c -o flac.o

pcm.o: pcm.c squeezelite.h
	$(CC) $(CFLAGS) -c pcm.c -o pcm.o

mad.o: mad.c squeezelite.h
	$(CC) $(CFLAGS) -c mad.c -o mad.o

