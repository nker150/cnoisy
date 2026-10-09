PROG= noisy
SRCS= noisy.c
OBJS= ${SRCS:.c=.o}
CFLAGS+= -std=c99 -Wall -Wextra -Wpedantic
PKG_CONFIG?= pkg-config
PYTHON?= python3
PREFIX?= /usr/local
BINDIR?= ${PREFIX}/bin
MANDIR?= ${PREFIX}/man/man1
DESTDIR?=

all: ${PROG}

${PROG}: ${OBJS}
	${CC} ${LDFLAGS} -o $@ ${OBJS} `${PKG_CONFIG} --libs libcurl jansson`

.c.o:
	${CC} ${CPPFLAGS} ${CFLAGS} `${PKG_CONFIG} --cflags libcurl jansson` -c $< -o $@

clean:
	rm -f ${PROG} ${OBJS}

test: ${PROG}
	${PYTHON} -m unittest discover -s tests

install: ${PROG}
	install -d -m 755 "${DESTDIR}${BINDIR}"
	install -c -m 755 ${PROG} "${DESTDIR}${BINDIR}/${PROG}"
	install -d -m 755 "${DESTDIR}${MANDIR}"
	install -c -m 644 noisy.1 "${DESTDIR}${MANDIR}/noisy.1"

uninstall:
	rm -f "${DESTDIR}${BINDIR}/${PROG}"
	rm -f "${DESTDIR}${MANDIR}/noisy.1"

.PHONY: all clean install test uninstall
