# libVoice

A small audio mixer and synth, generalizing recorded and synthesized audio.
See [libVoiceDemo](https://github.com/DinosaurBlanket/libVoiceDemo) for a demonstration.

Depends on SDL2.<br />

Sound comes from "voices", which have a number of "oscillators",
which all reference a shared array of "shapes".
These shapes can be simple waveforms, or longer audio clips,
they can be looped or clamped, and their amplitude and speed can
be modulated and enveloped.
The interface is thread-safe, and no allocation or locking is needed in the client thread.
Take a look at voice.h for more details.
