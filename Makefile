ifneq ("$(wildcard /usr/include/mosquitto.h)","")
LIBMQTT=-DLIBMQTT -lmosquitto
else
LIBMQTT=
endif

SQLINC=$(shell mysql_config --include)
SQLLIB=$(shell mysql_config --libs)
SQLVER=$(shell mysql_config --version | sed 'sx\..*xx')
CCOPTS=${SQLINC} -I. -I/usr/local/ssl/include -D_GNU_SOURCE -g -Wall -funsigned-char
OPTS=-L/usr/local/ssl/lib ${SQLLIB} ${CCOPTS}

all: git daikinac

SQLlib/sqllib.o: SQLlib/sqllib.c
	make -C SQLlib

daikinac: daikinac.c SQLlib/sqllib.o
	cc -O -o $@ $< ${OPTS} -lpopt ${LIBMQTT} -ISQLlib SQLlib/sqllib.o -lcurl -DSQLLIB

git:
	git submodule update --init

update:
	git submodule update --remote --merge
