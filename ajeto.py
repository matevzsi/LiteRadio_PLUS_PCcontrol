import winsound
import time

melody = [
    (415, 210),  # G#4
    (466, 155),  # A#4
    (523, 255),  # C5
    (466, 135),  # A#4
    (415, 195),  # G#4
    (523, 155),  # C5
    (622, 155),  # D#5
    (784, 395),  # G5
]

GAP_MS = 25

for i, (freq, duration) in enumerate(melody):
    winsound.Beep(freq, duration)

    if i != len(melody) - 1:
        time.sleep(GAP_MS / 1000.0)