# alsa-sndio

This alsa plugin provides a pcm that connects to a sndiod server
as a fallback for applications that don't support sndio.

At the moment only playback is supported, capturing might be added
later.

Each time the pcm is used a new sndio slot is created and sndio can
control the volume per application.

The downside of this fallback instead of using sndio directly is that
the application can't have control over the sndio per slot volume
because alsa has no "per application" mixers.

### Configuration

The simplest `.asoundrc` just sets the default pcm to this plugin.

```
pcm.!default {
	type sndio
}
```

The `volume` configuration option can be used to set an initial volume.

The `device` is used to overwrite the default device and/or the device
set by the `AUDIODEVICE` environment variable.
