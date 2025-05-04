# TextEditor

This is a minimalist, lightweight text editor designed for efficient text editing directly in the terminal. It supports creating, modifying, copy/cut/paste, search, and seamless file operations, it is perfect for developers, sysadmins, and anyone working in a command-line environment.

## Installation

```bash
git clone https://github.com/tyler232/TextEditor.git
```

```
gcc -o editor editor.c
```

## Usage

Open Editor:

```bash
./editor <filename>
```

move cursor: up/down/left/right arrow

Editing(Normal) Mode: Esc

Visual Mode: Ctrl + v

Save file: Ctrl + s

Quit Editor: Ctrl + q

Search Mode: Ctrl + f

Copy-Paste:
- Enter visual mode (Ctrl + v)
- move cursor to select text
- click y to copy to clipboard
- go to desired location to paste
- click p to paste

Cut-Paste:
- Enter visual mode (Ctrl + v)
- move cursor to select text
- click c to copy to clipboard
- go to desired location to paste
- click p to paste

Delete Chunk:
- Enter visual mode (Ctrl + v)
- move cursor to select text
- click d to delete selected text

Search:
- Enter search mode (Ctrl + f)
- Enter query
- click "return" key to show occurrence
- n for next occurrence, p for previous occurrence

## Contributing

Feel free to fork the project, submit issues, or contribute features like undo, visual block mode, or additional keybindings. Open a pull request with your changes.

