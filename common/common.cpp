#include "./common.hpp"

mutex pkt_mtx;

using namespace std;

int readpktnum = 0;
int sendpktnum = 0;

void serialize(PACKET *pkt, char data[sizeof(PACKET)])
{
    char *q = (char*)data;
    *q = pkt->type;
	q += sizeof(pkt->type);
	*q = pkt->seqn;
	q += sizeof(pkt->seqn);

	char *r = (char*)q;
    int i = 0;
    while (i < BUFFER_SIZE)
    {
        *r = pkt->_payload[i];
        r++;
        i++;
    }
}

void deserialize(PACKET *pkt, char data[sizeof(PACKET)])
{
    char *q = (char*)data;
    pkt->type = *q;
	q += sizeof(pkt->type);
	pkt->seqn = *q;
	q += sizeof(pkt->seqn);

	char *r = (char*)q;
    int i = 0;
    while (i < BUFFER_SIZE)
    {
        pkt->_payload[i] = *r;
        r++;
        i++;
    }
}

int readSocket(PACKET *pkt, int sock){
    int n = 0;

	char data[sizeof(PACKET)];

	//cout << "reading" << endl;

    while(n < sizeof(PACKET))
    {
    	/* read from the socket */
		int result = read(sock, data+n, sizeof(PACKET)-n);

		if (result > 0)
		{
			n += result;
		}
		else if (result == -1 && errno == EINTR)
		{
			continue;
		}
		else
		{
			// 0 = peer closed the connection, -1 = error. returning here instead of
			// retrying forever is what stops a dead client from pinning a CPU core
			pkt->type = CONEXAO_ENCERRADA;
			memset(pkt->_payload, 0, BUFFER_SIZE);
			return result;
		}
    }

	//cout << "deserializing" << endl;

	deserialize(pkt, data);

	// if(pkt->type == 0 || pkt->type > 60 || n != sizeof(*pkt) || n != sizeof(PACKET)){
	// 	cout << "n: " << n << endl;
	// 	cout << "pkt.type: " << pkt->type << endl;
	// 	cout << "pkt._payload: " << pkt->_payload << endl;
	// 	sleep(60);
	// }

	//cout << "pkt.type: " << pkt->type << ". readpktnum: " << readpktnum << ". pkt.seqn: " << pkt->seqn << ". sock: " << sock << endl;
	readpktnum++;

	return n;
}

int peekSocket(PACKET *pkt, int sock){
	char buffer[sizeof(PACKET)];

	int n = 0;

	while(n < sizeof(PACKET)){
		n = recv(sock, buffer, sizeof(PACKET), MSG_PEEK);
		if (n == -1 && errno == EINTR)
		{
			n = 0;
			continue;
		}
		if (n <= 0)
		{
			pkt->type = CONEXAO_ENCERRADA;
			memset(pkt->_payload, 0, BUFFER_SIZE);
			return n;
		}
	}

	deserialize(pkt, buffer);
	return n;
}

int sendMessage(char message[BUFFER_SIZE], int messageType, int sockfd)
{
	PACKET pkt;

	pkt_mtx.lock();
	pkt.type = messageType;
	pkt.seqn = sendpktnum;
	if (message == nullptr)
	{
		memset(pkt._payload, 0, BUFFER_SIZE);
	}
	else
	{
		memcpy(pkt._payload, message, BUFFER_SIZE);
	}

	char data[sizeof(PACKET)];

	serialize(&pkt, data);

	int n = 0;

    while(n < sizeof(PACKET))
    {
    	/* write to the socket */
		int result = write(sockfd, data+n, sizeof(PACKET)-n);

		if (result >= 0)
		{
			n += result;
		}
		else if (errno != EINTR)
		{
			// connection closed or broken (EPIPE, since SIGPIPE is ignored)
			printf("ERROR writing to socket\n");
			pkt_mtx.unlock();
			return -1;
		}
    }

	//cout << "pkt.type: " << pkt.type << ". sendpktnum: " << sendpktnum << ". sock: " << sockfd << endl;
	sendpktnum++;
	pkt_mtx.unlock();
	return n;
}

int connect_to_server(string server_ip, int PORT){

	hostent *server_host = gethostbyname((char*) server_ip.c_str());

	return connect_to_server(*server_host, PORT);
}

int connect_to_server(hostent server_host){
	return connect_to_server(server_host, DEFAULT_PORT);
}

int connect_to_server(hostent server_host, int PORT){
	int sockfd = -1;
    struct sockaddr_in serv_addr;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);

	int counter = 0;
	while (sockfd == -1 && counter < MAX_RETRIES)
	{
		usleep(WAIT_TIME_BETWEEN_RETRIES);
		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		counter++;
	}
	if(counter == MAX_RETRIES){
		cout << "ERROR opening socket" << endl;
		return sockfd;
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(PORT);
	inet_aton(server_host.h_name, &serv_addr.sin_addr);
	bzero(&(serv_addr.sin_zero), 8);

	counter = 0;
	while (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0 && counter < MAX_RETRIES)
	{
		usleep(WAIT_TIME_BETWEEN_RETRIES);
		counter++;
	}
	if(counter == MAX_RETRIES){
		close(sockfd);
		sockfd = -1;
		cout << "ERROR connecting" << endl;
	}
	return sockfd;
}

//checks for PACKET in socket
bool has_received_message(int sock){
	char buffer[sizeof(PACKET)];
	int n = recv(sock, buffer, sizeof(PACKET), MSG_PEEK);
	return n >= sizeof(PACKET);
}

vector<vector<char>> receiveFileData(int sock){
	PACKET pkt;
	vector<vector<char>> file_data = {};
	cout << "read7" << endl;
	readSocket(&pkt, sock);
	while (pkt.type == MENSAGEM_ENVIO_PARTE_ARQUIVO)
	{
		vector<char> buffer(BUFFER_SIZE);

		for (int i = 0; i < BUFFER_SIZE; i++)
		{
			buffer[i] = pkt._payload[i];
		}
		file_data.push_back(buffer);

		
		//cout << "read77" << endl;
		readSocket(&pkt, sock);
	}

	return file_data;
}

void writeFileData(vector<vector<char>> file_data, string directory, int remainder_file_size){
	ofstream file_received;
	file_received.open(directory, ios_base::binary);
	for (int i = 0; i < file_data.size(); i++)
	{
		for (int j = 0; j < file_data.at(i).size(); j++)
		{
			if (i == file_data.size() - 1)
			{
				if (j == remainder_file_size && remainder_file_size != 0)
				{
					cout << "remainder break" << endl;
					cout << "remainder_file_size: " << remainder_file_size << endl;
					break;
				}
			}
			char *frag = &(file_data.at(i).at(j));
			// printf("%x ", (unsigned char)file_data.at(i).at(j));
			file_received.write(frag, sizeof(char));
		}
	}
	file_received.close();
}

void receiveFile(int sock, string file_path, PACKET *pkt_addr){
	PACKET pkt = *(pkt_addr);

	string file_name = string(pkt._payload);

	cout << "read77" << endl;
	readSocket(&pkt, sock);
	uint32_t* file_size_addr = (uint32_t *) &(pkt._payload);
	uint32_t file_size = *file_size_addr;

	cout << "file_size: " << file_size << endl;

	vector<vector<char>> file_data = receiveFileData(sock);

	//write file data
	int remainder_file_size = file_size % BUFFER_SIZE;
	
	writeFileData(file_data, file_path, remainder_file_size);
}

void sendFile(int sock, string file_path){
	//open file
	ifstream file;
    cout << "sending file" << endl;
    cout << "file path: " << file_path << endl;
	file.open(file_path, ios_base::binary);
	if (!file.is_open())
	{
		cout << "error opening file" << "\n" << endl;
        cout << "write10" << endl;
        sendMessage((char*) "", MENSAGEM_FALHA_ENVIO, sock);
		return;
	}

	cout << "write11" << endl;
	sendMessage((char*) file_path.c_str(), MENSAGEM_ENVIO_NOME_ARQUIVO, sock);

	//get file size
	file.seekg(0, file.end);
	uint32_t file_size = file.tellg();
	cout << "file_size: " << file_size << "\n";
	file.clear();
	file.seekg(0);

	cout << "write122" << endl;
	sendMessage((char*)&file_size, MENSAGEM_ENVIO_TAMANHO_ARQUIVO, sock);
	cout << "file_size: " << file_size << endl;

	//send file pkts
	char buffer[BUFFER_SIZE];
	for (int i = 0; i < file_size; i += ((sizeof(buffer)))) // to read file
	{
		//cout << "i: " << i << endl;
		memset(buffer, 0, BUFFER_SIZE);
		file.read(buffer, sizeof(buffer));
		//for(int j = 0;j<BUFFER_SIZE; j++){
			//printf("%x ", (unsigned char)buffer[i]);
		//}
		
		//cout << "write12" << endl;
		if (sendMessage(buffer, MENSAGEM_ENVIO_PARTE_ARQUIVO, sock) < 0)
		{
			cout << "connection lost while sending file" << endl;
			file.close();
			return;
		}
	}
	cout << "write13" << endl;
	sendMessage(buffer, ACK, sock);
	file.close();
	cout << "file received"
			<< "\n"
			<< endl;
}

string getFileName(string file_path){
	string file_name;

	int last_pos = file_path.find_last_of("/") + 1;
	if(last_pos == 0){
		file_name = file_path;
	}
	else{
		file_name = file_path.substr(last_pos);
	}
	return file_name;
}

bool is_valid_file_name(string file_name){
	if(file_name.empty() || file_name.size() >= BUFFER_SIZE){
		return false;
	}
	if(file_name == "." || file_name == ".."){
		return false;
	}
	return file_name.find('/') == string::npos;
}

bool is_valid_username(string username){
	if(username.empty() || username.size() >= BUFFER_SIZE){
		return false;
	}
	for(int i = 0; i < username.size(); i++){
		char c = username[i];
		if(!isalnum((unsigned char) c) && c != '_' && c != '-'){
			return false;
		}
	}
	return true;
}

void create_thread(
	pthread_t *__restrict thr1,
	const pthread_attr_t *__restrict attr,
	void *(*handle_updates) (void *),
	void *__restrict sockfd_sync)
{
	int i = pthread_create(thr1, attr, handle_updates, sockfd_sync);
	while (i != 0)
	{
		i = pthread_create(thr1, attr, handle_updates, sockfd_sync);
		cout << "ERROR creating thread. retrying..." << endl;
		cout << "ERROR number: " << i << endl;
	}
}

void reconnectToClients(vector<USUARIO> listaDeUsuarios)
{
    struct hostent *client;
    struct sockaddr_in cli_addr, cli_addr2;
    int sockfd;
	PACKET* pack;
	int result;

	cout<<" entrou reconnect \n";

    client = gethostbyname("localhost"); //só para gerar uma estrutura valida -> é sobreescrita posteriormente
    if (client == NULL)
    {
        cout<< stderr << "ERROR, no such host\n";
    }
	cout<<"listaDeUsuarios:" << listaDeUsuarios.size();
    for (auto it : listaDeUsuarios)
    {
		cout<<"for lista de usuarios";
        if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
            printf("ERROR opening socket\n");

        // UM CLI_ADDR PARA CADA SESSÃO
        if (it.sessaoAtiva1)
        {
            cli_addr.sin_family = AF_INET;
            cli_addr.sin_port = htons(2000);
            cli_addr.sin_addr = *((struct in_addr *)client->h_addr);
            cli_addr.sin_addr.s_addr = it.socketAddress1.sin_addr.s_addr;
            bzero(&(cli_addr.sin_zero), 8);

            // inicia conexão com o
            if (connect(it.socketClient1, (struct sockaddr *)&cli_addr, sizeof(cli_addr)) < 0)
                printf("ERROR connecting\n");
        }
        if (it.sessaoAtiva2)
        {
            cli_addr2.sin_family = AF_INET;
            cli_addr2.sin_port = htons(2000);
            cli_addr2.sin_addr = *((struct in_addr *)client->h_addr);
            cli_addr2.sin_addr.s_addr = it.socketAddress2.sin_addr.s_addr;
            bzero(&(cli_addr2.sin_zero), 8);

            // inicia conexão
            if (connect(it.socketClient2, (struct sockaddr *)&cli_addr2, sizeof(cli_addr)) < 0)
                printf("ERROR connecting\n");
        }
    }
}