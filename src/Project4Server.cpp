#include "Project4Common.h"
#include <chrono>
#include <ctime>

using std::string;
using std::set;
using std::vector;
using std::ios;
using std::stringstream;
using std::ofstream;
using std::cout;
using std::endl;

void printHelp(char **argv) {
    cout << "Usage: " << *argv << " -p listeningPort [-d directory]" << endl;
    exit(1);
}

void log(const string &logMessage, const string &logFilepath) {
    std::time_t currentTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char *timeStr = std::ctime(&currentTime);
    timeStr[strlen(timeStr) - 1] = '\0';  // drop trailing newline
    stringstream outputMsg;
    outputMsg << "LOG: (Time: " << timeStr << ") " << logMessage << endl;
    ofstream fileWriter(logFilepath, std::ofstream::out | std::ofstream::app);  // append to log file if it exists
    cout << outputMsg.str();
    fileWriter << outputMsg.str();
    fileWriter.close();
}

void doListResponse(int sock, const string &directory) {
    auto files = list(directory);

    vector<json> jsonFiles(files.size());
    for (auto f : files) {
        jsonFiles.push_back(f.getAsJSON(false));
    }

    json listResponsePacket;
    listResponsePacket["version"] = VERSION;
    listResponsePacket["type"] = JSON("listResponse", true);
    listResponsePacket["response"] = jsonFiles;
    sendToSocket(sock, listResponsePacket);
}

void doPullResponse(int sock, const string &directory, const json &pullRequest) {

    json pullResponse;
    pullResponse["version"] = VERSION;
    pullResponse["type"] = JSON("pullResponse", true);
    json emptyArr;
    emptyArr.makeArray();
    pullResponse["response"] = emptyArr;

    vector<MusicData> musicList = list(directory);
    for (auto reqItem : pullRequest["request"]) {
        for (MusicData datum : musicList) {
            if (datum.getFilename() == reqItem["filename"].getString() &&
                datum.getChecksum() == reqItem["checksum"].getString()) {
                pullResponse["response"].push(datum.getAsJSON(true));
            }
        }
    }

    sendToSocket(sock, pullResponse);
}

void doPushResponse(int sock, const string &directory, const json &pushRequest) {
    json pushResponse;
    pushResponse["version"] = VERSION;
    pushResponse["type"] = JSON("pushResponse", true);
    json emptyArr;
    emptyArr.makeArray();
    pushResponse["response"] = emptyArr;

    vector<string> filepaths = directoryFileListing(directory);
    set<string> filenames;
    for (string path : filepaths) {
        filenames.insert(getFilename(path));
    }
    for (auto file : pushRequest["request"]) {
        string filename = directory + filenameIncrement(file["filename"].getString(), filenames);
        writeBase64ToFile(filename, file["data"].getString());
        MusicData d(filename);
        if (d.getChecksum() != file["checksum"].getString()) {
            std::cerr << "Checksum mismatch, probable write or decode error" << endl;
            // Delete the file
            remove(filename.c_str());
        }

        pushResponse["response"].push(d.getAsJSON(false));
    }
    sendToSocket(sock, pushResponse);
}

void closeSocket(int sock, int* clientSockets, int clientSocketIndex){
    clientSockets[clientSocketIndex] = 0;
    close(sock);
}

void handleClient(int sock, const string &directory, int* client_socket, int client_sock_close_index,
                  const string &logFilepath) {
    auto query = receiveUntilByteEquals(sock, '\n');
    try {
        auto queryJ = json(query);  // will throw an exception if invalid JSON received
        debug("Received: " + queryJ.stringify());

        if (verifyJSONPacket(queryJ)) {
            string type = queryJ["type"].getString();

            if (type == "listRequest") {
                log(string("Client at ").append(getPeerStringFromSocket(sock)).append(
                        string(" requested a list of files")), logFilepath);
                doListResponse(sock, directory);
            } else if (type == "pullRequest") {
                log(string("Client at ").append(getPeerStringFromSocket(sock)).append(
                        string(" requested to pull files ")).append(prettyListFiles(queryJ)), logFilepath);
                doPullResponse(sock, directory, queryJ);
            } else if (type == "pushRequest") {
                log(string("Client at ").append(getPeerStringFromSocket(sock)).append(
                        string(" requested to push files ")).append(prettyListFiles(queryJ)), logFilepath);
                doPushResponse(sock, directory, queryJ);
            } else if (type == "leave") {
                log("Client at " + getPeerStringFromSocket(sock) + " cleanly closed connection", logFilepath);
                closeSocket(sock, client_socket, client_sock_close_index);
            } else {
                cout << "Unknown type: " << type << endl;
                closeSocket(sock, client_socket, client_sock_close_index);
            }
        }

        // Loop
    } catch (std::exception &e) {
        log("Client at " + getPeerStringFromSocket(sock) + " unexpectedly closed connection", logFilepath);
        closeSocket(sock, client_socket, client_sock_close_index);
        return;
    }
}

int main(int argc, char **argv) {
    unsigned int serverPort;
    string directory = ".";
    string logFilepath = "serverLog.txt";

    // Select() code
    // ------------------------------------------
    int opt = true;
    const int max_clients = 1024;
    int master_socket;
    int client_socket[max_clients];

    // set of socket descriptors
    fd_set readfds;

    for (int i = 0; i < max_clients; i++) {
        client_socket[i] = 0;
    }

    if ((master_socket = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // set master socket to accept multiple connections (up to 1024)
    if (setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, (char *) &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    InputParser input(argc, argv);
    if (input.findCmdHelp()) {
        printHelp(argv);
    }
    if (input.cmdOptionExists("-p")) {
        serverPort = htons(static_cast<uint16_t>((unsigned int) stoul(input.getCmdOption("-p"))));
    } else {
        printHelp(argv);
    }
    if (input.cmdOptionExists("-d")) {
        string newDirectory = input.getCmdOption("-d");

        if (!isDirectory(newDirectory)) {
            cout << "Could not access provided directory: "
                 << newDirectory << ", are you sure that's a directory?" << endl;
            exit(1);
        }

        directory = newDirectory;
    }

    if (directory.back() != '/' && directory.back() != '\\') {
        directory = directory + '/';
    }

    // Create structs to save the server and client addresses
    struct sockaddr_in serverAddress; /* Local address */

    /* Construct local address structure */
    memset(&serverAddress, 0, sizeof(serverAddress)); /* Zero out structure */
    serverAddress.sin_family = AF_INET; /* Internet address family */
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = serverPort; /* Local port */

    /* Bind to the local address */
    if (bind(master_socket, (struct sockaddr *) &serverAddress, sizeof(serverAddress)) < 0) {
        perror("bind() failed");
        exit(1);
    }

    /* Mark the socket so it will listen for incoming connections */
    if (listen(master_socket, max_clients) < 0) {
        perror("listen() failed");
        exit(1);
    }

    int addrlen = sizeof(serverAddress);

    // Constantly listen for clients
    while (true) {
        // clear the socket set
        FD_ZERO(&readfds);

        // add master socket to set
        FD_SET(master_socket, &readfds);
        int max_sd = master_socket;

        // add child sockets to set
        for (int i = 0; i < max_clients; i++) {
            // socket descriptor
            int sd = client_socket[i];
            // if the socket descriptor is valid, add it to the read list of sockets
            if (sd > 0){
                FD_SET(sd, &readfds);
            }

            // find highest file descriptor number
            if (sd > max_sd){
                max_sd = sd;
            }
        }

        if ((select(max_sd + 1, &readfds, nullptr, nullptr, nullptr) < 0) && (errno != EINTR)) {
            printf("select() error");
        }

        // Handle incoming connection on master socket
        if (FD_ISSET(master_socket, &readfds)) {
            int new_socket;
            if ((new_socket = accept(master_socket, (struct sockaddr *) &serverAddress, (socklen_t *) &addrlen)) < 0) {
                perror("accept");
                exit(EXIT_FAILURE);
            }

            // inform user of socket number - used in send and receive commands
            stringstream msgStream;
            msgStream << "New connection request from client at " << getPeerStringFromSocket(new_socket);
            log(msgStream.str(), logFilepath);

            // add new socket to array of sockets
            for (int i = 0; i < max_clients; i++) {
                // if position is empty
                if (client_socket[i] == 0) {
                    client_socket[i] = new_socket;
                    stringstream s;
                    s << "  Connection request granted; adding to list of sockets as " << i;
                    log(s.str(), logFilepath);
                    break;
                } else if (i == max_clients - 1) {
                    log("  Connection request denied; no more sockets available", logFilepath);
                }
            }
        }

        // Handle IO operations on socket with incoming message
        for (int i = 0; i < max_clients; i++) {
            int sd = client_socket[i];

            if (FD_ISSET(sd, &readfds)) {
                handleClient(sd, directory, client_socket, i, logFilepath);
            }
        }
    }

    return 0;
}
