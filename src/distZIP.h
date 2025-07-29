
typedef struct {
        uuid_t uuid;
        char filename[1024];
        size_t size;
        int status; // 1: ready to process, 2: processing, 3: processed //
        char ip[255], buf[512];
} table;

char *configParse(const char *key_to_find, char *exc);
char *getIPAddr(const char *interface);
int connectToClient(const char *clientIP, int port);
int listenOnIP(const char *ip, int port);
void print_table(table t);
int compress_file(const char *compressor, const char *file, table *t);
int comperess_and_send();
void create_table(const char *Ruuid, const char *filename, size_t rSize, const char *serverIP);
table *set_table(const table *t);
table *get_table();
int tq_is_available();

