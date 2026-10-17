#!/usr/bin/env python3
"""Regenerates the 16x16 block textures in content/base/textures/.

stone.png and water.png predate this script and are left alone. Output is
deterministic (fixed seeds), so re-running only changes files when the palette
or drawing code below changes. Requires Pillow.

    python3 scripts/gen_base_textures.py
"""
import os
import random
import sys

from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "content", "base", "textures")
S = 16


def jitter(rng, base, amount):
	d = rng.randint(-amount, amount)
	return tuple(max(0, min(255, c + d)) for c in base) + (255,)


def noise(seed, base, amount):
	rng = random.Random(seed)
	return Image.new("RGBA", (S, S)), rng, [[jitter(rng, base, amount) for _ in range(S)] for _ in range(S)]


def to_image(px):
	img = Image.new("RGBA", (S, S))
	for y in range(S):
		for x in range(S):
			img.putpixel((x, y), px[y][x])
	return img


def dirt():
	_, rng, px = noise(1, (121, 85, 58), 10)
	for _ in range(18):
		px[rng.randrange(S)][rng.randrange(S)] = jitter(rng, (92, 64, 43), 6)
	for _ in range(8):
		px[rng.randrange(S)][rng.randrange(S)] = jitter(rng, (150, 110, 80), 6)
	return to_image(px)


def grass():
	# One texture covers every face, so it is mostly green with a ragged dirt
	# fringe along the bottom, readable from the side and the top alike.
	_, rng, px = noise(2, (96, 150, 58), 12)
	for x in range(S):
		depth = rng.randint(2, 4)
		for y in range(S - depth, S):
			px[y][x] = jitter(rng, (121, 85, 58), 10)
	for _ in range(10):
		px[rng.randrange(S - 4)][rng.randrange(S)] = jitter(rng, (66, 118, 40), 6)
	return to_image(px)


def sand():
	_, rng, px = noise(3, (219, 205, 148), 8)
	for _ in range(14):
		px[rng.randrange(S)][rng.randrange(S)] = jitter(rng, (196, 180, 124), 6)
	return to_image(px)


def wood():
	# Bark: vertical streaks.
	rng = random.Random(4)
	cols = [jitter(rng, (104, 78, 46), 14) for _ in range(S)]
	px = [[tuple(max(0, min(255, c + rng.randint(-6, 6))) for c in cols[x][:3]) + (255,) for x in range(S)] for _ in range(S)]
	for x in (0, 5, 11):
		for y in range(S):
			px[y][x] = jitter(rng, (70, 50, 28), 5)
	return to_image(px)


def planks():
	rng = random.Random(5)
	px = [[(0, 0, 0, 255)] * S for _ in range(S)]
	for board in range(4):
		base = jitter(rng, (178, 140, 86), 10)[:3]
		for y in range(board * 4, board * 4 + 4):
			for x in range(S):
				px[y][x] = tuple(max(0, min(255, c + rng.randint(-5, 5))) for c in base) + (255,)
		for x in range(S):
			px[board * 4 + 3][x] = jitter(rng, (120, 88, 48), 4)
		nail = rng.randrange(S)
		px[board * 4][nail] = jitter(rng, (140, 104, 60), 4)
	return to_image(px)


def leaves():
	# Cutout foliage: ~20% fully transparent holes, the rest opaque. Kept well
	# under TextureAtlas's partial-alpha threshold so it stays a cutout block.
	_, rng, px = noise(6, (48, 112, 40), 16)
	for y in range(S):
		for x in range(S):
			if rng.random() < 0.18:
				px[y][x] = (0, 0, 0, 0)
			elif rng.random() < 0.2:
				px[y][x] = jitter(rng, (34, 86, 30), 6)
	return to_image(px)


def sticks():
	img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
	rng = random.Random(7)
	for i in range(3):
		x0 = 3 + i * 4
		for t in range(12):
			x = x0 + (t // 4) % 2
			y = 2 + t + i
			if 0 <= y < S and 0 <= x < S:
				img.putpixel((x, y), jitter(rng, (130, 94, 52), 10))
	return img


def main():
	os.makedirs(OUT, exist_ok=True)
	for name, fn in (("dirt", dirt), ("grass", grass), ("sand", sand), ("wood", wood),
			("planks", planks), ("leaves", leaves), ("sticks", sticks)):
		fn().save(os.path.join(OUT, name + ".png"))
		print("wrote", name + ".png")
	return 0


if __name__ == "__main__":
	sys.exit(main())
