CC = gcc
CFLAGS ?= -O2
WARN = -Wall -Wextra -pedantic -ansi
PREFIX ?= /usr/local
OBJS = main.o sysmon.o proc.o config.o ui.o

.PHONY: clean install uninstall

sysmon: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o sysmon

%.o: %.c
	$(CC) $(CFLAGS) $(WARN) -c $<

main.o: main.c sysmon.h proc.h config.h ui.h
sysmon.o: sysmon.c sysmon.h
proc.o: proc.c proc.h sysmon.h
config.o: config.c config.h proc.h
ui.o: ui.c ui.h proc.h sysmon.h

install: sysmon
	install -Dm755 sysmon $(DESTDIR)$(PREFIX)/bin/sysmon
	install -Dm644 sysmon.conf.example $(DESTDIR)$(PREFIX)/share/doc/sysmon/sysmon.conf.example
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/sysmon/README.md

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/sysmon
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/sysmon

clean:
	rm -f *.o sysmon
