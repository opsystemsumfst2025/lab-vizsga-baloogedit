#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define NUM_TRADERS 3
#define BUFFER_SIZE 10
#define INITIAL_BALANCE 10000.0

typedef struct Transaction {
    char type[10];
    char stock[10];
    int quantity;
    double price;
    struct Transaction *next;
} Transaction;

typedef struct {
    char symbol[10];
    double price;
} StockPrice;

StockPrice price_buffer[BUFFER_SIZE];
int buffer_count = 0;
int buffer_read_idx = 0;
int buffer_write_idx = 0;

double wallet_balance = INITIAL_BALANCE;
int stocks_owned = 0;

pthread_mutex_t wallet_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t transaction_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_cond = PTHREAD_COND_INITIALIZER;

Transaction *transaction_head = NULL;
volatile sig_atomic_t running = 1;
pid_t market_pid = -1;

void add_transaction(const char *type, const char *stock, int quantity, double price) {
    Transaction *new_trans = (Transaction *)malloc(sizeof(Transaction));
    if (!new_trans) return;

    strncpy(new_trans->type, type, sizeof(new_trans->type) - 1);
    new_trans->type[sizeof(new_trans->type) - 1] = '\0';
    
    strncpy(new_trans->stock, stock, sizeof(new_trans->stock) - 1);
    new_trans->stock[sizeof(new_trans->stock) - 1] = '\0';
    
    new_trans->quantity = quantity;
    new_trans->price = price;

    pthread_mutex_lock(&transaction_mutex);
    new_trans->next = transaction_head;
    transaction_head = new_trans;
    pthread_mutex_unlock(&transaction_mutex);
}

void print_transactions() {
    pthread_mutex_lock(&transaction_mutex);
    Transaction *current = transaction_head;
    printf("\n========== TRANZAKCIOS NAPLO ==========\n");
    while (current != NULL) {
        printf("[%s] %s: %d db @ %.2f $\n", current->type, current->stock, current->quantity, current->price);
        current = current->next;
    }
    printf("=======================================\n");
    pthread_mutex_unlock(&transaction_mutex);
}

void free_transactions() {
    pthread_mutex_lock(&transaction_mutex);
    Transaction *current = transaction_head;
    while (current != NULL) {
        Transaction *temp = current;
        current = current->next;
        free(temp);
    }
    transaction_head = NULL;
    pthread_mutex_unlock(&transaction_mutex);
}

void signal_handler(int sig) {
    (void)sig;
    running = 0;
    if (market_pid > 0) {
        kill(market_pid, SIGTERM);
    }
    pthread_cond_broadcast(&buffer_cond);
}

void market_process(int write_fd) {
    const char *stocks[] = {"AAPL", "GOOG", "TSLA", "MSFT", "AMZN"};
    int num_stocks = 5;
    char buffer[64];

    srand(time(NULL) ^ getpid());

    while (1) {
        int stock_idx = rand() % num_stocks;
        double price = 100.0 + (rand() % 20000) / 100.0;

        int len = snprintf(buffer, sizeof(buffer), "%s %.2f", stocks[stock_idx], price);
        if (write(write_fd, buffer, len) == -1) {
            break;
        }

        sleep(1);
    }
}

void *trader_thread(void *arg) {
    int trader_id = *(int *)arg;
    free(arg);

    while (running) {
        StockPrice current_stock;

        pthread_mutex_lock(&buffer_mutex);
        while (buffer_count == 0 && running) {
            pthread_cond_wait(&buffer_cond, &buffer_mutex);
        }

        if (!running && buffer_count == 0) {
            pthread_mutex_unlock(&buffer_mutex);
            break;
        }

        current_stock = price_buffer[buffer_read_idx];
        buffer_read_idx = (buffer_read_idx + 1) % BUFFER_SIZE;
        buffer_count--;
        pthread_mutex_unlock(&buffer_mutex);

        int action = rand() % 2; 

        pthread_mutex_lock(&wallet_mutex);
        
        if (action == 0) { 
            if (wallet_balance >= current_stock.price) {
                wallet_balance -= current_stock.price;
                stocks_owned++;
                add_transaction("VETEL", current_stock.symbol, 1, current_stock.price);
                printf("[Trader %d] VETEL: %s @ %.2f $ (Egyenleg: %.2f $)\n", trader_id, current_stock.symbol, current_stock.price, wallet_balance);
            }
        } else { 
            if (stocks_owned > 0) {
                wallet_balance += current_stock.price;
                stocks_owned--;
                add_transaction("ELADAS", current_stock.symbol, 1, current_stock.price);
                printf("[Trader %d] ELADAS: %s @ %.2f $ (Egyenleg: %.2f $)\n", trader_id, current_stock.symbol, current_stock.price, wallet_balance);
            }
        }
        
        pthread_mutex_unlock(&wallet_mutex);
        usleep(100000); 
    }
    return NULL;
}

int main() {
    int pipe_fd[2];
    pthread_t traders[NUM_TRADERS];
    
    printf("========================================\n");
    printf("  WALL STREET - PARHUZAMOS TOZSDE\n");
    printf("========================================\n");
    printf("Kezdo egyenleg: %.2f $\n", INITIAL_BALANCE);
    printf("Kereskedok szama: %d\n", NUM_TRADERS);
    printf("Ctrl+C a leallitashoz\n");
    printf("========================================\n\n");
    
    signal(SIGINT, signal_handler);
    
    if (pipe(pipe_fd) == -1) {
        perror("pipe");
        return 1;
    }
    
    market_pid = fork();
    if (market_pid < 0) {
        perror("fork");
        return 1;
    }

    if (market_pid == 0) {
        close(pipe_fd[0]);
        market_process(pipe_fd[1]);
        close(pipe_fd[1]);
        exit(0);
    }
    
    close(pipe_fd[1]);

    for (int i = 0; i < NUM_TRADERS; i++) {
        int *id = malloc(sizeof(int));
        *id = i + 1;
        if (pthread_create(&traders[i], NULL, trader_thread, id) != 0) {
            perror("pthread_create");
            free(id);
        }
    }
    
    char buffer[64];
    while (running) {
        ssize_t bytes_read = read(pipe_fd[0], buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            
            char symbol[10];
            double price;
            if (sscanf(buffer, "%s %lf", symbol, &price) == 2) {
                pthread_mutex_lock(&buffer_mutex);
                
                if (buffer_count < BUFFER_SIZE) {
                    strncpy(price_buffer[buffer_write_idx].symbol, symbol, 9);
                    price_buffer[buffer_write_idx].symbol[9] = '\0';
                    price_buffer[buffer_write_idx].price = price;
                    
                    buffer_write_idx = (buffer_write_idx + 1) % BUFFER_SIZE;
                    buffer_count++;
                    
                    pthread_cond_broadcast(&buffer_cond);
                }
                
                pthread_mutex_unlock(&buffer_mutex);
            }
        } else if (bytes_read < 0) {
             if (running) perror("read");
        }
    }
    
    for (int i = 0; i < NUM_TRADERS; i++) {
        pthread_join(traders[i], NULL);
    }
    
    waitpid(market_pid, NULL, 0);
    
    printf("\n========================================\n");
    printf("            VEGLEGES EGYENLEG\n");
    printf(" Penzegeszles: %.2f $\n", wallet_balance);
    printf(" Reszveny keszlet: %d db\n", stocks_owned);
    
    print_transactions();
    free_transactions();
    
    pthread_mutex_destroy(&wallet_mutex);
    pthread_mutex_destroy(&buffer_mutex);
    pthread_mutex_destroy(&transaction_mutex);
    pthread_cond_destroy(&buffer_cond);
    close(pipe_fd[0]);
    
    printf("\n[RENDSZER] Sikeres leallitas.\n");
    return 0;
}
