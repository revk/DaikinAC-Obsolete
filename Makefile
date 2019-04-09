ifneq ("$(wildcard /usr/include/mosquitto.h)","")
LIBMQTT=-DLIBMQTT -lmosquitto
else
LIBMQTT=
endif

ifneq ("$(wildcard /usr/include/net-snmp/session_api.h)","")
LIBSNMP=-DLIBSNMP -lsnmp
else
LIBSNMP=
endif

SQLINC=$(shell mysql_config --include)
SQLLIB=$(shell mysql_config --libs)
SQLVER=$(shell mysql_config --version | sed 'sx\..*xx')
CCOPTS=${SQLINC} -I. -I/usr/local/ssl/include -D_GNU_SOURCE -g -Wall -funsigned-char -lm
OPTS=-L/usr/local/ssl/lib ${SQLLIB} ${CCOPTS}

all: git daikinac

SQLlib/sqllib.o: SQLlib/sqllib.c
	make -C SQLlib
AXL/axl.o: AXL/axl.c
	make -C AXL

daikinac: daikinac.c SQLlib/sqllib.o AXL/axl.o
	cc -O -o $@ $< ${OPTS} -lpopt ${LIBMQTT} ${LIBSNMP} -ISQLlib SQLlib/sqllib.o -lcurl -DSQLLIB -IAXL AXL/axl.o

git:
	git submodule update --init

update:
	git submodule update --remote --merge
