#include <linux/slab.h>
#include <linux/string.h>

#include "game.h"
#include "mcts.h"
#include "util.h"

struct node {
    int move;
    char player;
    int n_visits;
    fixed_point_t score;
    struct node *parent;
    struct node *children[N_GRIDS];
};

static struct mcts_info mcts_obj;

static struct node *new_node(int move, char player, struct node *parent)
{
    struct node *node = kzalloc(sizeof(struct node), GFP_KERNEL);
    node->move = move;
    node->player = player;
    node->n_visits = 0;
    node->score = 0;
    node->parent = parent;
    memset(node->children, 0, sizeof(node->children));
    return node;
}

static void free_node(struct node *node)
{
    for (int i = 0; i < N_GRIDS; i++)
        if (node->children[i])
            free_node(node->children[i]);
    kfree(node);
}

static fixed_point_t fixed_sqrt(fixed_point_t x)
{
    if (!x || x == (1U << FIXED_SCALE_BITS))
        return x;

    fixed_point_t s = 0U;
    for (int i = (31 - __builtin_clz(x | 1)); i >= 0; i--) {
        fixed_point_t t = (1U << i);
        if ((((s + t) * (s + t)) >> FIXED_SCALE_BITS) <= x)
            s += t;
    }
    return s;
}

static fixed_point_t fixed_log(fixed_point_t v)
{
    if (!v || v == (1U << FIXED_SCALE_BITS))
        return 0;

    fixed_point_t numerator = (v - (1U << FIXED_SCALE_BITS));
    int neg = 0;
    if (GET_SIGN(numerator)) {
        neg = 1;
        numerator = CLR_SIGN(numerator);
        numerator = (1U << 31) - numerator;
    }

    fixed_point_t y =
        (numerator << FIXED_SCALE_BITS) / (v + (1U << FIXED_SCALE_BITS));

    fixed_point_t ans = 0U;
    for (unsigned i = 1; i < 20; i += 2) {
        fixed_point_t z = (1U << FIXED_SCALE_BITS);
        for (int j = 0; j < i; j++) {
            z *= y;
            z >>= FIXED_SCALE_BITS;
        }
        z <<= FIXED_SCALE_BITS;
        z /= (i << FIXED_SCALE_BITS);

        ans += z;
    }
    ans <<= 1;
    ans = neg ? SET_SIGN(ans) : ans;
    return ans;
}

#define EXPLORATION_FACTOR fixed_sqrt(1U << (FIXED_SCALE_BITS + 1))

static inline fixed_point_t uct_score(int n_total,
                                      int n_visits,
                                      fixed_point_t score)
{
    if (n_visits == 0)
        return FIXED_MAX;

    fixed_point_t result =
        score << FIXED_SCALE_BITS /
                     (fixed_point_t) (n_visits << FIXED_SCALE_BITS);
    fixed_point_t tmp =
        EXPLORATION_FACTOR *
        fixed_sqrt(fixed_log(n_total << FIXED_SCALE_BITS) / n_visits);
    tmp >>= FIXED_SCALE_BITS;
    return result + tmp;
}

static struct node *select_move(struct node *node)
{
    struct node *best_node = NULL;
    fixed_point_t best_score = 0U;
    for (int i = 0; i < N_GRIDS; i++) {
        if (!node->children[i])
            continue;
        fixed_point_t score =
            uct_score(node->n_visits, node->children[i]->n_visits,
                      node->children[i]->score);
        if (score > best_score) {
            best_score = score;
            best_node = node->children[i];
        }
    }
    return best_node;
}

static fixed_point_t simulate(const char *table, char player)
{
    char current_player = player;
    char temp_table[N_GRIDS];
    memcpy(temp_table, table, N_GRIDS);
    xoro_jump(&(mcts_obj.xoro_obj));
    while (1) {
        int *moves = available_moves(temp_table);
        if (moves[0] == -1) {
            kfree(moves);
            break;
        }
        int n_moves = 0;
        while (n_moves < N_GRIDS && moves[n_moves] != -1)
            ++n_moves;
        int move =
            moves[xoro_next(&(mcts_obj.xoro_obj)) % n_moves];  // 隨機下棋
        kfree(moves);
        temp_table[move] = current_player;
        char win;
        if ((win = check_win(temp_table)) != ' ')
            return calculate_win_value(win, player);
        current_player ^= 'O' ^ 'X';
    }
    return (fixed_point_t) (1UL << (FIXED_SCALE_BITS - 1));
}

/*目的是把模擬（simulation/rollout）結果往樹的上層節點逐層累加，
 *讓每一層都能獲得該分支下的勝率資訊。這是 MCTS 的標準步驟之一。
 */

static void backpropagate(struct node *node, fixed_point_t score)
{
    while (node) {
        node->n_visits++;
        node->score += score;
        node = node->parent;
        score = 1 - score;  // 要給父節點(對手)的分數
    }
}

static int expand(
    struct node *node,
    const char *table)  // 產生所有可能的下一步，回傳還有幾個位置可以落子
{
    int *moves = available_moves(table);
    int n_moves = 0;
    while (n_moves < N_GRIDS && moves[n_moves] != -1)
        ++n_moves;
    for (int i = 0; i < n_moves; i++) {
        node->children[i] = new_node(moves[i], node->player ^ 'O' ^ 'X', node);
    }
    kfree(moves);
    return n_moves;
}

int mcts(const char *table, char player)
{
    char win;
    struct node *root = new_node(-1, player, NULL);
    mcts_obj.nr_active_nodes = 1;
    for (int i = 0; i < ITERATIONS; i++) {
        struct node *node = root;
        char temp_table[N_GRIDS];
        memcpy(temp_table, table, N_GRIDS);
        while (1) {
            if ((win = check_win(temp_table)) != ' ') {
                fixed_point_t score =
                    calculate_win_value(win, node->player ^ 'O' ^ 'X');
                backpropagate(node, score);
                break;
            }
            if (node->n_visits == 0) {
                fixed_point_t score = simulate(temp_table, node->player);
                backpropagate(node, score);
                break;
            }
            if (node->children[0] == NULL)
                mcts_obj.nr_active_nodes += expand(node, temp_table);
            node = select_move(node);
            if (!node)
                return -1;
            temp_table[node->move] = node->player ^ 'O' ^ 'X';
        }
    }
    struct node *best_node = root;
    int most_visits = -1;
    for (int i = 0; i < N_GRIDS; i++) {
        if (root->children[i] && root->children[i]->n_visits > most_visits) {
            most_visits = root->children[i]->n_visits;
            best_node = root->children[i];
        }
    }
    int best_move = best_node->move;
    free_node(root);
    return best_move;
}

void mcts_init(void)
{
    xoro_init(&(mcts_obj.xoro_obj));
    mcts_obj.nr_active_nodes = 0;
}
