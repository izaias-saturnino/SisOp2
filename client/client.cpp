#include "./client.hpp"
#include <mutex>

int main(int argc, char *argv[])
{
	int sockfd, PORT, newSocket;
	socklen_t clilen;
	char buffer[256], username[256];
	struct sockaddr_in serv_addr, cli_addr;
	PACKET receivedPkt;
	string message, servAddr;
	bool Logout = false;

	verificaRecebimentoParametros(argc);

	strcpy(username, argv[1]);
	PORT = atoi(argv[3]);
	servAddr = string(argv[2]);

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		printf("ERROR opening socket");
		exit(0);
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(PORT);
	inet_aton(servAddr.c_str(), &serv_addr.sin_addr);
	bzero(&(serv_addr.sin_zero), 8);

	if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
	{
		printf("ERROR connecting\n");
		exit(0);
	}

	bzero(buffer, 256);

	sendMessage("", 1, 1, 1, username, sockfd); // login message

	readSocket(&receivedPkt, sockfd);

	if (strcmp(receivedPkt._payload, "OK") == 0)
	{
		pthread_t thr1, thr2;
		int n1 = 1;
		int n2 = 2;
		// somente criar se comando get sync dir for ativado pthread_create(&thr1, NULL, folderchecker, (void *)&n1);
		pthread_create(&thr2, NULL, input, (void *)&n2);
		cout << "type exit to end your session \n" << endl;
		while (true)
		{
			if (action != 0)
			{
				std::cout << action << " " << name << "\n";
				action = 0;
			}
			if (command != "")
			{
				std::cout << command;
				if(command=="exit"){
				command = "";
				break;
				}
				if(command=="list_client"){
					print_file_list("./sync_dir");
				}
				pthread_create(&thr2, NULL, input, (void *)&n2);
			}
		}

		sendMessage("", 1, 2, 1, username, sockfd); // logout message

		readSocket(&receivedPkt, sockfd);

		cout << receivedPkt._payload << endl;
	}
	else
	{
		cout << receivedPkt._payload << endl;
	}

	close(sockfd);
	return 0;
}

void verificaRecebimentoParametros(int argc)
{
	if (argc < 3)
	{
		cout << "Faltam parametros" << endl;
		exit(0);
	}
}
