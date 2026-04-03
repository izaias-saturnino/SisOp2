#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <vector>
#include <iostream>
#include "../common/common.hpp"

#define MAX_SESSIONS 2

#define TAM_LISTA 200

class LoginManager{
    public:
        LoginManager();
        bool login(int socketCli, char nome[], sockaddr_in socketAdr);
        vector<USUARIO> listaDeUsuarios;
        void printListaUsuario();
        void Logout(char user[], int socket,char resposta[]);
        void activate_sync_dir(char user[], int socket);
        vector<int> get_active_sync_dir(char user[]);
        int get_sender_sync_sock(int sock);
    private:
        void criarNovoUsuario(char nome[],int socketCli);
};