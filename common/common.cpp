#include "./common.hpp"

mutex pkt_mtx;

using namespace std;

int readpktnum = 0;
int sendpktnum = 0;

void serialize(PACKET *pkt, char data[sizeof(PACKET)])
{
    int offset = 0;

    memcpy(data + offset, &pkt->type, sizeof(pkt->type));
    offset += sizeof(pkt->type);

    memcpy(data + offset, &pkt->seqn, sizeof(pkt->seqn));
    offset += sizeof(pkt->seqn);

    memcpy(data + offset, &pkt->file_byte_size, sizeof(pkt->file_byte_size));
    offset += sizeof(pkt->file_byte_size);

    memcpy(data + offset, pkt->user, BUFFER_SIZE);
    offset += BUFFER_SIZE;

    memcpy(data + offset, pkt->_payload, BUFFER_SIZE);
}

void deserialize(PACKET *pkt, char data[sizeof(PACKET)])
{
    int offset = 0;

    memcpy(&pkt->type, data + offset, sizeof(pkt->type));
    offset += sizeof(pkt->type);

    memcpy(&pkt->seqn, data + offset, sizeof(pkt->seqn));
    offset += sizeof(pkt->seqn);

    memcpy(&pkt->file_byte_size, data + offset, sizeof(pkt->file_byte_size));
    offset += sizeof(pkt->file_byte_size);

    memcpy(pkt->user, data + offset, BUFFER_SIZE);
    offset += BUFFER_SIZE;

    memcpy(pkt->_payload, data + offset, BUFFER_SIZE);
}

int readSocket(PACKET *pkt, int sock){
    int n = 0;

	char data[sizeof(PACKET)];

	//cout << "reading" << endl;

    while(n < sizeof(PACKET))
    {
    	/* read from the socket */
		int result = read(sock, data+n, sizeof(PACKET)-n);

		if (result >= 0)
		{
			n += result;
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

	//cout << "pkt.type: " << pkt->type;
	//cout << ". readpktnum: " << readpktnum;
	//cout << ". pkt.seqn: " << pkt->seqn;
	//cout << ". sock: " << sock << endl;
	readpktnum++;

	return n;
}

void sendMessage(char message[BUFFER_SIZE], uint32_t file_byte_size, int messageType, int fragmentos, char username[BUFFER_SIZE], int sockfd)
{
	PACKET pkt;
	
	pkt_mtx.lock();
	pkt.type = messageType;
	pkt.seqn = sendpktnum;
	strcpy(pkt.user, username);
	memcpy(pkt._payload, message, BUFFER_SIZE);
	pkt.file_byte_size = file_byte_size;

	char data[sizeof(PACKET)];

	serialize(&pkt, data);

	int n = 0;

    while(n < sizeof(PACKET))
    {
    	/* read from the socket */
		int result = write(sockfd, data+n, sizeof(PACKET)-n);

		if (result >= 0)
		{
			n += result;
		}
    }

	if (n < 0)
		printf("ERROR writing to socket\n");
	if (n != sizeof(pkt) || n != sizeof(PACKET)){
		printf("ERROR writing all data to socket\n");
		// cout << "n: " << n << endl;
		// cout << "pkt.type: " << pkt.type << endl;
		// cout << "pkt._payload: " << pkt._payload << endl;
		// if(pkt.type == 0 || pkt.type > 60){
		// 	sleep(60);
		//}
	}

	//cout << "pkt.type: " << pkt.type;
	//cout << ". sendpktnum: " << sendpktnum;
	//cout << ". sock: " << sockfd << endl;
	sendpktnum++;
	pkt_mtx.unlock();
}

