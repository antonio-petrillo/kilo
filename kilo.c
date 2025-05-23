#include <asm-generic/ioctls.h>
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

/*** data ***/

struct editor_config {
    int screenrows;
    int screencols;
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

char editor_read_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }
    return c;
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

/*** terminal ***/
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {.b = NULL, .len = 0}

void ab_append(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new != NULL)
        return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void ab_free(struct abuf *ab) { free(ab->b); }

/*** output ***/

void editor_draw_rows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        ab_append(ab, "~", 1);

        ab_append(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1) {
            ab_append(ab, "\r\n", 2);
        }
    }
    ab_append(ab, "~", 1);
}

void editor_refresh_screen() {
    struct abuf ab = ABUF_INIT;
    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);

    ab_append(&ab, "\x1b[H", 3);
    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

/*** input ***/

void editor_process_keypress() {
    char c = editor_read_key();
    switch (c) {
    case CTRL_KEY('q'):
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);

        exit(0);
        break;
    }
}

/*** init ***/

void init_editor() {
    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        die("get_window_size");
}

int main() {
    enable_raw_mode();
    init_editor();

    while (true) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
