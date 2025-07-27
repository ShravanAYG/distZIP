char *configParse(const char *key_to_find, char *exc);
char *getIPAddr(const char *interface);
int connectToClient(const char *clientIP, int port);
int listenOnIP(const char *ip, int port);

