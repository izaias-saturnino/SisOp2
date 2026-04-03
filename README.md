# Distributed File Synchronization System

## Overview

This project implements a client–server distributed file synchronization service written in C++ for Linux environments. The system allows authenticated users to maintain synchronized directories across multiple client instances connected to a central server. File changes in a local sync directory are detected automatically and propagated to the server and to other active client sessions of the same user.

The architecture is based on TCP sockets, custom packet serialization, multithreading, and Linux filesystem monitoring (inotify).

---

## Main Features

* User authentication with session management
* Two simultaneous client sessions per user
* Automatic detection of file changes (create, delete, modify, move)
* Bidirectional file synchronization between client and server
* Initial full synchronization when client connects
* Incremental synchronization during runtime
* Custom reliable packet protocol over TCP
* Multi-threaded client and server design

---

## System Architecture

### Components

Server
Responsible for:

* User authentication and session tracking
* Maintaining user directories
* Receiving file updates from clients
* Broadcasting updates to other client sessions

Client
Responsible for:

* Login/logout operations
* Monitoring the local sync directory
* Sending file updates to server
* Receiving updates from server and applying them locally

Common Module
Shared networking utilities:

* Packet structure definition
* Serialization/deserialization
* Reliable socket read/write wrappers

Directory Manager
Handles filesystem operations:

* Directory creation
* File listing and metadata
* File deletion and comparisons

File Watcher
Uses inotify to detect local filesystem changes.

---

## Networking Protocol

Communication uses a custom packet structure:

```
typedef struct {
    uint16_t type;
    uint16_t seqn;
    uint32_t file_byte_size;
    char user[BUFFER_SIZE];
    char _payload[BUFFER_SIZE];
} PACKET;
```

Packets are serialized manually before transmission and deserialized upon reception to guarantee fixed-size transfers.

### Message Types

Key protocol messages include:

Authentication

* MENSAGEM_LOGIN
* MENSAGEM_LOGOUT
* MENSAGEM_USUARIO_VALIDO
* MENSAGEM_USUARIO_INVALIDO

File Synchronization

* MENSAGEM_ENVIO_NOME_ARQUIVO
* MENSAGEM_ENVIO_PARTE_ARQUIVO
* MENSAGEM_ENVIO_PARTE_ARQUIVO_SYNC
* MENSAGEM_PEDIDO_LISTA_ARQUIVOS_SERVIDOR
* MENSAGEM_ITEM_LISTA_DE_ARQUIVOS
* MENSAGEM_DELETAR_NO_SERVIDOR
* MENSAGEM_DELETAR_NOS_CLIENTES
* MENSAGEM_DOWNLOAD_NO_SERVIDOR

Control

* ACK
* FIRST_SYNC_END
* GET_SYNC_DIR

---

## Synchronization Workflow

### 1) Client Login

1. Client sends login packet with username.
2. Server validates user and registers session.
3. If valid, synchronization begins.

### 2) Initial Sync

1. Client requests server file list.
2. Client compares server files with local directory.
3. Missing files are downloaded from server.
4. Extra local files are uploaded.

### 3) Continuous Sync

Client runs a background thread using inotify to detect:

* File creation
* File deletion
* File modification
* File moves

Detected events are queued and sent to server.

Server then:

1. Updates user directory.
2. Propagates change to second active session (if exists).

---

## Multithreading Model

### Client Threads

1. Main thread
   Handles connection and message exchange.

2. Folder watcher thread
   Monitors filesystem events using inotify.

3. Input thread
   Handles terminal commands (logout, exit).

### Server Threads

Each client connection is handled in its own thread, allowing multiple concurrent users.

Mutexes protect:

* Packet send sequence numbers
* File event queues
* Shared session structures

---

## File Monitoring (inotify)

Watched events:

* IN_CREATE
* IN_DELETE
* IN_MODIFY
* IN_MOVED_TO
* IN_MOVED_FROM

Events are converted into actions:

* FILE_CREATED
* FILE_DELETED
* FILE_MODIFIED

These events trigger synchronization messages to the server.

---

## Directory Structure

Expected runtime structure:

Server side:

```
server/
  users/
    <username>/
      synced files
```

Client side:

```
client/
  sync_dir/
      local files to sync
```

---

## Build Instructions

Requirements:

* Linux OS
* g++ with C++17 support
* POSIX libraries (pthread, sockets, inotify)

Compile server:

```
g++ -std=c++17 server.cpp login.cpp sync.cpp dir_manager.cpp common.cpp -lpthread -o server
```

Compile client:

```
g++ -std=c++17 client.cpp filewatcher.cpp dir_manager.cpp common.cpp -lpthread -o client
```

---

## Running the System

Start the server:

```
./server <port>
```

Start the client:

```
./client <server_ip> <port>
```

After login, the client automatically begins synchronization with the server.

---

## Known Limitations

* No encryption (plain TCP)
* No checksum verification for file integrity
* No conflict resolution (last write wins)
* File transfers do not support resume

---

## Possible Improvements

* Add TLS encryption
* Implement file hashing and integrity checks
* Conflict detection and versioning
* Resume interrupted transfers
* Replace polling loops with event-driven networking
