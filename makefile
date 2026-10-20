build:
	g++ -std=c++17 server/server.cpp login/login.cpp common/common.cpp -o server/server -lpthread -lstdc++fs && \
	g++ -std=c++17 client/client.cpp common/common.cpp -o client/client -lpthread -lstdc++fs
