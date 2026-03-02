CXX = g++
CXXFLAGS = -std=c++17 -Wall
LIBS = -lws2_32

SERVER = server
CLIENT = client

all: $(SERVER) $(CLIENT)

$(SERVER): server.cpp
	$(CXX) $(CXXFLAGS) server.cpp -o $(SERVER) $(LIBS)

$(CLIENT): client.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o $(CLIENT) $(LIBS)

clean:
	del /Q $(SERVER).exe $(CLIENT).exe 2>nul || rm -f $(SERVER) $(CLIENT)