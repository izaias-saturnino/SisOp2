#include "./server.hpp"

LoginManager *loginManager = new LoginManager();


bool END = false;

vector<int> client_connections;

bool main_server = true;
bool election;
//global variable used as thread argument
vector<SERVER_COPY> election_servers;
//used to check for correct thread cancelation
bool thr_send_election_init = false;
pthread_t thr_send_election;

SERVER_COPY this_server;
SERVER_COPY main_server_copy;
vector<SERVER_COPY> servers;

atomic_int timer_countdown = MAX_TIMER;
atomic_int connection_timer_countdown = MAX_TIMER;

atomic_bool last_number_of_servers = 0;
atomic_int number_of_servers = 0;

//args this_ip other_server_ip this_port other_server_port

int main(int argc, char *argv[])
{
    //arguments test
    verificaRecebimentoIP(argc, argv);

    //host verification
    struct hostent *server;
    server = gethostbyname(argv[1]);

    if (server == NULL)
    {
        fprintf(stderr, "ERROR, no such host\n");
        exit(0);
    }

    srand(time(NULL));

    int PORT = DEFAULT_PORT;

    if(argc > 3){
        PORT = atoi(argv[3]);
    }

    this_server.ip = argv[1];
    this_server.id = -1;
    int other_server_PORT = DEFAULT_PORT;
    if(argc > 4){
        other_server_PORT = atoi(argv[4]);
    }
    this_server.PORT = PORT;

    //handle_ctrlc
    struct sigaction sigIntHandler;
    sigIntHandler.sa_handler = handle_ctrlc;
	sigemptyset(&sigIntHandler.sa_mask);
	sigIntHandler.sa_flags = 0;

    // writing to a socket whose peer is gone must return EPIPE instead of killing the server
    signal(SIGPIPE, SIG_IGN);

    //
    struct sockaddr_in serv_addr;

    int connectionSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (connectionSocket == -1){ /// Verifica IP valido
        cout << "ERROR opening socket\n";
        return 0;
    }

    client_connections.push_back(connectionSocket);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr = *((struct in_addr *)server->h_addr);
    bzero(&(serv_addr.sin_zero), 8);

    cout << "starting server" << endl;
    while(bind(connectionSocket, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0){
        //cout << "ERROR on binding" << endl;
        sleep(1);
    }

    cout << "server started on port " << PORT << endl << endl;

    if(argc > 3){
        char* other_server_ip = argv[2];
        hostent *other_server_host = gethostbyname(other_server_ip);
        main_server = false;
        pthread_t between_server;
        cout << "between server sync starting" << endl;
        int other_server_socket = connect_to_server(*other_server_host, other_server_PORT);
        if(other_server_socket == -1){
            return 0;
        }
        create_thread(&between_server, NULL, share_server_list, &other_server_socket);
    }
    if(main_server){
        this_server.id = get_new_id(servers);
        cout << "this server id: " << this_server.id << endl;
        main_server_copy = this_server;
    }
    servers.push_back(this_server);

    cout << "server sync ended" << endl << endl;

    while (true)
    {
        /*listen to clients*/
        listen(connectionSocket, 30);

        sigaction(SIGINT, &sigIntHandler, NULL);
        if(END){
            break;
        }

        socklen_t clilen;
        sockaddr_in cli_addr;
        clilen = sizeof(struct sockaddr_in);

        int newSockfd;
        newSockfd = accept(connectionSocket, (struct sockaddr *)&cli_addr, &clilen);

        if (newSockfd == -1){
            cout << "ERROR on accept" << endl;
            continue;
        }
        
        client_connections.push_back(newSockfd);

        cout << "read0" << endl;
        PACKET pkt;
        if(peekSocket(&pkt, newSockfd) <= 0){
            cout << "connection closed before first message" << endl;
            client_connections.pop_back();
            close(newSockfd);
            continue;
        }
        if(pkt.type == MENSAGEM_LOGIN){
            // one SESSION per connection (freed by ThreadClient): a single shared
            // variable could be overwritten by the next accept before the thread copied it
            SESSION *session = new SESSION;
            session->socketAddrClient = cli_addr;
            session->socket = newSockfd;
            memcpy(session->user, pkt._payload, BUFFER_SIZE);
            pthread_t clientThread;
            cout << "creating client thread" << endl;
            create_thread(&clientThread, NULL, ThreadClient, session);
        }
        else if(pkt.type == GET_SYNC_DIR){
            cout << "reading GET_SYNC_DIR" << endl;
            readSocket(&pkt, newSockfd);
            char user[BUFFER_SIZE];
            strncpy(user, pkt._payload, BUFFER_SIZE - 1);
            user[BUFFER_SIZE - 1] = '\0';

            if(!is_valid_username(string(user)) || !fs::is_directory("./" + string(user))){
                cout << "GET_SYNC_DIR refused for user: " << user << endl;
                client_connections.pop_back();
                close(newSockfd);
                continue;
            }

            //baixar todos os arquivos do syncdir do servidor
            vector<string> file_paths = get_file_list("./" + string(user));

            for(int i = 0; i < file_paths.size(); i++){
                send_file_to_client(newSockfd, file_paths[i]);
            }

            cout << "sending FIRST_SYNC_END" << endl;
            sendMessage("", FIRST_SYNC_END, newSockfd);
    
            //salvar o socket que pediu atualizações de sync dir
            loginManager->activate_sync_dir(user, newSockfd);

            cout << string(user) << " actived sync dir" << endl;
        }
        else if(pkt.type == LIVENESS_CHECK){
            if(!main_server){
                cout << "ERROR: non main server answering for liveness" << endl;
            }
            pthread_t server_up;
            readSocket(&pkt, newSockfd);
            create_thread(&server_up, NULL, answer_server_up, &newSockfd);
        }
        else if(pkt.type == LIST_SERVERS){
            send_list_of_servers(newSockfd);
            cout << "reading SERVER_ITEMs msg" << endl;
            receive_list_of_servers(newSockfd);
        }
        else if(pkt.type == ID_REQUEST){
            if(!main_server){
                cout << "ERROR: non main server receiving ID_REQUESTS" << endl;
            }
            cout << "reading ID_REQUEST" << endl;
            SERVER_COPY server_copy = receive_server_copy(newSockfd);
            server_copy.id = get_new_id(servers);
            broadcast_new_server(server_copy, SERVER_ITEM);
        }
        else if(pkt.type == SERVER_ITEM){
            int old_id = this_server.id;
            int old_number_of_servers = servers.size();
            SERVER_COPY server_copy = receive_server_copy(newSockfd);
            insert_in_server_list(server_copy);
            if(old_id != this_server.id && has_biggest_id(this_server) && !main_server){
                send_election();
            }
            if(main_server){
                send_election();
            }
            if(old_number_of_servers < servers.size()){
                number_of_servers++;
            }

            //debug
            if(old_id != this_server.id){
                cout << "this_server.id: " << this_server.id;
            }
        }
        else if(pkt.type == ELECTION){
            cout << "reading ELECTION" << endl;
            readSocket(&pkt, newSockfd);
            cout << "sending ELECTION ACK" << endl;
            sendMessage("", ACK, newSockfd);
            send_election();
        }
        else if(pkt.type == ELECTED){
            cout << "reading ELECTED" << endl;
            election = false;
            SERVER_COPY server_copy = receive_server_copy(newSockfd);
            if(this_server.id > server_copy.id){
                cout << "ERROR: this_server.id > server_copy.id should not happen on elected" << endl;
                cout << "this_server.id: " << this_server.id << endl;
                cout << "server_copy.id: " << server_copy.id << endl;
                send_election();
                continue;
            }
            if(this_server.id < server_copy.id){
                main_server = false;
                main_server_copy = server_copy;
                
                pthread_t check_main_server_up_thr;
                create_thread(&check_main_server_up_thr, NULL, check_main_server_up, &server_copy);
            }
            if(this_server.id == server_copy.id){
                main_server = true;
                last_number_of_servers = 0;
            }
            else{
                main_server = false;
            }
            if(main_server){
                cout << "this is the main server" << endl;   
                reconnectToClients(loginManager->listaDeUsuarios);
                cout<<"reconectou" <<endl;
            }
            else{
                cout << "this is not the main server" << endl;
                //cout << "this_server.id: " << this_server.id << endl;
                //cout << "server_copy.id: " << server_copy.id << endl;
            }
        }
        else{
            readSocket(&pkt, newSockfd);
        }
    }
    
    cout << endl << "closing client_connections" << endl;
    close_client_connections();

    return 0;
}

bool str_equals(char* str1, int str1_size, char* str2, int str2_size){
    if(str1_size != str2_size){
        return false;
    }
    bool equals = true;
    for(int i = 0; i < str1_size; i++){
        if(str1[i] != str2[i]){
            equals = false;
            break;
        }
    }
    return equals;
}

void insert_in_server_list(SERVER_COPY server_copy){
    bool found_server = false;
    for(int i = 0; i < servers.size(); i++){
        bool ip_equals = str_equals((char*) (server_copy.ip).c_str(), server_copy.ip.size(), (char*) (servers[i].ip).c_str(), servers[i].ip.size());
        bool port_equals = server_copy.PORT == servers[i].PORT;
        if(ip_equals && port_equals){
            found_server = true;
            servers[i].id = server_copy.id;
            break;
        }
    }

    bool ip_equals = str_equals((char*) (server_copy.ip).c_str(), server_copy.ip.size(), (char*) (this_server.ip).c_str(), this_server.ip.size());
    bool port_equals = server_copy.PORT == this_server.PORT;
    if(ip_equals && port_equals){
        this_server.id = server_copy.id;
    }
    
    if(!found_server){
        servers.push_back(server_copy);
    }
}

void *share_server_list(void *arg){
    int other_server_socket = *(int *)arg;

    if(other_server_socket == -1){
        cout << "ERROR opening other server socket\n";
        return 0;
    }

    send_list_of_servers(other_server_socket);

    cout << "reading LIST_SERVER msg again" << endl;
    receive_list_of_servers(other_server_socket);

    int main_server_socket = connect_to_main_server();

    while(main_server_socket == -1){
        cout << "ERROR: could not connect to main server" << endl;
        send_election();
        return 0;
    }

    //request_id
    cout << "sending ID_REQUEST" << endl;
    send_server_copy(main_server_socket, this_server, ID_REQUEST);
    
    return 0;
}

SERVER_COPY receive_server_copy(int socket){
    SERVER_COPY server_copy;

    PACKET pkt;

    readSocket(&pkt, socket);
    memcpy((char*) &(server_copy.id), pkt._payload, sizeof(server_copy.id));
    readSocket(&pkt, socket);
    memcpy((char*) &(server_copy.PORT), pkt._payload, sizeof(server_copy.PORT));
    int str_size;
    readSocket(&pkt, socket);
    memcpy((char*) &(str_size), pkt._payload, sizeof(str_size));
    readSocket(&pkt, socket);
    server_copy.ip = (string) (pkt._payload);
    cout << "received server_copy.id: " << server_copy.id << endl;

    return server_copy;
}

void send_server_copy(int socket, SERVER_COPY server_copy, int msg_type){
    char buffer[BUFFER_SIZE];
    memcpy(buffer, (char*) &(server_copy.id), sizeof(server_copy.id));
    sendMessage(buffer, msg_type, socket);
    memcpy(buffer, (char*) &(server_copy.PORT), sizeof(server_copy.PORT));
    sendMessage(buffer, msg_type, socket);
    int str_size = server_copy.ip.size();
    memcpy(buffer, (char*) &(str_size), sizeof(str_size));
    sendMessage(buffer, msg_type, socket);
    bzero(buffer, BUFFER_SIZE);
    memcpy(buffer, (char*) (server_copy.ip).c_str(), str_size);
    sendMessage(buffer, msg_type, socket);
    cout << "sended server_copy.id: " << server_copy.id << endl;
}

vector<int> open_broadcast(){
    vector<int> server_connections;
    for (int i = 0; i < servers.size(); i++)
    {
        int socket = connect_to_server(servers[i].ip, servers[i].PORT);
        if(socket != -1){
            server_connections.push_back(socket);
        }
    }
    return server_connections;
}

void close_broadcast(vector<int> server_connections){
    for (int i = 0; i < server_connections.size(); i++)
    {
        close(server_connections[i]);
    }
}

void broadcast_new_server(SERVER_COPY server_copy, int msg_type){
    vector<int> server_connections = open_broadcast();

    //cout << "broadcast type: " << msg_type << endl;
    for (int i = 0; i < server_connections.size(); i++)
    {
        send_server_copy(server_connections[i], server_copy, msg_type);
    }

    close_broadcast(server_connections);
}

void send_list_of_servers(int server_socket){
    cout << "sending LIST_SERVERS msg" << endl;
    sendMessage("", LIST_SERVERS, server_socket);
    for(int i = 0; i < servers.size(); i++){
        cout << "sending SERVER_ITEM" << endl;
        send_server_copy(server_socket, servers[i], SERVER_ITEM);
    }
    cout << "LIST_SERVER end ACK" << endl;
    sendMessage("", ACK, server_socket);
}

void receive_list_of_servers(int server_socket){
    cout << "receive_list_of_servers" << endl;
    PACKET pkt;
    peekSocket(&pkt, server_socket);
    if(pkt.type == LIST_SERVERS){
        readSocket(&pkt, server_socket);
    }
    while(pkt.type == SERVER_ITEM);{
        cout << "reading SERVER_ITEM" << endl;
        SERVER_COPY server_copy = receive_server_copy(server_socket);
        insert_in_server_list(server_copy);
        cout << "peeking SERVER_ITEM" << endl;
        peekSocket(&pkt, server_socket);
    }
    cout << "LIST_SERVER_ITEM end" << endl;
    readSocket(&pkt, server_socket);
}

int host_cmp(char* ip, char* other_ip){
    struct hostent *server = gethostbyname(ip);
    struct hostent *other_server = gethostbyname(other_ip);
    return strcmp(server->h_name, other_server->h_name);
}

bool has_biggest_id(SERVER_COPY server_copy){
    return server_copy.id == main_server_copy.id;
}

int connect_to_main_server(){
    int socket = -1;

    vector<SERVER_COPY> servers_copy = servers;
    sort(servers_copy.begin(), servers_copy.end(), compare_id);

    for (int i = servers.size() - 1; i >= 0; i--)
    {
        if(servers[i].id > this_server.id){
            socket = connect_to_server(servers[i].ip, servers[i].PORT);
            if(socket != -1){
                break;
            }
        }
        else{
            break;
        }
    }

    return socket;
}

bool compare_id(SERVER_COPY copy1, SERVER_COPY copy2){
    return copy1.id < copy2.id;
}

void *send_election(void *arg){
    if(election){
        cout << "duplicate election" << endl;
        return 0;
    }
    cout << "election started" << endl;
    election = true;
    sort(election_servers.begin(), election_servers.end(), compare_id);
    bool possible_main_server = true;
    for (int i = election_servers.size() - 1; i >= 0; i--){
        
        //cout << "send_election election_servers[i].id: " << election_servers[i].id << endl;

        if(election_servers[i].id <= this_server.id){
            break;
        }

        //cout << "send_election election_servers[i].id 2: " << election_servers[i].id << endl;

        pthread_t timer_thr;
        create_thread(&timer_thr, NULL, connection_timer, &election_servers[i]);

        int socket = connect_to_server(election_servers[i].ip, election_servers[i].PORT);
        if(socket == -1){
            continue;
        }

        cout << "sending ELECTION msg" << endl;
        sendMessage("", ELECTION, socket);
        bool message_received = has_received_message(socket);
        
        if(message_received){
            cout << "message received" << endl;
            possible_main_server = false;
            break;
        }
        close(socket);
    }
    if(possible_main_server){
        if(this_server.id == -1){
            this_server.id = get_new_id(servers);
            cout << "this server id: " << this_server.id << endl;
        }
        broadcast_new_server(this_server, ELECTED);
    }
    return 0;
}

void send_election(){
    send_election(servers);
}

void send_election(vector<SERVER_COPY> servers){
    election_servers = servers;
    pthread_t new_thr_send_election;
    create_thread(&new_thr_send_election, NULL, send_election, NULL);
    if(thr_send_election_init){
        pthread_cancel(thr_send_election);
    }
    thr_send_election_init = true;
    thr_send_election = new_thr_send_election;
}

vector<SERVER_COPY> remove_from_server_list(SERVER_COPY server_copy, vector<SERVER_COPY> servers){
    for(int i = 0; i < servers.size(); i++){
        if(server_copy.id == servers[i].id){
            servers.erase(servers.begin()+i);
            break;
        }
    }
    return servers;
}

void remove_from_server_list(SERVER_COPY server_copy){
    servers = remove_from_server_list(server_copy, servers);
}

void* connection_timer(void *arg){
    cout << "connection_timer started" << endl;
    connection_timer_countdown = MAX_TIMER;
    SERVER_COPY server_copy = *(SERVER_COPY *)arg;
    while(true){
        //cout << "connection_timer_countdown: " << connection_timer_countdown << endl;
        usleep(WAIT_TIME_BETWEEN_RETRIES);
        if(connection_timer_countdown <= 0){
            cout << "connection timeout" << endl;
            election = false;
            election_servers = remove_from_server_list(server_copy, election_servers);
            send_election(election_servers);
            break;
        }
        else{
            connection_timer_countdown--;
        }
    }
    return 0;
}

void* timer(void *arg){
    cout << "timer started" << endl;
    timer_countdown = MAX_TIMER;
    while(true){
        usleep(WAIT_TIME_BETWEEN_RETRIES);
        //cout << "timer_countdown: " << timer_countdown << endl;
        if(timer_countdown <= 0){
            cout << "main server timeout" << endl;
            send_election();
            break;
        }
        else{
            timer_countdown--;
        }
    }
    return 0;
}

void* check_main_server_up(void *arg){
    if(main_server){
        cout << "ERROR main_server should not check for liveness" << endl;
        return 0;
    }

    SERVER_COPY server_copy = *(SERVER_COPY *)arg;

    int server_socket = connect_to_server(server_copy.ip, server_copy.PORT);
    
    if(server_socket == -1){
        cout << "ERROR: could not connect to main server" << endl;
        return 0;
    }

    pthread_t timer_thr;
    create_thread(&timer_thr, NULL, timer, NULL);

    PACKET pkt;
    cout << "check_main_server_up running" << endl;
    while(true){
        //cout << "sending LIVENESS_CHECK" << endl;
        //cout << "reading LIVENESS_CHECK ack" << endl;
        if(sendMessage("", LIVENESS_CHECK, server_socket) < 0 || readSocket(&pkt, server_socket) <= 0){
            // stop here; the timer thread keeps counting down and starts the election
            cout << "lost connection to main server" << endl;
            close(server_socket);
            return 0;
        }
        //cout << "timer_countdown == MAX_TIMER" << endl;
        timer_countdown = MAX_TIMER;
    }
    return 0;
}

void *answer_server_up(void *arg){
    int server_socket = *(int *)arg;
    PACKET pkt;
    cout << "answer_server_up running" << endl;
    while(true){
        //cout << "sending LIVENESS_CHECK ack" << endl;
        //cout << "reading LIVENESS_CHECK" << endl;
        if(sendMessage("", ACK, server_socket) < 0 || readSocket(&pkt, server_socket) <= 0){
            cout << "backup server disconnected" << endl;
            close(server_socket);
            return 0;
        }
    }
    return 0;
}

void verificaRecebimentoIP(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "usage %s hostname\n", argv[0]);
        exit(0);
    }
}

void imprimeServerError(void)
{
    fprintf(stderr, "ERROR, no such host\n");
    exit(0);
}

bool client_login(SESSION session){
    int newSockfd = session.socket;
    char user[BUFFER_SIZE];
    memcpy(user, session.user, BUFFER_SIZE);

    cout << "reading client login" << endl;
    PACKET pkt;
    readSocket(&pkt, newSockfd);

    bool login_successful = loginManager->login(newSockfd, user, session.socketAddrClient);

    if (login_successful)
    {
        string path = "./" + string(user);
        cout << path << "\n";
        cout << "user " + string(user) + " logged in" << endl;
        if (!fs::is_directory(path))
        {
            cout << path << "\n";
            create_folder(path);
        }
        cout << "sending MENSAGEM_USUARIO_VALIDO" << endl;
        sendMessage("", MENSAGEM_USUARIO_VALIDO, newSockfd); // Mensagem de usuario Valido
    }
    else
    {
        cout << "sending MENSAGEM_USUARIO_INVALIDO" << endl;
        sendMessage((char*)"Excedido numero de sessoes", MENSAGEM_USUARIO_INVALIDO, newSockfd); // Mensagem de usuario invalido
    }

    return login_successful;
}

vector<int> get_other_servers_sockets(vector<SERVER_COPY> servers){
    vector<int> other_servers_sockets = {};
    for(int i = 0; i < servers.size(); i++){
        if(servers[i].id == this_server.id){
            continue;
        }
        int non_main_server_socket = connect_to_server(servers[i].ip, servers[i].PORT);
        if(non_main_server_socket == -1){
            continue;
        }
        other_servers_sockets.push_back(non_main_server_socket);
    }
    return other_servers_sockets;
}

vector<int> connect_to_non_main_servers(SESSION session){
    vector<int> other_servers_sockets = {};
    other_servers_sockets = clear_connections(other_servers_sockets);
    other_servers_sockets = get_other_servers_sockets(servers);
    for(int i = 0; i < other_servers_sockets.size(); i++){
        sendMessage(session.user, MENSAGEM_LOGIN, other_servers_sockets[i]);
    }
    return other_servers_sockets;
}

void *ThreadClient(void *arg)
{
    SESSION session = *(SESSION *)arg;
    delete (SESSION *)arg;
    int sockfd = session.socket;
    char user[BUFFER_SIZE];
    memcpy(user, session.user, BUFFER_SIZE);
    user[BUFFER_SIZE - 1] = '\0';
    Sockaddr_in sock_addr =  session.socketAddrClient;

    // the username becomes a directory name, so "..", "a/b" etc. would escape ./<user>
    if(!is_valid_username(string(user))){
        cout << "invalid username: " << user << endl;
        if(main_server){
            PACKET login_pkt;
            readSocket(&login_pkt, sockfd);
            sendMessage((char*)"Nome de usuario invalido", MENSAGEM_USUARIO_INVALIDO, sockfd);
        }
        close(sockfd);
        return 0;
    }

    if(main_server){

    bool login_successful = client_login(session);

        if(!login_successful){
            close(sockfd);
            return 0;
        }
    }
    else{
        cout << "Conectando a um servidor nao principal" << endl;
    }

    vector<int> other_servers_sockets = {};
    if(main_server){
        other_servers_sockets = connect_to_non_main_servers(session);
    }

    PACKET pkt;
    char resposta[40];
    ofstream file_server;
    vector<vector<char>> fragments = {};
    string directory;

    bool file_received_from_sync = false;

    while (true)
    {
        memset(pkt._payload, 0, BUFFER_SIZE);
        cout << "reading client msg" << endl;
        if (readSocket(&pkt, sockfd) <= 0)
        {
            // client crashed or lost its connection without sending MENSAGEM_LOGOUT
            cout << string(user) << " connection closed" << endl;
            if(main_server){
                loginManager->Logout(user, sockfd, resposta);
            }
            else{
                close(sockfd);
            }
            break;
        }
        cout << "pkt.type: " << pkt.type << ". ";

        if(last_number_of_servers < number_of_servers && main_server){
            other_servers_sockets = connect_to_non_main_servers(session);
            if(number_of_servers > other_servers_sockets.size()){
                number_of_servers = other_servers_sockets.size();
            }
            last_number_of_servers = number_of_servers;
        }

        // client heartbeat: receiving it is enough (the client detects a dead server
        // when the send fails). not answered, so no reply gets mixed into the
        // command stream, and not forwarded to the backups
        if (pkt.type == MENSAGEM_VERIFICACAO)
        {
            continue;
        }

        if(main_server && pkt.type != MENSAGEM_ENVIO_NOME_ARQUIVO){
            for (int i = 0; i < other_servers_sockets.size(); i++)
            {
                sendMessage(pkt._payload, pkt.type, other_servers_sockets[i]);
            }
        }

        if (pkt.type == MENSAGEM_LOGOUT)
        {
            if(!main_server){
                continue;
            }
            cout << string(user) << " logout" << endl;
            loginManager->Logout(user, sockfd, resposta);
            break;
        }
        if (pkt.type == MENSAGEM_ENVIO_SYNC){
            if(!main_server){
                continue;
            }
            file_received_from_sync = true;
        }
        if (pkt.type == MENSAGEM_ENVIO_NOME_ARQUIVO)
        {
            string file_name = getFileName(string(pkt._payload));
            cout << "receiving file" << endl;
            cout << "file name: " << file_name << endl;
            if(!is_valid_file_name(file_name)){
                // still read the whole transfer so the stream stays in sync
                cout << "invalid file name, discarding upload" << endl;
                receiveFile(sockfd, "/dev/null", &pkt);
                file_received_from_sync = false;
                continue;
            }
            string directory = "./";
            directory = directory + user + "/" + file_name;
            cout << directory << "\n" << endl;

            receiveFile(sockfd, directory, &pkt);

            cout << "file reasembled" << endl;

            vector<int> sync_dir_sockets = loginManager->get_active_sync_dir(user);

            cout << "file location: " << directory << endl;

            cout << "sync_dir_sockets.size(): " << sync_dir_sockets.size() << endl;

            if(!main_server){
                continue;
            }

            cout << "sending file to passive servers" << endl;
            for (int i = 0; i < other_servers_sockets.size(); i++)
            {
                sendFile(other_servers_sockets[i], directory);
            }

            cout << "propagating file" << endl;
            for(int i = 0; i < sync_dir_sockets.size(); i++){
                if(file_received_from_sync){
                    if(sync_dir_sockets[i] == loginManager->get_sender_sync_sock(sockfd)){
                        cout << "sync_dir_sockets[" << i << "]: " << sync_dir_sockets[i] << " não recebe o arquivo." << endl;
                        continue;
                    }
                }
                cout << "sync_dir_sockets[" << i << "]: " << sync_dir_sockets[i] << endl;
                send_file_to_client(sync_dir_sockets[i], directory);
            }
            cout << "file propagated" << endl;
            file_received_from_sync = false;
        }
        if (pkt.type == MENSAGEM_PEDIDO_LISTA_ARQUIVOS_SERVIDOR)
        {
            if(!main_server){
                continue;
            }
            cout << "sending " << string(user) << " file list" << endl;
            vector<string> infos = print_file_list("./" + string(user));
            for (int i = 0; i < infos.size(); i++)
            {
                cout << "sending MENSAGEM_ITEM_LISTA_DE_ARQUIVOS" << endl;
                sendMessage((char*)infos.at(i).c_str(), MENSAGEM_ITEM_LISTA_DE_ARQUIVOS, sockfd);
            }
            cout << "ack MENSAGEM_PEDIDO_LISTA_ARQUIVOS_SERVIDOR" << endl;
            sendMessage("", ACK, sockfd);
        }
        if (pkt.type == MENSAGEM_DELETAR_NO_SERVIDOR){
            string file_name = getFileName(string(pkt._payload));
            cout << "removing file" << endl;
            cout << "file name: " << file_name << endl << endl;
            if(!is_valid_file_name(file_name)){
                // "delete .." used to remove_all() the server's working directory
                cout << "invalid file name, delete refused" << endl;
                continue;
            }
            string file_path = "./";
            file_path = file_path + user + "/" + file_name;

            int result = delete_file(file_path);

            if(result == -1){
				cout << "could not delete file" << endl;
                cout << "file path:" << endl;
			    cout << file_path << endl;
			}

            if(!main_server){
                continue;
            }

            vector<int> sync_dir_sockets = loginManager->get_active_sync_dir(user);

            cout << "file_name: " << file_name << endl;

            for(int i = 0; i < sync_dir_sockets.size(); i++){
                cout << "sync_dir_sockets[" << i << "]: " << sync_dir_sockets[i] << endl;
                cout << "sending MENSAGEM_DELETAR_NOS_CLIENTES" << endl;
                sendMessage((char *)file_name.c_str(), MENSAGEM_DELETAR_NOS_CLIENTES, sync_dir_sockets[i]); // pedido de delete enviado para o cliente
            }
        }
        if (pkt.type == MENSAGEM_DOWNLOAD_FROM_SERVER){
            if(!main_server){
                continue;
            }
            string file_name = getFileName(string(pkt._payload));
            if(!is_valid_file_name(file_name)){
                cout << "invalid file name, download refused" << endl;
                sendMessage((char*) "", MENSAGEM_FALHA_ENVIO, sockfd);
                continue;
            }
            string directory = "./";
            directory = directory + user + "/" + file_name;
            send_file_to_client(sockfd, directory);
        }
    }

    // lets the backups' copies of this session see the connection close
    clear_connections(other_servers_sockets);

    return 0;
}

void handle_ctrlc(int s){
    END = true;
}

vector<int> clear_connections(vector<int> connections){
    for(int i = 0; i < connections.size(); i++){
        close(connections[i]);
    }
    connections.clear();
    return connections;
}

void close_client_connections(){
    client_connections = clear_connections(client_connections);
}

void send_file_to_client(int sock, string file_path)
{
	sendFile(sock, file_path);
}

int get_new_id(vector<SERVER_COPY> servers){
    int id = 100;
    bool unique_id;
    do{
        unique_id = true;
        for(int i = 0; i < servers.size(); i++){
            if(servers[i].id == id){
                unique_id = false;
                id -= 1;
                break;
            }
        }
    }while(!unique_id);
    return id;
}
