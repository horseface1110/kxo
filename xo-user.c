#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <stdint.h>
#include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

typedef struct MoveNode {
    char pos[3];  // 例如 "C1"，記住一定要加\0
    struct MoveNode *prev;
    struct MoveNode *next;
} MoveNode;

typedef struct GameRecord {
    MoveNode *head;                // 這一盤棋的第一步
    MoveNode *tail;                // 這一盤棋的最後一步
    int move_count;                // 記錄總步數
    struct GameRecord *next_game;  // 可以串成list
} GameRecord;

void add_move(GameRecord *game, const char *pos)
{
    MoveNode *node = malloc(sizeof(MoveNode));
    strncpy(node->pos, pos, 3);  // 保險起見3
    node->pos[2] = '\0';
    node->next = NULL;
    node->prev = game->tail;

    if (game->tail) {
        game->tail->next = node;
    } else {
        game->head = node;  // 第一步
    }
    game->tail = node;
    game->move_count++;
}

void print_game(GameRecord *game)
{
    MoveNode *cur = game->head;
    while (cur->next) {
        printf("%s ->", cur->pos);
        cur = cur->next;
    }
    printf("%s", cur->pos);
    printf("\n");
}

GameRecord *all_games_head = NULL;  // 多盤棋的起點
GameRecord *all_games_tail = NULL;  // 多盤棋的終點

GameRecord *add_new_game()
{
    GameRecord *new_game = calloc(1, sizeof(GameRecord));  // calloc 自動清 0
    // 把新棋局加到串列尾端
    if (!all_games_head) {
        all_games_head = all_games_tail = new_game;
    } else {
        all_games_tail->next_game = new_game;
        all_games_tail = new_game;
    }
    return new_game;  // 傳回這一盤的指標，後面可以繼續 add_move
}



static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~IXON;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool read_attr, end_attr;

static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!read_attr)
                printf("Stopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf(
                "Stopping the kernel space tic-tac-toe game...\n");  // TODO：這邊印出多個下棋順序
            GameRecord *cur = all_games_head;
            int idx = 1;
            while (cur) {
                printf("Game #%d Moves: ", idx++);
                print_game(cur);
                cur = cur->next_game;
            }
            break;
        }
    }
    close(attr_fd);
}

static char draw_buffer[DRAWBUFFER_SIZE];
static char table[N_GRIDS];


/* Draw the board into draw_buffer */
static int draw_board(char *table)
{
    int i = 0, k = 0;
    draw_buffer[i++] = '\n';

    draw_buffer[i++] = '\n';

    while (i < DRAWBUFFER_SIZE) {
        for (int j = 0; j < (BOARD_SIZE << 1) - 1 && k < N_GRIDS; j++) {
            draw_buffer[i++] = j & 1 ? '|' : table[k++];
        }
        draw_buffer[i++] = '\n';

        for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
            draw_buffer[i++] = '-';
        }
        draw_buffer[i++] = '\n';
    }

    return 0;
}



int main(int argc, char *argv[])
{
    if (!status_check())
        exit(1);
    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    char display_buf;
    fd_set readset;
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    uint8_t mask = 0b00001111;
    read_attr = true;
    end_attr = false;
    memset(table, ' ', sizeof(table));  // 初始化table
    GameRecord *game = add_new_game();
    all_games_head = game;


    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(device_fd, &readset);

        // select() 會阻塞，直到其中任一個 fd 可以讀取為止
        int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
        if (result < 0) {  // 看誰可以讀取
            printf("Error with select system call\n");
            exit(1);
        }
        if (FD_ISSET(STDIN_FILENO, &readset)) {
            FD_CLR(STDIN_FILENO, &readset);
            listen_keyboard_handler();
        } else if (read_attr && FD_ISSET(device_fd, &readset)) {
            FD_CLR(device_fd, &readset);
            printf("\033[H\033[J"); /* ASCII escape code to clear the screen
                                     */
            read(device_fd, &display_buf, 1);
            if (display_buf >> 5) {
                memset(table, ' ', sizeof(table));  // 初始化table
                game->next_game = add_new_game();
                game = game->next_game;
            }

            int posi = display_buf & mask;
            table[posi] = (display_buf >> 4) ? 'O' : 'X';
            draw_board(table);
            printf("%s\n", draw_buffer);

            char buf[3];
            snprintf(buf, sizeof(buf), "%c%d\0", posi % 4 + 'A', posi / 4);
            add_move(game, buf);
            printf("%c%d  ", posi % 4 + 'A', posi / 4);
        }
    }

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}
