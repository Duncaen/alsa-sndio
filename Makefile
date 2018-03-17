PREFIX ?= /usr/local

ALSA_CFLAGS = $(shell pkg-config --cflags alsa)
ALSA_LIBS = $(shell pkg-config --libs alsa)
SIO_LIBS = -lsndio
SOFLAGS = -Wl,-soname -Wl,libasound_module_pcm_sndio.so -Wl,-export-dynamic -Wl,-no-undefined

all: libasound_module_pcm_sndio.so

pcm_sndio.o: pcm_sndio.c
	${CC} -c -fPIC -DPIC ${ALSA_CFLAGS} ${CFLAGS} $<

libasound_module_pcm_sndio.so: pcm_sndio.o
	${CC} -shared ${LDFLAGS} $^ ${LIBS} ${ALSA_LIBS} ${SIO_LIBS} -ldl ${SOFLAGS} -o $@

install: libasound_module_pcm_sndio.so
	install -D -m755 libasound_module_pcm_sndio.so ${DESTDIR}/${PREFIX}/lib/alsa-lib/libasound_module_pcm_sndio.so

clean:
	rm -f *.o *.so

.PONY: all install clean
