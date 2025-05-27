#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#endif // __linux
#ifdef WIN32
#include <ws2tcpip.h>
#endif // WIN32
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>

#ifdef WIN32
void myerror(const char* msg) { fprintf(stderr, "%s %lu\n", msg, GetLastError()); }
#else
void myerror(const char* msg) { fprintf(stderr, "%s %s %d\n", msg, strerror(errno), errno); }
#endif

std::vector<int> connectedClients;
std::mutex clientsMutex;

void usage() {
	printf("syntax : echo-server <port> [-e[-b]]\nsample : echo-server 1234 -e -b\n");
}

struct Param {
	bool echo{false};
	bool broadcast{false};
	uint16_t port{0};
	uint32_t srcIp{INADDR_ANY};

	bool parse(int argc, char* argv[]) {
		for (int i = 1; i < argc;) {
			if (strcmp(argv[i], "-e") == 0) {
				echo = true;
				i++;
				continue;
			}

			if (strcmp(argv[i], "-b") == 0) {
				broadcast = true;
				i++;
				continue;
			}

			if (strcmp(argv[i], "-si") == 0) {
				if (i + 1 >= argc) {
					fprintf(stderr, "Missing IP address after -si\n");
					return false;
				}
				int res = inet_pton(AF_INET, argv[i + 1], &srcIp);
				switch (res) {
					case 1: break;
					case 0: fprintf(stderr, "not a valid network address\n"); return false;
					case -1: myerror("inet_pton"); return false;
				}
				i += 2;
				continue;
			}

			port = atoi(argv[i]);
			if (port == 0) {
				fprintf(stderr, "Invalid port number: %s\n", argv[i]);
				return false;
			}
			i++;
		}
		return port != 0;
	}
} param;

void addClient(int sd) {
	std::lock_guard<std::mutex> lock(clientsMutex);
	connectedClients.push_back(sd);
}

void removeClient(int sd) {
	std::lock_guard<std::mutex> lock(clientsMutex);
	connectedClients.erase(std::remove(connectedClients.begin(), connectedClients.end(), sd), connectedClients.end());
}

void broadcastMessage(int senderSd, const char* buf, ssize_t len) {
	std::lock_guard<std::mutex> lock(clientsMutex);
	for (int clientSd : connectedClients) {
		ssize_t res = ::send(clientSd, buf, len, 0);
		if (res == -1) {
			fprintf(stderr, "Failed to broadcast to client %d", clientSd);
			myerror(" ");
		}
	}
}

void recvThread(int sd) {
	printf("Client connected (socket %d)\n", sd);
	fflush(stdout);

	addClient(sd);

	static const int BUFSIZE = 65536;
	char buf[BUFSIZE];
	while (true) {
		ssize_t res = ::recv(sd, buf, BUFSIZE - 1, 0);
		if (res == 0 || res == -1) {
			if (res == 0) {
				printf("Client disconnected (socket %d)\n", sd);
			} else {
				fprintf(stderr, "recv return %zd for socket %d", res, sd);
				myerror(" ");
			}
			break;
		}
		buf[res] = '\0';
		printf("Client %d: %s", sd, buf);
		fflush(stdout);

		if (param.echo) {
			if (param.broadcast) {
				broadcastMessage(sd, buf, res);
			} else {
				res = ::send(sd, buf, res, 0);
				if (res == 0 || res == -1) {
					fprintf(stderr, "send return %zd for socket %d", res, sd);
					myerror(" ");
					break;
				}
			}
		}
	}

	removeClient(sd);
	::close(sd);
}

int main(int argc, char* argv[]) {
	if (!param.parse(argc, argv)) {
		usage();
		return -1;
	}

#ifdef WIN32
	WSAData wsaData;
	WSAStartup(0x0202, &wsaData);
#endif // WIN32

	//
	// socket
	//
	int sd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (sd == -1) {
		myerror("socket");
		return -1;
	}

#ifdef __linux__
	//
	// setsockopt
	//
	{
		int optval = 1;
		int res = ::setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
		if (res == -1) {
			myerror("setsockopt");
			return -1;
		}
	}
#endif // __linux

	//
	// bind
	//
	{
		struct sockaddr_in addr;
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = param.srcIp;
		addr.sin_port = htons(param.port);

		ssize_t res = ::bind(sd, (struct sockaddr *)&addr, sizeof(addr));
		if (res == -1) {
			myerror("bind");
			return -1;
		}
	}

	//
	// listen
	//
	{
		int res = listen(sd, 5);
		if (res == -1) {
			myerror("listen");
			return -1;
		}
	}

	while (true) {
		struct sockaddr_in addr;
		socklen_t len = sizeof(addr);
		int newsd = ::accept(sd, (struct sockaddr *)&addr, &len);
		if (newsd == -1) {
			myerror("accept");
			break;
		}

		std::thread clientThread(recvThread, newsd);
		clientThread.detach();
	}
	::close(sd);
}
