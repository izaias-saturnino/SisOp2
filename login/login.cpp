#include "./login.hpp"

using namespace std;

mutex mtx_list; //evitar problemas com a lista de usuarios
mutex mtx_sessoes; // evitar problemas com sessoes ativas

LoginManager::LoginManager(){

}

void LoginManager::printListaUsuario(){
    vector<USUARIO>::iterator it;

    for(it = this->listaDeUsuarios.begin(); it != this->listaDeUsuarios.end(); it++){
        cout<< it->nome<<endl;
	}
}

void LoginManager::criarNovoUsuario(char nome[],int socketCli){
    USUARIO conta;

    strcpy(conta.nome,nome);
    conta.sessaoAtiva1 = true;
    conta.sessaoAtiva2 = false;
    conta.socketClient1 = socketCli;
    conta.socketClient2 = -1;  // valor invalido
    conta.sync1 = -1;  // valor invalido
    conta.sync2 = -1;  // valor invalido

    mtx_list.lock();
    this->listaDeUsuarios.push_back(conta);
    //this->printListaUsuario();
    mtx_list.unlock();
}

void LoginManager::Logout(char user[],int socket, char resposta[]){
    vector<USUARIO>::iterator it;
    
    mtx_sessoes.lock(); 
    for(it = this->listaDeUsuarios.begin(); it != this->listaDeUsuarios.end(); it++){
        if(strcmp(user,(*it).nome) == 0){
            if((*it).socketClient1 == socket)
            {
                (*it).sessaoAtiva1 = false;
                close((*it).socketClient1);
                close((*it).sync1);
                (*it).socketClient1 = -1;
                (*it).sync1 = -1;
                strcpy(resposta,"Sessao 1 desconectada");
            }
            if((*it).socketClient2 == socket){
                (*it).sessaoAtiva2 = false;
                close((*it).socketClient2);
                close((*it).sync2);
                (*it).socketClient2 = -1;
                (*it).sync2 = -1;
                strcpy(resposta,"Sessao 2 desconectada");
            }
            break;
        }
    }
    mtx_sessoes.unlock();

    if((*it).sessaoAtiva1 == false && ((*it).sessaoAtiva2 == false)) //sem conexão -> remove da lista
    {
        strcpy(resposta,"Usuario desconectado");
    }
    
}

bool LoginManager::login(int socketCli, char nome[], sockaddr_in socketAdr){
    vector<USUARIO>::iterator it;
    bool achou = false, usuarioValido = true;

    mtx_sessoes.lock();
    for(it = this->listaDeUsuarios.begin(); it != this->listaDeUsuarios.end(); it++){
        if(strcmp(nome,(*it).nome) == 0 ){
            achou = true;
            if((*it).sessaoAtiva1 == false)
            {
                (*it).sessaoAtiva1 = true;
                (*it).socketClient1 = socketCli;
                (*it).socketAddress1 = socketAdr;
                cout << "Conta da pos 1 ativada. socket: " << socketCli << endl;
            }
            else if((*it).sessaoAtiva2 == false){
                (*it).sessaoAtiva2 = true;
                (*it).socketClient2 = socketCli;
                (*it).socketAddress2 = socketAdr;
                cout << "Conta da pos 2 ativada. socket: " << socketCli << endl;
            }
            else{
                cout << "Excedido o número de sessoes possiveis\n" << endl;
                usuarioValido = false;
            }
            break;
        }
	}
    mtx_sessoes.unlock();

    if(achou != true){
        cout<<"Usuário não encontrado!"<<endl<< "Criando um novo usuario...\n"<<endl;
        this->criarNovoUsuario(nome,socketCli);
    }

    return usuarioValido;
}

void LoginManager::activate_sync_dir(char user[], int socketCli){
    
    vector<USUARIO>::iterator it;

    mtx_sessoes.lock();
    for(it = this->listaDeUsuarios.begin(); it != this->listaDeUsuarios.end(); it++){
        if(strcmp(user, (*it).nome) == 0){
            if((*it).sync1 == -1){
                (*it).sync1 = socketCli;
                cout << "socket: " << socketCli << " foi guardado em sync1" << endl;
            }
            else if((*it).sync2 == -1){
                (*it).sync2 = socketCli;
                cout << "socket: " << socketCli << " foi guardado em sync2" << endl;
            }
            break;
        }
    }
    mtx_sessoes.unlock();
}

vector<int> LoginManager::get_active_sync_dir(char user[]){
    vector<int> sockets;

    vector<USUARIO>::iterator it;
    for(it = this->listaDeUsuarios.begin(); it != this->listaDeUsuarios.end(); it++){
        if(strcmp(user, (*it).nome) == 0){
            if((*it).sync1 != -1){
                sockets.push_back((*it).sync1);
                cout << "sync1 tem o valor: " << (*it).sync1 << endl;
            }
            if((*it).sync2 != -1){
                sockets.push_back((*it).sync2);
                cout << "sync2 tem o valor: " << (*it).sync2 << endl;
            }
            break;
        }
    }
    return sockets;
}

int LoginManager::get_sender_sync_sock(int sock){
    int sync_sock = -1;
    vector<USUARIO>::iterator it;
    for(it = this->listaDeUsuarios.begin(); it != this->listaDeUsuarios.end(); it++){
        if((*it).socketClient1 == sock){
            sync_sock = (*it).sync1;
            cout << "sync1 não recebe o arquivo. sock: " << (*it).sync1 << endl;
        }
        else if((*it).socketClient2 == sock){
            sync_sock = (*it).sync2;
            cout << "sync2 não recebe o arquivo. sock: " << (*it).sync2 << endl;
        }
        break;
    }
    return sync_sock;
}