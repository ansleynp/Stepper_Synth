"""Play up to three local preview tones together with one mixer channel per voice."""
from array import array
from functools import lru_cache
import math

RATE = 44100


@lru_cache(maxsize=40)
def waveform(note):
    frequency = 440 * 2 ** ((note - 69) / 12)
    cycles = max(1, round(frequency / 10))
    length = round(RATE * cycles / frequency)
    return array("h", (round(5600 * math.sin(2 * math.pi * cycles * i / length))
                       for i in range(length))).tobytes()


class PreviewAudio:
    def __init__(self):
        self.mixer = None
        self.channels = []
        self.sounds = {}

    def play(self, notes):
        self.stop()
        if not notes:
            return
        if self.mixer is None:
            import os
            os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
            from pygame import mixer
            mixer.init(frequency=RATE, size=-16, channels=1, buffer=256, allowedchanges=0)
            self.mixer = mixer
            self.channels = [mixer.Channel(i) for i in range(3)]
        for channel, note in zip(self.channels, notes):
            if note not in self.sounds:
                self.sounds[note] = self.mixer.Sound(buffer=waveform(note))
            channel.play(self.sounds[note], loops=-1, fade_ms=8)

    def stop(self):
        for channel in self.channels:
            channel.stop()

    def close(self):
        if self.mixer:
            self.mixer.quit()
