#include "./server.hpp"
#include "../login/login.hpp"

#define PORT 4000

int main(int argc, char *argv[])
{
    int sockfd,newSockfd;
    struct sockaddr_in serv_addr,cli_addr;
    struct hostent *server;
	pthread_t clientThread;
    socklen_t clilen;
    char user[256];
    bool usuarioValido;

    verificaRecebimentoIP(argc, argv);

    server = gethostbyname(argv[1]);

    if (server == NULL) {
        imprimeServerError();
    }
    
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)  ///Verifica IP valido
        printf("ERROR opening socket\n");
        
    serv_addr.sin_family = AF_INET;     
    serv_addr.sin_port = htons(PORT);    
    serv_addr.sin_addr = *((struct in_addr *)server->h_addr);
    bzero(&(serv_addr.sin_zero), 8);     
     
	if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
		printf("ERROR on binding\n");
	else
	{	
        while(true)
        {    
            /*listen to clients*/
		    listen(sockfd, 5);
            clilen = sizeof(struct sockaddr_in);
            if ((newSockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen)) == -1) 
                printf("ERROR on accept");
            //memset(&package, 0, sizeof(package));
            //thread_mtx.lock();

            /*inicio login*/

            LoginManager *loginManager = new LoginManager();
            readSocket(user,newSockfd);
            usuarioValido = loginManager->login(newSockfd,user); 

            if(usuarioValido)
                pthread_create(&clientThread, NULL, readAndWriteSocket, &newSockfd);  //CUIDADO: newSocket e não socket
            else{
                //logout
            }
        }
    }

    close(newSockfd);
    close(sockfd);

    return 0;
}

void verificaRecebimentoIP(int argc,char *argv[]){
    if (argc < 2) {  //Verifica se recebeu o IP como parametro
        fprintf(stderr,"usage %s hostname\n", argv[0]);
        exit(0);
    }
}

void imprimeServerError(void){
    fprintf(stderr,"ERROR, no such host\n");
    exit(0);
}

void writeSocket(char buffer[],int sockfd){
    int n;

	/* write in the socket */ 
	n = write(sockfd,"I got your message", 18);
	if (n < 0) 
		printf("ERROR writing to socket");
}

void readSocket(char array[], int sockfd){
    int n;

	/* read from the socket */
	n = read(sockfd, array, 256); 
	if (n < 0) 
		printf("ERROR reading from socket");
}

void *readAndWriteSocket(void *arg) {
    int sockfd= *(int *) arg;
    char buffer[256]; 

    writeSocket(buffer,sockfd);
    
    readSocket(buffer,sockfd);
    
    //printf("Here is the message: %s\n", buffer);
    return 0;
}
