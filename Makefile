CFLAGS += -std=c89 -Wall -Wextra -pedantic-errors ${INCLUDES}

SRCFILES = intv.c parseconfig.c rotator.c snaps.c strv.c syncer.c util.c

ETCDIR = /etc
PREFIX = /usr/local
BINDIR = ${PREFIX}/sbin
MANDIR = ${PREFIX}/man

INSTALL_DIR = install -dm 755
INSTALL_ETC = install -m 0640
INSTALL_BIN = install -m 0555 -g bin
INSTALL_MAN = install -m 0444

FETCH=ftp
CKSUM=sha256

HRSYNCVER=1.0.1
HRSYNCDIR=hrsync-${HRSYNCVER}
HRSYNCBIN=${HRSYNCDIR}/hrsync

all: snaps prsync

snaps: snaps.o strv.o intv.o util.o rotator.o syncer.o parseconfig.o y.tab.o
	${CC} ${CFLAGS} -o $@ snaps.o strv.o intv.o util.o rotator.o syncer.o \
	    parseconfig.o y.tab.o ${LDFLAGS}

# currently scfg.y has an anonymous union that should be removed for c89
# compatibility
y.tab.o: y.tab.c
	${CC} ${CFLAGS} -std=c11 -c y.tab.c

.SUFFIXES: .c .o
.c.o:
	${CC} ${CFLAGS} -c $<

y.tab.c: scfg.y
	yacc scfg.y

lint:
	${CC} ${CFLAGS} -fsyntax-only ${SRCFILES} 2>&1

v${HRSYNCVER}.tar.gz:
	${FETCH} https://github.com/timkuijsten/hrsync/archive/refs/tags/v${HRSYNCVER}.tar.gz

${HRSYNCBIN}: v${HRSYNCVER}.tar.gz
	ln v${HRSYNCVER}.tar.gz hrsync.tar.gz
	${CKSUM} -c SHA256SUMS
	tar zxf hrsync.tar.gz
	# first let hrsync patch rsync, then we patch and build
	cd ${HRSYNCDIR} && make rsync-3.1.3
	patch ${HRSYNCDIR}/rsync-3.1.3/rsync.c patch-hrsync-rsync_c
	patch ${HRSYNCDIR}/rsync-3.1.3/util.c  patch-hrsync-util_c
	cd ${HRSYNCDIR} && make

prsync: ${HRSYNCBIN}
	cp ${HRSYNCBIN} prsync

install: all
	${INSTALL_DIR} ${DESTDIR}${ETCDIR}
	${INSTALL_DIR} ${DESTDIR}${BINDIR}
	${INSTALL_DIR} ${DESTDIR}${MANDIR}/man5
	${INSTALL_DIR} ${DESTDIR}${MANDIR}/man8
	${INSTALL_ETC} snaps.conf.example ${DESTDIR}${ETCDIR}
	${INSTALL_BIN} prsync snaps ${DESTDIR}${BINDIR}
	${INSTALL_MAN} snaps.conf.5 ${DESTDIR}${MANDIR}/man5
	${INSTALL_MAN} snaps.8 ${DESTDIR}${MANDIR}/man8

clean:
	rm -f v${HRSYNCVER}.tar.gz snaps prsync *.o y.tab.c y.output tutil \
	    tscfg hrsync.tar.gz
	rm -rf ${HRSYNCDIR}/ /tmp/snapstestutil

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
