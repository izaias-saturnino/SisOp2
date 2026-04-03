#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstdlib>
#include <signal.h>
#include <ctime>
#include <mutex>
#include <fstream>
#include <vector>
#include <sys/ioctl.h>
#include <fcntl.h>

using namespace std;

//messages
#define MENSAGEM_LOGIN 1
#define MENSAGEM_LOGOUT 2
#define MENSAGEM_USUARIO_INVALIDO 3
#define MENSAGEM_USUARIO_VALIDO 4
#define MENSAGEM_RESPOSTA_LOGOUT 5
#define MENSAGEM_IP 6
#define MENSAGEM_VERIFICACAO 7
#define MENSAGEM_ENVIO_NOME_ARQUIVO 10
#define MENSAGEM_ENVIO_PARTE_ARQUIVO 11
#define MENSAGEM_ENVIO_SYNC 12
#define MENSAGEM_ENVIO_TAMANHO_ARQUIVO 13
#define MENSAGEM_PEDIDO_LISTA_ARQUIVOS_SERVIDOR 20
#define MENSAGEM_ITEM_LISTA_DE_ARQUIVOS 21
#define MENSAGEM_DELETAR_NO_SERVIDOR 30
#define MENSAGEM_DELETAR_NOS_CLIENTES 31
#define MENSAGEM_DOWNLOAD_FROM_SERVER 40
#define MENSAGEM_FALHA_ENVIO 41
#define GET_SYNC_DIR 50
#define FIRST_SYNC_END 51
#define ACK 60
#define LIST_SERVERS 70
#define SERVER_ITEM 72
#define ID_REQUEST 73
#define LIVENESS_CHECK 74
#define ELECTION 80
#define ELECTED 81

//consts
#define BUFFER_SIZE 256
#define MAX_RETRIES 10
#define WAIT_TIME_BETWEEN_RETRIES 20 * 1000
#define DEFAULT_PORT 4000
#define MAX_TIMER 4*MAX_RETRIES

typedef struct {
    uint16_t type; //Tipo do pacote
    uint16_t seqn; //Número de sequência
    char _payload[BUFFER_SIZE]; //Dados do pacote
}PACKET;

typedef struct sockaddr_in Sockaddr_in;

struct usuario{
    char nome[BUFFER_SIZE];
    bool sessaoAtiva1;
    bool sessaoAtiva2;
    int socketClient1;
    int socketClient2;
    int sync1;
    int sync2;
    Sockaddr_in socketAddress1;
    Sockaddr_in socketAddress2;
};
typedef struct usuario USUARIO;

typedef struct server_copy{
    int id;
    int PORT;
    string ip;
}SERVER_COPY;

 typedef struct {
    int PORT ;
    int sockfd;
    string server_ip; 
	hostent* server_host ;
}ALIVE;

void serialize(PACKET *pkt, char data[sizeof(PACKET)]);
void deserialize(PACKET *pkt, char data[sizeof(PACKET)]);
int readSocket(PACKET *pkt, int sock);
void peekSocket(PACKET *pkt, int sock);
void sendMessage(char message[BUFFER_SIZE], int messageType, int sockfd);
//returns the amount of bytes written
void receiveFile(int sock, string file_path, PACKET *pkt_addr);
void sendFile(int sock, string file_path);
string getFileName(string file_path);
//returns server socket
bool has_received_message(int sock);
int connect_to_server(hostent server_host);
int connect_to_server(string server_ip, int PORT);
int connect_to_server(hostent server_host, int PORT);
void create_thread(
    pthread_t *__restrict thr1,
	const pthread_attr_t *__restrict attr,
	void *(*handle_updates) (void *),
	void *__restrict sockfd_sync);
SERVER_COPY receive_server_copy(int socket);
void send_server_copy(int socket, SERVER_COPY server_copy, int msg_type);
void reconnectToClients(vector<USUARIO> listaDeUsuarios);