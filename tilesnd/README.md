# tilesnd

A small sound engine for console-style games. Sixteen voices; each is a PCM sample player or a PSG-style generator
(pulse with duty cycle, triangle, long and short noise, 32-entry wavetable), with an ADSR envelope, volume and pan.
Plain C, no dependencies, no floating point: it runs in desktop tests and on a microcontroller.

The game thread queues commands (`snd_play_pcm`, `snd_play_tone`, `snd_set_freq`, `snd_key_off`, ...) through a
lock-free single-producer queue; the audio thread calls `snd_render()`, which applies them and mixes. The game thread can also ask what the audio thread
has done (`snd_room`, `snd_mark`/`snd_reached`, `snd_sounding`): whether a voice is still sounding, and when a stopped voice no longer reads a
sample, so its memory can be given back. See `snd.h`.

Music: `snd_mml_compile` (`snd_mml.c`) turns MML text, one string a track, into a song (instruments and an event list for each track; repeats stay loops),
and the audio thread plays it by itself with `snd_music_play`, one voice a track, to within a 64-frame block. `snd.h` lists the MML.

`make check` runs the tests (259,872 checks): frequencies and duty cycles, a separate model of the noise shift
register, exact PCM playback, interpolation and loops, envelope timing, pan and volume maths, exact mixing, output
independent of how it is cut into calls, the queue, MML (events, lengths, ties, loops, mistakes) and the sequencer's timing, and a
two-thread test under ThreadSanitizer. FM synthesis is
planned as another voice type. MIT licensed.
