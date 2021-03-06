# -*- Makefile -*-

# Copyright (C) 2015 Alexander Chernov <cher@ejudge.ru> */

# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.

ifdef RELEASE
CDEBUGFLAGS=-O2 -s -Wall -DNDEBUG -DRELEASE
else
CDEBUGFLAGS=-g -Wall
endif

CCOMPFLAGS=-mno-cygwin
LDCOMPFLAGS=-mno-cygwin

LDLIBS=${EXTRALIBS} -lm
CFLAGS=-I. ${CDEBUGFLAGS} ${CCOMPFLAGS} ${CEXTRAFLAGS} ${WPTRSIGN}
LDFLAGS=${CDEBUGFLAGS} ${LDCOMPFLAGS} ${LDEXTRAFLAGS}
CC=gcc
LD=gcc
AR=ar

include files.make

OFILES=$(CFILES:.c=.o) testinfo.o
CHKXFILES = $(CHKCFILES:.c=.exe)

TARGETS = checker.lib checker.dll libchecker.a ${CHKXFILES}

all : ${TARGETS}

clean :
	-rm -fr *.o *.a *.dll *.lib *.def *~ *.bak testinfo.h testinfo.c ${CHKXFILES}

distclean : clean
	rm -f Makefile Makefile.in

install : all
	mkdir -p "${DESTDIR}${includedir}"
	for i in checker.h checker_internal.h testinfo.h; do install -m 644 $$i "${DESTDIR}${includedir}"; done
	mkdir -p "${DESTDIR}${libdir}"
	install -m 644 libchecker.a checker.lib checker.dll "${DESTDIR}${libdir}"
libchecker.a checker.dll checker.def : ${OFILES}
	${CC} -mno-cygwin -shared $^ -Wl,--output-def,checker.def,--out-implib,libchecker.a -o checker.dll

checker.lib : checker.def checker.dll
	lib /machine:i386 /def:checker.def

init.o : init.c checker_internal.h testinfo.h

%.o : %.c checker_internal.h
	${CC} ${CFLAGS} -c $< -o $@

testinfo.o: testinfo.c testinfo.h

testinfo.h: ../testinfo.h
	cp -p ../testinfo.h .
testinfo.c: ../testinfo.c
	cp -p ../testinfo.c .

cmp_%.exe : cmp_%.c checker.h checker_internal.h libchecker.a
	${CC} ${CFLAGS} ${LDFLAGS} -L. $< -o $@ -lchecker -lm
