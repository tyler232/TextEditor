#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>

/*** Defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_LINES 100

int editor_rows = 24;
int editor_cols = 80;

/*** Terminal ***/

struct termios orig_termios;

void die(const char *s);
void disableRawMode();
void enableRawMode();

/*** Editor State ***/

int cx;
int cy;
char *lines[MAX_LINES];
int num_lines;
const char *current_filename;
char statusmsg[80];
int visual_mode;
int sx, sy; // selection start coordinates
char *clipboard; // buffer for copied text
int clip_len; // length of clipboard content
int rowoff; // row offset for scrolling

/*** Input ***/

enum EditorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN
};

int readKey();
void moveCursor(int key);
void insertChar(int c);
void deleteChar();
void insertNewline();
void processKeypress();
void toggleVisualMode();
void copySelection();
void cutSelection();
void deleteSelection();
void pasteClipboard();

/*** Output ***/

void editorDrawRows();
void drawStatusBar();
void editorRefreshScreen();

/*** File I/O ***/

void saveFile(const char *filename);
void loadFile(const char *filename);

/*** Terminal Setup ***/

void die(const char *s) {
    perror(s);
    exit(1);
}

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/*** Input Functions ***/

int readKey() {
    char c;
    struct timeval tv = {0, 1000}; // 1ms timeout
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    // Wait for input with timeout
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) {
        return 0; // no input ready
    }

    if (read(STDIN_FILENO, &c, 1) != 1) {
        return 0; // read failed
    }

    if (c == '\x1b') {
        char seq[2];
        // Check for next character
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) {
                return '\x1b';
            }
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
            }
        }
        return '\x1b'; // consume unrecognized sequences
    }
    return c;
}

void moveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (cx > 0) cx--;
            break;
        case ARROW_RIGHT:
            if (cx < editor_cols - 1) cx++;
            break;
        case ARROW_UP:
            if (cy > 0) cy--;
            break;
        case ARROW_DOWN:
            if (cy < num_lines - 1) cy++;
            break;
    }

    // adjust scroll offset
    if (cy < rowoff) {
        rowoff = cy;
    }
    if (cy >= rowoff + editor_rows) {
        rowoff = cy - editor_rows + 1;
    }

    // ensure cursor x doesn't exceed line length
    int len = lines[cy] ? strlen(lines[cy]) : 0;
    if (cx > len) cx = len;
}

void insertChar(int c) {
    if (cy >= MAX_LINES) return;

    if (lines[cy] == NULL) {
        lines[cy] = malloc(1);
        lines[cy][0] = '\0';
    }

    int len = strlen(lines[cy]);
    if (cx > len) cx = len;

    lines[cy] = realloc(lines[cy], len + 2);
    memmove(&lines[cy][cx + 1], &lines[cy][cx], len - cx + 1);
    lines[cy][cx] = c;
    cx++;
    if (cy >= num_lines) num_lines = cy + 1;
}

void deleteChar() {
    if (cy >= num_lines || !lines[cy]) return;

    if (cx > 0) {
        int len = strlen(lines[cy]);
        memmove(&lines[cy][cx - 1], &lines[cy][cx], len - cx + 1);
        lines[cy] = realloc(lines[cy], len);
        cx--;
    } else if (cy > 0) {
        int prev_len = strlen(lines[cy - 1]);
        int curr_len = strlen(lines[cy]);

        lines[cy - 1] = realloc(lines[cy - 1], prev_len + curr_len + 1);
        strcat(lines[cy - 1], lines[cy]);

        free(lines[cy]);
        for (int i = cy; i < num_lines - 1; i++) {
            lines[i] = lines[i + 1];
        }
        lines[num_lines - 1] = NULL;
        num_lines--;
        cy--;
        cx = prev_len;
    }
}

void insertNewline() {
    if (cy >= MAX_LINES - 1) return;

    char *line = lines[cy];
    if (!line) {
        cy++;
        cx = 0;
        return;
    }

    int len = strlen(line);
    char *left = malloc(cx + 1);
    char *right = malloc(len - cx + 1);

    memcpy(left, line, cx);
    left[cx] = '\0';
    strcpy(right, &line[cx]);

    lines[cy] = left;

    for (int i = num_lines; i > cy + 1; i--) {
        lines[i] = lines[i - 1];
    }

    lines[cy + 1] = right;
    num_lines++;
    cy++;
    cx = 0;
}

void toggleVisualMode() {
    if (visual_mode) {
        visual_mode = 0;
        snprintf(statusmsg, sizeof(statusmsg), "[Normal Mode]");
    } else {
        visual_mode = 1;
        sx = cx;
        sy = cy;
        snprintf(statusmsg, sizeof(statusmsg), "[Visual Mode]");
    }
}

void copySelection() {
    if (!visual_mode) return;

    // free previous clipboard content
    if (clipboard) free(clipboard);

    // boundarys of selection
    int start_y = sy < cy ? sy : cy;
    int end_y = sy < cy ? cy : sy;
    int start_x = (sy < cy || (sy == cy && sx <= cx)) ? sx : cx;
    int end_x = (sy < cy || (sy == cy && sx <= cx)) ? cx : sx;

    // handle same-line selection
    if (start_y == end_y && start_x == end_x) {
        clipboard = malloc(1);
        clipboard[0] = '\0';
        clip_len = 0;
        visual_mode = 0;
        snprintf(statusmsg, sizeof(statusmsg), "[Copied 0 chars]");
        return;
    }

    // calculate total length needed
    clip_len = 0;
    for (int y = start_y; y <= end_y; y++) {
        if (lines[y]) {
            int len = strlen(lines[y]);
            int x_start = (y == start_y) ? start_x : 0;
            int x_end = (y == end_y) ? end_x : len;
            clip_len += x_end - x_start;
            if (y < end_y) clip_len++; // For newline
        }
    }

    // allocate and copy to clipboard
    clipboard = malloc(clip_len + 1);
    int pos = 0;
    for (int y = start_y; y <= end_y; y++) {
        if (lines[y]) {
            int len = strlen(lines[y]);
            int x_start = (y == start_y) ? start_x : 0;
            int x_end = (y == end_y) ? end_x : len;
            for (int x = x_start; x < x_end; x++) {
                clipboard[pos++] = lines[y][x];
            }
            if (y < end_y && pos < clip_len) clipboard[pos++] = '\n';
        }
    }
    clipboard[pos] = '\0';
    clip_len = pos;

    // exit visual mode
    visual_mode = 0;
    snprintf(statusmsg, sizeof(statusmsg), "[Copied %d chars]", clip_len);
}

void cutSelection() {
    if (!visual_mode) return;

    // copy selection to clipboard first
    copySelection();

    // determine selection boundaries
    int start_y = sy < cy ? sy : cy;
    int end_y = sy < cy ? cy : sy;
    int start_x = (sy < cy || (sy == cy && sx <= cx)) ? sx : cx;
    int end_x = (sy < cy || (sy == cy && sx <= cx)) ? cx : sx;

    // handle same-line selection
    if (start_y == end_y) {
        if (start_x == end_x) {
            visual_mode = 0;
            snprintf(statusmsg, sizeof(statusmsg), "[Cut 0 chars]");
            return;
        }
        int len = strlen(lines[start_y]);
        memmove(&lines[start_y][start_x], &lines[start_y][end_x], len - end_x + 1);
        lines[start_y] = realloc(lines[start_y], len - (end_x - start_x) + 1);
        cx = start_x;
        cy = start_y;
    } else {
        // handle multi-line selection
        // first line: keep text before start_x
        int first_len = strlen(lines[start_y]);
        lines[start_y] = realloc(lines[start_y], start_x + 1);
        lines[start_y][start_x] = '\0';

        // last line: keep text after end_x
        int last_len = strlen(lines[end_y]);
        char *last_part = malloc(last_len - end_x + 1);
        strcpy(last_part, &lines[end_y][end_x]);
        free(lines[end_y]);
        lines[end_y] = last_part;

        // concatenate first and last lines
        int new_len = strlen(lines[start_y]) + strlen(lines[end_y]) + 1;
        lines[start_y] = realloc(lines[start_y], new_len);
        strcat(lines[start_y], lines[end_y]);
        free(lines[end_y]);

        // shift lines up
        for (int i = end_y; i < num_lines - 1; i++) {
            lines[i] = lines[i + 1];
        }
        lines[num_lines - 1] = NULL;

        // remove intermediate lines
        for (int i = start_y + 1; i < end_y; i++) {
            free(lines[i]);
            lines[i] = NULL;
        }
        for (int i = start_y + 1; i < num_lines - (end_y - start_y); i++) {
            lines[i] = lines[i + (end_y - start_y)];
        }
        for (int i = num_lines - (end_y - start_y); i < num_lines; i++) {
            lines[i] = NULL;
        }
        num_lines -= (end_y - start_y);

        cx = start_x;
        cy = start_y;
    }

    // exit visual mode
    visual_mode = 0;
    snprintf(statusmsg, sizeof(statusmsg), "[Cut %d chars]", clip_len);

    // adjust scroll offset
    if (cy < rowoff) {
        rowoff = cy;
    }
    if (cy >= rowoff + editor_rows) {
        rowoff = cy - editor_rows + 1;
    }
}

void deleteSelection() {
    if (!visual_mode) return;

    // determine selection boundaries
    int start_y = sy < cy ? sy : cy;
    int end_y = sy < cy ? cy : sy;
    int start_x = (sy < cy || (sy == cy && sx <= cx)) ? sx : cx;
    int end_x = (sy < cy || (sy == cy && sx <= cx)) ? cx : sx;

    // calculate deleted chars for status message
    int deleted_chars = 0;
    for (int y = start_y; y <= end_y; y++) {
        if (lines[y]) {
            int len = strlen(lines[y]);
            int x_start = (y == start_y) ? start_x : 0;
            int x_end = (y == end_y) ? end_x : len;
            deleted_chars += x_end - x_start;
            if (y < end_y) deleted_chars++; // For newline
        }
    }

    // handle same-line selection
    if (start_y == end_y) {
        if (start_x == end_x) {
            visual_mode = 0;
            snprintf(statusmsg, sizeof(statusmsg), "[Deleted 0 chars]");
            return;
        }
        int len = strlen(lines[start_y]);
        memmove(&lines[start_y][start_x], &lines[start_y][end_x], len - end_x + 1);
        lines[start_y] = realloc(lines[start_y], len - (end_x - start_x) + 1);
        cx = start_x;
        cy = start_y;
    } else {
        // handle multi-line selection
        // first line: keep text before start_x
        int first_len = strlen(lines[start_y]);
        lines[start_y] = realloc(lines[start_y], start_x + 1);
        lines[start_y][start_x] = '\0';

        // last line: keep text after end_x
        int last_len = strlen(lines[end_y]);
        char *last_part = malloc(last_len - end_x + 1);
        strcpy(last_part, &lines[end_y][end_x]);
        free(lines[end_y]);
        lines[end_y] = last_part;

        // concatenate first and last lines
        int new_len = strlen(lines[start_y]) + strlen(lines[end_y]) + 1;
        lines[start_y] = realloc(lines[start_y], new_len);
        strcat(lines[start_y], lines[end_y]);
        free(lines[end_y]);

        // shift lines up
        for (int i = end_y; i < num_lines - 1; i++) {
            lines[i] = lines[i + 1];
        }
        lines[num_lines - 1] = NULL;

        // remove intermediate lines
        for (int i = start_y + 1; i < end_y; i++) {
            free(lines[i]);
            lines[i] = NULL;
        }
        for (int i = start_y + 1; i < num_lines - (end_y - start_y); i++) {
            lines[i] = lines[i + (end_y - start_y)];
        }
        for (int i = num_lines - (end_y - start_y); i < num_lines; i++) {
            lines[i] = NULL;
        }
        num_lines -= (end_y - start_y);

        cx = start_x;
        cy = start_y;
    }

    // exit visual mode
    visual_mode = 0;
    snprintf(statusmsg, sizeof(statusmsg), "[Deleted %d chars]", deleted_chars);

    // adjust scroll offset
    if (cy < rowoff) {
        rowoff = cy;
    }
    if (cy >= rowoff + editor_rows) {
        rowoff = cy - editor_rows + 1;
    }
}

void pasteClipboard() {
    if (!clipboard) return;

    for (int i = 0; i < clip_len; i++) {
        if (clipboard[i] == '\n') {
            insertNewline();
        } else {
            insertChar(clipboard[i]);
        }
    }
    snprintf(statusmsg, sizeof(statusmsg), "[Pasted %d chars]", clip_len);
}

void saveFile(const char *filename) {
    FILE *file = fopen(filename, "w");
    if (!file) {
        snprintf(statusmsg, sizeof(statusmsg), "Can't save! fopen error.");
        return;
    }

    for (int i = 0; i < num_lines; i++) {
        if (lines[i]) {
            fprintf(file, "%s\n", lines[i]);
        }
    }

    fclose(file);
    snprintf(statusmsg, sizeof(statusmsg), "[Saved to %s]", filename);
}

void loadFile(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        file = fopen(filename, "w");
        if (!file) {
            die("fopen");
        }
        fclose(file);
        return;
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t nread;

    num_lines = 0;
    while ((nread = getline(&line, &len, file)) != -1) {
        line[strcspn(line, "\n")] = 0;
        lines[num_lines] = malloc(strlen(line) + 1);
        strcpy(lines[num_lines], line);
        num_lines++;
    }

    free(line);
    fclose(file);
}

void processKeypress() {
    int c = readKey();

    // no input, skip
    if (c == 0) return;

    // Esc always returns to normal mode
    if (c == '\x1b') {
        visual_mode = 0;
        snprintf(statusmsg, sizeof(statusmsg), "[Normal Mode]");
        editorRefreshScreen();
        return;
    }

    if (c == CTRL_KEY('q')) {
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
    } else if ((c == CTRL_KEY('v') || c == 'v') && !visual_mode) {
        toggleVisualMode();
        editorRefreshScreen();
    } else if (c == 'y' && visual_mode) {
        copySelection();
        editorRefreshScreen();
    } else if (c == 'c' && visual_mode) {
        cutSelection();
        editorRefreshScreen();
    } else if (c == 'd' && visual_mode) {
        deleteSelection();
        editorRefreshScreen();
    } else if (c == 'p' && !visual_mode) {
        pasteClipboard();
        editorRefreshScreen();
    } else if (visual_mode) {
        moveCursor(c); // allow cursor movement in visual mode
        editorRefreshScreen();
    } else if (c == 127) { // backspace
        deleteChar();
        editorRefreshScreen();
    } else if (c == '\r') { // Enter
        insertNewline();
        editorRefreshScreen();
    } else if (c == CTRL_KEY('s')) {
        saveFile(current_filename);
        editorRefreshScreen();
    } else if (c == CTRL_KEY('o')) { // Open
        loadFile(current_filename);
        editorRefreshScreen();
    } else if (c >= 32 && c <= 126) {
        insertChar(c);
        editorRefreshScreen();
    } else {
        moveCursor(c);
        editorRefreshScreen();
    }
}

void editorDrawRows() {
    for (int y = 0; y < editor_rows; y++) {
        int file_y = y + rowoff;
        if (file_y < num_lines && lines[file_y]) {
            if (visual_mode) {
                // determine if this row is within the selection
                int start_y = sy < cy ? sy : cy;
                int end_y = sy < cy ? cy : sy;
                if (file_y >= start_y && file_y <= end_y) {
                    int start_x, end_x;
                    if (sy == cy) {
                        // same-line selection
                        start_x = sx < cx ? sx : cx;
                        end_x = sx < cx ? cx : sx;
                    } else if (file_y == start_y) {
                        // first line of multi-line selection
                        start_x = (file_y == sy) ? sx : (file_y == cy) ? cx : 0;
                        end_x = strlen(lines[file_y]);
                    } else if (file_y == end_y) {
                        // last line of multi-line selection
                        start_x = 0;
                        end_x = (file_y == sy) ? sx : (file_y == cy) ? cx : strlen(lines[file_y]);
                    } else {
                        // middle line of multi-line selection
                        start_x = 0;
                        end_x = strlen(lines[file_y]);
                    }

                    // write line with highlighting
                    int len = strlen(lines[file_y]);
                    for (int x = 0; x < len; x++) {
                        if (x >= start_x && x < end_x) {
                            write(STDOUT_FILENO, "\x1b[7m", 4); // reverse video (highlight)
                            write(STDOUT_FILENO, &lines[file_y][x], 1);
                            write(STDOUT_FILENO, "\x1b[0m", 4); // reset
                        } else {
                            write(STDOUT_FILENO, &lines[file_y][x], 1);
                        }
                    }
                } else {
                    write(STDOUT_FILENO, lines[file_y], strlen(lines[file_y]));
                }
            } else {
                write(STDOUT_FILENO, lines[file_y], strlen(lines[file_y]));
            }
        } else {
            write(STDOUT_FILENO, "~", 1);
        }
        write(STDOUT_FILENO, "\r\n", 2);
    }
}

void updateWindowSize() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // If ioctl fails or reports 0, use defaults
        editor_rows = 24;
        editor_cols = 80;
    } else {
        // Subtract 1 row for the status bar
        editor_rows = ws.ws_row - 1;
        editor_cols = ws.ws_col;
    }
}

void drawStatusBar() {
    updateWindowSize();

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        // fallback to a reasonable default if ioctl fails
        ws.ws_row = editor_rows + 1;
        ws.ws_col = editor_cols;
    }

    // move to the bottom row
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;1H", ws.ws_row);
    write(STDOUT_FILENO, buf, strlen(buf));

    // clear the line and enable reverse video
    write(STDOUT_FILENO, "\x1b[K", 3); // Clear line
    write(STDOUT_FILENO, "\x1b[7m", 4); // Reverse video

    // write status message, truncate if too long
    int msg_len = strlen(statusmsg);
    if (msg_len > ws.ws_col) msg_len = ws.ws_col;
    write(STDOUT_FILENO, statusmsg, msg_len);

    // Pad with spaces to fill the row
    for (int i = msg_len; i < ws.ws_col; i++) {
        write(STDOUT_FILENO, " ", 1);
    }

    // reset attributes
    write(STDOUT_FILENO, "\x1b[0m", 4);
}


void editorRefreshScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3); 

    // draw editor content and status bar at buttom
    editorDrawRows();
    drawStatusBar();

    // move cursor to editor position
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        ws.ws_row = editor_rows + 1;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cy - rowoff + 1, cx + 1);
    write(STDOUT_FILENO, buf, strlen(buf));

    fflush(stdout); // ensure immediate output
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    current_filename = argv[1];
    loadFile(current_filename);
    visual_mode = 0;
    clipboard = NULL;
    clip_len = 0;
    rowoff = 0;
    snprintf(statusmsg, sizeof(statusmsg), "[Normal Mode]");

    enableRawMode();
    editorRefreshScreen();
    while (1) {
        // editorRefreshScreen();
        processKeypress();
    }

    return 0;
}

