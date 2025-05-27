#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#define _BSD_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"

enum editor_key {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

/*** data ***/

struct erow {
    int size;
    char *chars;
};

struct editor_config {
    int cx, cy;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    struct erow *row;
    struct termios orig_termios;
};

struct editor_config E;

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(EXIT_FAILURE);
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios))
        die("tcsetattr");
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios))
        die("tcgetattr");

    atexit(disable_raw_mode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw))
        die("tcsetattr");
}

int editor_read_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
        if (nread == -1 && errno != EAGAIN)
            die("read");

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, seq, 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, seq + 1, 1) != 1)
            return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, seq + 2, 1) != 1)
                    return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

int get_cursor_position(int *rows, int *cols) {
    (void)rows, (void)cols;
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    //  TODO  force the code to enter the `if` to test the logic, remove in
    //  production
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

void editor_append_row(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(struct erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

/*** file i/o ***/

void editor_open(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("ropen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    linelen = getline(&line, &linecap, fp);
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 &&
               (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editor_append_row(line, linelen);
    }
    free(line);
    fclose(fp);
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {.b = NULL, .len = 0}

void ab_append(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL)
        return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void ab_free(struct abuf *ab) { free(ab->b); }

/*** output ***/

void editor_scroll() {
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows)
        E.rowoff = E.cy - E.screenrows + 1;
    if (E.cx < E.coloff)
        E.coloff = E.cx;
    if (E.cx >= E.coloff + E.screencols)
        E.coloff = E.cx - E.screencols + 1;
}

void editor_draw_rows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int len = snprintf(welcome, sizeof(welcome),
                                   "Kilo editor -- version %s", KILO_VERSION);
                if (len > E.screencols)
                    len = E.screencols;
                int padding = (E.screencols - len) / 2;
                // TODO: bitwise padding table
                if (padding != 0) {
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    ab_append(ab, " ", 1);
                ab_append(ab, welcome, len);
            } else {
                ab_append(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].size - E.coloff;
            if (len < 0)
                len = 0;
            if (len > E.screencols)
                len = E.screencols;
            ab_append(ab, &E.row[filerow].chars[E.coloff], len);
        }

        ab_append(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1)
            ab_append(ab, "\r\n", 2);
    }
}

void editor_refresh_screen() {
    editor_scroll();
    struct abuf ab = ABUF_INIT;
    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1,
             E.cx - E.coloff + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

/*** input ***/

void editor_move_cursor(int key) {
    struct erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
    case ARROW_RIGHT:
        if (row && E.cx < row->size)
            E.cx++;
        else if (row && E.cx == row->size) {
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_LEFT:
        if (E.cx > 0)
            E.cx--;
        else if (E.cy > 0) {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_UP:
        if (E.cy > 0)
            E.cy--;
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows)
            E.cy++;
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editor_process_keypress() {
    int c = editor_read_key();
    switch (c) {
    case CTRL_KEY('q'):
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);

        exit(0);
        break;
    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        E.cx = E.screencols - 1;
        break;

    case ARROW_LEFT:
    case ARROW_RIGHT:
    case ARROW_UP:
    case ARROW_DOWN:
        editor_move_cursor(c);
        break;

    case PAGE_UP:
    case PAGE_DOWN: {
        int times = E.screenrows;
        while (times--)
            editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    } break;
    }
}

/*** init ***/

void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.row = NULL;
    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        die("get_window_size");
}

int main(int argc, char *argv[]) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    while (true) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
