/*
 * Copyright (c) 2008-2012 Alexandre Ratchov <alex@caoua.org>
 * Copyright (c) 2018      Duncan Overbruck <mail@duncano.de>
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <stdio.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include <sndio.h>

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))

static snd_pcm_format_t cap_fmts[] = {
	/* XXX add s24le3 and s24be3 */
	SND_PCM_FORMAT_S32_LE,	SND_PCM_FORMAT_S32_BE,
	SND_PCM_FORMAT_S24_LE,	SND_PCM_FORMAT_S24_BE,
	SND_PCM_FORMAT_S16_LE,	SND_PCM_FORMAT_S16_BE,
	SND_PCM_FORMAT_U8
};

static snd_pcm_access_t cap_access[] = {
	SND_PCM_ACCESS_RW_INTERLEAVED,
};

typedef struct snd_pcm_sndio {
	snd_pcm_ioplug_t io;

	struct sio_hdl *hdl;
	struct sio_par par;

	unsigned int bpf;

	snd_pcm_sframes_t ptr;
	snd_pcm_sframes_t realptr;

	int started;
} snd_pcm_sndio_t;

static void sndio_free(snd_pcm_sndio_t *);

static snd_pcm_sframes_t
sndio_write(snd_pcm_ioplug_t *io,
    const snd_pcm_channel_area_t *areas,
    snd_pcm_uframes_t offset,
    snd_pcm_uframes_t size)
{
	snd_pcm_sndio_t *sndio = io->private_data;
	unsigned int bufsz, n;
	char *buf;

	n = 0;
	buf = (char *)areas->addr + (areas->first + areas->step * offset) / 8;
	bufsz = (size * sndio->bpf);

	if (io->stream == SND_PCM_STREAM_PLAYBACK) {
		if ((n = sio_write(sndio->hdl, buf, bufsz)) <= 0) {
			if (sio_eof(sndio->hdl) == 1)
				return -EIO;
			return n;
		}
	} else {
		if ((n = sio_read(sndio->hdl, buf, bufsz)) <= 0) {
			if (sio_eof(sndio->hdl) == 1)
				return -EIO;
			return n;
		}
	}

	sndio->ptr += n / sndio->bpf;
	return n / sndio->bpf;
}

static int
sndio_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
	snd_pcm_sndio_t *sndio = io->private_data;
	*delayp = sndio->ptr - sndio->realptr;
	return 0;
}

static snd_pcm_sframes_t
sndio_pointer(snd_pcm_ioplug_t *io)
{
	snd_pcm_sndio_t *sndio = io->private_data;
	return sndio->ptr + (io->stream == SND_PCM_STREAM_CAPTURE) * io->buffer_size;
}

static int
sndio_start(snd_pcm_ioplug_t *io)
{
	return 0;
}

static int
sndio_stop(snd_pcm_ioplug_t *io)
{
	snd_pcm_sndio_t *sndio = io->private_data;
	if (sndio->started) {
		sio_stop(sndio->hdl);
		sndio->started = 0;
	}
	return 0;
}

static int
sndio_close(snd_pcm_ioplug_t *io)
{
	snd_pcm_sndio_t *sndio = io->private_data;
	if (sndio->started)
		sndio_stop(io);
	sndio_free(sndio);
	return 0;
}

static int
sndio_drain(snd_pcm_ioplug_t *io)
{
	snd_pcm_sndio_t *sndio = io->private_data;
	sio_stop(sndio->hdl);
	return 0;
}

static int
sndio_prepare(snd_pcm_ioplug_t *io)
{
	snd_pcm_sndio_t *sndio = io->private_data;

	sndio->ptr = 0;
	sndio->realptr = 0;

	sndio_stop(io);

	if (sio_start(sndio->hdl) == 0) {
		if (sio_eof(sndio->hdl) == 1)
			return -EBADFD;
		return -EAGAIN;
	}
	sndio->started = 1;

	return 0;
}

static int
sndio_hw_constraint(snd_pcm_sndio_t *sndio)
{
	struct sio_cap cap;

	snd_pcm_ioplug_t *io = &sndio->io;

	unsigned int cap_rates[SIO_NRATE];
	unsigned int cap_chans[SIO_NCHAN];

	int err, i, chan;
	int nchan = 0;
	int nrate = 0;

	err = sio_getcap(sndio->hdl, &cap);
	if (err == 0)
		return -EINVAL;

	err = snd_pcm_ioplug_set_param_list(io,
	    SND_PCM_IOPLUG_HW_ACCESS,
	    ARRAY_SIZE(cap_access),
	    cap_access);
	if(err < 0)
		return err;

	err = snd_pcm_ioplug_set_param_list(io,
	    SND_PCM_IOPLUG_HW_FORMAT,
	    ARRAY_SIZE(cap_fmts),
	    (unsigned int *)cap_fmts);
	if(err < 0)
		return err;

	chan = (io->stream == SND_PCM_STREAM_PLAYBACK) ? cap.confs[0].pchan : cap.confs[0].rchan;
	for (i = 0; i < SIO_NCHAN; i++) {
		if ((chan & (1 << i)) == 0)
			continue;
		cap_chans[nchan++] = (io->stream == SND_PCM_STREAM_PLAYBACK) ? cap.pchan[i] : cap.rchan[i];
	}
	err = snd_pcm_ioplug_set_param_list(io,
	    SND_PCM_IOPLUG_HW_CHANNELS,
	    ARRAY_SIZE(cap_chans),
	    cap_chans-1);
	if (err < 0)
		return err;

	for (i = 0; i < SIO_NRATE; i++) {
		if ((cap.confs[0].rate & (1 << i)) == 0)
			continue;
		cap_rates[nrate++] = cap.rate[i];
	}
	err = snd_pcm_ioplug_set_param_list(io,
	    SND_PCM_IOPLUG_HW_RATE,
	    nrate-1,
	    cap_rates);
	if (err < 0)
		return err;

	err = snd_pcm_ioplug_set_param_minmax(io,
	    SND_PCM_IOPLUG_HW_BUFFER_BYTES,
	    64,
		4 * 1024 * 1024);
	if (err < 0)
		return err;

	err = snd_pcm_ioplug_set_param_minmax(io,
	    SND_PCM_IOPLUG_HW_PERIOD_BYTES,
	    64,
		2 * 1024 * 1024);
	if (err < 0)
		return err;

	err = snd_pcm_ioplug_set_param_minmax(io,
	    SND_PCM_IOPLUG_HW_PERIODS,
	    1,
	    2048);
	if(err < 0)
		return err;

	return 0;
}

static int
sndio_alsa_fmttopar(snd_pcm_format_t fmt,
    unsigned int *bits, unsigned int *sig, unsigned int *le)
{
	switch (fmt) {
	case SND_PCM_FORMAT_U8:
		*bits = 8;
		*sig = 0;
		break;
	case SND_PCM_FORMAT_S8:
		*bits = 8;
		*sig = 1;
		break;
	case SND_PCM_FORMAT_S16_LE:
		*bits = 16;
		*sig = 1;
		*le = 1;
		break;
	case SND_PCM_FORMAT_S16_BE:
		*bits = 16;
		*sig = 1;
		*le = 0;
		break;
	case SND_PCM_FORMAT_U16_LE:
		*bits = 16;
		*sig = 0;
		*le = 1;
		break;
	case SND_PCM_FORMAT_U16_BE:
		*bits = 16;
		*sig = 0;
		*le = 0;
		break;
	case SND_PCM_FORMAT_S24_LE:
		*bits = 24;
		*sig = 1;
		*le = 1;
		break;
	case SND_PCM_FORMAT_S24_BE:
		*bits = 24;
		*sig = 1;
		*le = 0;
		break;
	case SND_PCM_FORMAT_U24_LE:
		*bits = 24;
		*sig = 0;
		*le = 1;
		break;
	case SND_PCM_FORMAT_U24_BE:
		*bits = 24;
		*sig = 0;
		*le = 0;
		break;
	case SND_PCM_FORMAT_S32_LE:
		*bits = 32;
		*sig = 1;
		*le = 1;
		break;
	case SND_PCM_FORMAT_S32_BE:
		*bits = 32;
		*sig = 1;
		*le = 0;
		break;
	case SND_PCM_FORMAT_U32_LE:
		*bits = 32;
		*sig = 0;
		*le = 1;
		break;
	case SND_PCM_FORMAT_U32_BE:
		*bits = 32;
		*sig = 0;
		*le = 0;
		break;
	default:
		SNDERR("sndio: sndio_alsa_fmttopar: 0x%x: unsupported format\n", fmt);
		return 0;
	}
	return 1;
}

static int
sndio_hw_params(snd_pcm_ioplug_t *io,
    snd_pcm_hw_params_t *params)
{
	snd_pcm_sndio_t *sndio = io->private_data;
	struct sio_par *par, retpar;

	par = &sndio->par;
	par->pchan = io->channels;
	par->rchan = io->channels;

	if (sndio_alsa_fmttopar(io->format, &par->bits, &par->sig, &par->le) == 0)
		return -EINVAL;

	sndio->bpf =
		((snd_pcm_format_physical_width(io->format) * io->channels) / 8);

	par->bps = SIO_BPS(par->bits);
	par->rate = io->rate;
	par->appbufsz = io->buffer_size;

	if (sio_setpar(sndio->hdl, par) == 0 ||
	    sio_getpar(sndio->hdl, &retpar) == 0)
		return -EINVAL;

	if (par->bits != retpar.bits ||
	    par->bps != retpar.bps ||
	    par->rate != retpar.rate ||
	    (par->bps > 1 && par->le != retpar.le) ||
	    (par->bits < par->bps * 8 && par->msb != retpar.msb))
		return -1;

	return 0;
}

static void
sndio_free(snd_pcm_sndio_t *sndio)
{
	if (sndio->hdl)
		sio_close(sndio->hdl);
	free(sndio);
}

static void
cb(void *arg, int delta)
{
	snd_pcm_sndio_t *sndio = arg;
	sndio->realptr += delta;
}

static snd_pcm_ioplug_callback_t sndio_pcm_callback = {
	.start = sndio_start,
	.stop = sndio_stop,
	.drain = sndio_drain,
	.transfer = sndio_write,
	.pointer = sndio_pointer,
	.close = sndio_close,
	.prepare = sndio_prepare,
	.hw_params = sndio_hw_params,
	.delay = sndio_delay,
};

static int
sndio_open(snd_pcm_t **pcmp, const char *name, const char *device,
    snd_pcm_stream_t stream, int mode, long volume)
{
	snd_pcm_sndio_t *pcm_sndio;
	int err;

	pcm_sndio = calloc(1, sizeof *pcm_sndio);
	if(pcm_sndio == NULL)
		return -ENOMEM;

	pcm_sndio->hdl = sio_open(device ? device : SIO_DEVANY,
	    stream == SND_PCM_STREAM_PLAYBACK ? SIO_PLAY : SIO_REC, 0);
	if (pcm_sndio->hdl == NULL) {
		free(pcm_sndio);
		return -ENOENT;
	}

	sio_onmove(pcm_sndio->hdl, cb, pcm_sndio);

	if (volume >= 0 && volume <= SIO_MAXVOL) {
		if (sio_setvol(pcm_sndio->hdl, (unsigned int)volume) == 0)
			SNDERR("sndio: couldn't set intial volume");
	}

	sio_initpar(&pcm_sndio->par);

	pcm_sndio->io.version = SND_PCM_IOPLUG_VERSION;
	pcm_sndio->io.name = "ALSA <-> SNDIO PCM I/O Plugin";
	pcm_sndio->io.callback = &sndio_pcm_callback;
	pcm_sndio->io.private_data = pcm_sndio;
	pcm_sndio->io.mmap_rw = 0;

	struct pollfd pfd;
	sio_pollfd(pcm_sndio->hdl, &pfd, stream == SND_PCM_STREAM_PLAYBACK ? POLLOUT : POLLIN);
	pcm_sndio->io.poll_fd = pfd.fd;
	pcm_sndio->io.poll_events = pfd.events;

	pcm_sndio->ptr = 0;
	pcm_sndio->started = 0;

	err = snd_pcm_ioplug_create(&pcm_sndio->io, name, stream, mode);
	if(err < 0) {
		sndio_free(pcm_sndio);
		return err;
	}

	err = sndio_hw_constraint(pcm_sndio);
	if (err < 0) {
		snd_pcm_ioplug_delete(&pcm_sndio->io);
		sndio_free(pcm_sndio);
	}

	*pcmp = pcm_sndio->io.pcm;

	return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(sndio)
{
	snd_config_iterator_t i, next;
	const char *device = NULL;
	int err;
	long volume = -1;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;

		if (snd_config_get_id(n, &id) < 0)
			continue;

		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
			continue;

		if (strcmp(id, "device") == 0) {
			snd_config_get_string(n, &device);
			continue;
		}

		if (strcmp(id, "volume") == 0) {
			snd_config_get_integer(n, &volume);
			continue;
		}

		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	err = sndio_open(pcmp, name, device, stream, mode, volume);
	return err;
}

SND_PCM_PLUGIN_SYMBOL(sndio);
