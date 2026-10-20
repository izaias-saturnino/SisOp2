CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -pthread
LDFLAGS ?= -pthread
LDLIBS := -lssl -lcrypto

BUILD := build

COMMON := common/log.cpp common/protocol.cpp common/connection.cpp common/net.cpp common/files.cpp \
          common/tls.cpp
SERVER := server/main.cpp server/server.cpp server/storage.cpp server/sessions.cpp server/cluster.cpp \
          server/accounts.cpp server/health.cpp
CLIENT := client/main.cpp client/client.cpp client/sync_state.cpp client/watcher.cpp

COMMON_OBJ := $(COMMON:%.cpp=$(BUILD)/obj/%.o)
SERVER_OBJ := $(SERVER:%.cpp=$(BUILD)/obj/%.o)
CLIENT_OBJ := $(CLIENT:%.cpp=$(BUILD)/obj/%.o)

.PHONY: all clean test certs

all: $(BUILD)/server $(BUILD)/client

$(BUILD)/server: $(SERVER_OBJ) $(COMMON_OBJ)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/client: $(CLIENT_OBJ) $(COMMON_OBJ)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

test: all
	tests/integration.sh

# A private CA plus a server certificate for localhost; run tools/make-certs.sh
# directly to add more host names.
certs:
	tools/make-certs.sh certs

clean:
	rm -rf $(BUILD)

-include $(COMMON_OBJ:.o=.d) $(SERVER_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d)
