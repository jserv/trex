# trex

`trex` is a terminal-based recreation of Google Chrome's famous T-Rex game, enhanced with modern optimizations and additional features.

## Features
- Classic T-Rex Gameplay - Jump and duck to avoid obstacles
- Enhanced Mechanics - Power-ups, fire abilities, and invincibility
- Scoring System - Track your high scores and level progression
- Rich Graphics - ASCII art sprites with full color support
- Zero Dependencies - No external libraries required

## Quick Start

### Building
```shell
make                # Build the game
make clean          # Clean build artifacts
```

### Running
```shell
./trex                          # Play the game (optimized rendering)
TUI_DISABLE_WRITEV=1 ./trex     # Compatibility mode for older systems
```

### Controls
- Space or Up Arrow: Jump over obstacles
- Down Arrow: Duck under pterodactyls
- ESC: Return to menu / Quit game

## Performance Optimizations
This implementation includes several advanced optimizations for terminal rendering:

### Vectored I/O (Default)
- 70-90% fewer system calls compared to traditional terminal applications
- Batches cursor movements, colors, and text into single writev() operations
- Especially beneficial for SSH connections and remote terminals
- Automatic fallback to compatibility mode if unsupported

### Additional Optimizations
- Hierarchical dirty region tracking - Only updates changed screen areas
- Escape sequence caching - Pre-computed terminal control sequences
- RLE compression - Optimized rendering of repeated characters
- Attribute state caching - Eliminates redundant color/style changes

## License
`trex` is available under a permissive MIT-style license.
Use of this source code is governed by a MIT license that can be found in the [LICENSE](LICENSE) file. 
