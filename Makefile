CFLAGS += -std=c89 -Wall -Wextra -pedantic-errors ${INCLUDES}

SRCFILES = intv.c parseconfig.c rotator.c snaps.c strv.c syncer.c util.c

FETCH=ftp
CKSUM=sha256

HRSYNCV=1.0.1

all:
	make build

hrsync.tar.gz:
	${FETCH} https://github.com/timkuijsten/hrsync/archive/refs/tags/v${HRSYNCV}.tar.gz
	mv v${HRSYNCV}.tar.gz hrsync.tar.gz

hrsync: hrsync.tar.gz
	${CKSUM} -c SHA256SUMS
	tar zxf hrsync.tar.gz
	mv hrsync-${HRSYNCV} hrsync
	# let hrsync patch rsync
	cd hrsync && make rsync-3.1.3
	# then apply our own patches on top of it
	patch hrsync/rsync-3.1.3/rsync.c	patch-hrsync-rsync_c
	patch hrsync/rsync-3.1.3/util.c	patch-hrsync-util_c

prsync: hrsync
	cd hrsync && make
	cp hrsync/hrsync prsync

y.tab.c: scfg.y
	yacc scfg.y

snaps: *.[ch]
	cc -Wall -g strv.c intv.c util.c y.tab.c rotator.c syncer.c \
		parseconfig.c snaps.c -o snaps

build: snaps prsync

install: snaps prsync
	install -m 0555 -g bin snaps prsync /usr/local/sbin
	install -m 0444 -g bin snaps.8 /usr/local/man/man8
	install -m 0444 -g bin snaps.conf.5 /usr/local/man/man5
	install -m 0640 snaps.conf.example /etc/examples/snaps.conf

lint:
	${CC} ${CFLAGS} -fsyntax-only ${SRCFILES} 2>&1

clean:
	rm -f snaps prsync y.tab.c y.output tutil tscfg
	rm -rf hrsync/ /tmp/snapstestutil

docs:
	mandoc -T html -Ostyle=man.css snaps.8 > snaps.8.html
	mandoc -T html -Ostyle=man.css snaps.conf.5 > snaps.conf.5.html

# test utilities
tutil: strv.c intv.c util.c test/util.c
	rm -rf /tmp/snapstestutil
	mkdir /tmp/snapstestutil
	cc -Wall -g strv.c intv.c util.c test/util.c -o tutil

# test simple config file parser
tscfg: scfg.y scfg.h strv.c strv.h intv.c intv.h test/scfg.c
	yacc -tv scfg.y
	cc -Wall -g strv.c intv.c y.tab.c test/scfg.c -o tscfg
	./tscfg

runtests: tutil tscfg
	./tutil
	./tscfg
