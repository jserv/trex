# Sprite Tools for T-Rex Game

This directory contains tools for bidirectional conversion between plain bitmap form and RLE compressed format.

## RLE Converter Tool

The converter supports both compression and decompression operations:

### Plain to RLE Compression
Convert ASCII art sprites to compact RLE format:

```
[ original ]
.............
......##.....
...#..##..#..
...##.##.##..
....######...
......##.....
......##.....
......##.....

[ compressed ]
19, 0, 2, 1, 8, 0, 1, 1, 2, 0, 2, 1, 2, 0, 1, 1,
5, 0, 2, 1, 1, 0, 2, 1, 1, 0, 2, 1, 6, 0, 6, 1,
9, 0, 2, 1, 11, 0, 2, 1, 11, 0, 2, 1, 5, 0
```

### RLE to Plain Decompression
Convert RLE data back to human-readable ASCII art for verification.

## Tools Available

### test-sprites.c
Verifies that RLE decompression produces correct sprite output.

Build and run:
```bash
cd tools
gcc -o test-sprites test-sprites.c
./test-sprites
```

## RLE Format Specification

Run-Length Encoding format used for efficient sprite storage:
- Structure: [count, value, count, value, ...]
- count: Number of consecutive pixels (1-255)
- value: Pixel value (0 for '.', 1 for '#')

## Current Game Sprites

All sprites have been verified and render correctly:

- Cactus: 13×8 = 104 pixels → 46 RLE bytes
- Rock: 11×3 = 33 pixels → 12 RLE bytes
- Egg: 13×6 = 78 pixels → 26 RLE bytes
- Pterodactyl: 32×12 = 384 pixels → 50 RLE bytes
- T-Rex Normal: 22×15 = 330 pixels → 98 RLE bytes
- T-Rex Duck: 30×15 = 450 pixels → 66 RLE bytes

## Usage Examples

Convert plain ASCII art to RLE:
```
Input:  ..##..
        .####.
        ......

Output: 2, 0, 2, 1, 2, 0, 1, 0, 4, 1, 1, 0, 6, 0
```

Convert RLE back to ASCII art:
```
Input:  2, 0, 2, 1, 2, 0, 1, 0, 4, 1, 1, 0, 6, 0

Output: ..##..
        .####.
        ......
```

The converter ensures data integrity through round-trip verification, making it safe for sprite development and debugging.
