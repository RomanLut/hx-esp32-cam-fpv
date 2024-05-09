PROG=pi-button-to-kbd
VERSION=0.0.1

DESTDIR=/

all: $(PROG)

$(PROG): main.c
	gcc -DVERSION=\"$(VERSION)\" -s -Wall -O3 -o $(PROG) main.c

clean:
	rm -f *.o $(PROG)

install: $(PROG)
	strip $(PROG)
	install -m 755 $(PROG) $(DESTDIR)/usr/bin


