TARGET=dkrfs
LIBS=-lfuse -lpthread -lnetsnmp
CFLAGS=-g -Wall -I. -I/usr/include -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=29

OBJECTS=$(patsubst %.c, %.o, $(wildcard *.c))
HEADERS=$(wildcard *.h)

.PHONY: default all clean install

default: $(TARGET)

all: default

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LIBS) -o $@

%.o: %.c $(HEADERS)
	$(CC) -c $(CFLAGS) $<

install: all
	mkdir -p $(PREFIX)/bin
	cp -a $(TARGET) $(PREFIX)/bin/

clean:
	rm -f *.o $(TARGET) $(OBJECTS)
