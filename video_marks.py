"""Finds the loading animation and the start of the scene in the battle part of the free play
video. Prints three times in seconds: animation start, animation end, scene start.

Usage: python video_marks.py <part.mp4>

The wait before a free play battle is black, then the game's loading animation (a small figure
bottom right on black), then black again, then the scene. The three are told apart by how
bright the picture is: nothing, a trace, a picture.
"""
import subprocess
import sys

RATE = 5
WIDTH, HEIGHT = 64, 36

raw = subprocess.run(
    ["ffmpeg", "-loglevel", "error", "-t", "60", "-i", sys.argv[1], "-vf",
     f"fps={RATE},scale={WIDTH}:{HEIGHT}", "-pix_fmt", "gray", "-f", "rawvideo", "-"],
    capture_output=True).stdout
size = WIDTH * HEIGHT
levels = [sum(raw[i:i + size]) / size for i in range(0, len(raw) - size + 1, size)]
# The figure is a few bright pixels: a mean of well under one level at this size, but not nothing.
kinds = ["scene" if v > 12 else "animation" if max(raw[i * size:(i + 1) * size]) > 40 else "black"
         for i, v in enumerate(levels)]
scene = next((i for i in range(len(kinds) - 5) if all(k == "scene" for k in kinds[i:i + 5])), None)
if scene is None:
    sys.exit("no scene found in the first minute")
animation = [i for i in range(scene) if kinds[i] == "animation"]
if not animation:
    sys.exit("no loading animation found before the scene")
print(animation[0] / RATE, (animation[-1] + 1) / RATE, scene / RATE)
