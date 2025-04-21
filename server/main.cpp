// server.cpp
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <thread>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <cstring>
#include <exception>

#pragma comment(lib, "Ws2_32.lib")
using namespace std;
using namespace std::chrono;

const int PORT = 54000;

const uint8_t TAG_CONFIG = 0x01;
const uint8_t TAG_MATRIX = 0x02;
const uint8_t TAG_START_PROCESS = 0x03;
const uint8_t TAG_STATUS_REQUEST = 0x04;
const uint8_t TAG_RESULT = 0x05;
const uint8_t TAG_STATUS_RESP = 0x06;

const uint8_t STATUS_NOT_STARTED = 0x00;
const uint8_t STATUS_IN_PROGRESS = 0x01;
const uint8_t STATUS_FINISHED = 0x02;

#pragma pack(push, 1)
struct TLVHeader {
    uint8_t tag;
    uint32_t length;
};
#pragma pack(pop)

int recvAll(SOCKET s, char* buffer, int len) {
    int total = 0;
    while (total < len) {
        int rec = recv(s, buffer + total, len - total, 0);
        if (rec <= 0) return rec;
        total += rec;
    }
    return total;
}

int sendAll(SOCKET s, const char* buffer, int len) {
    int sentTotal = 0;
    while (sentTotal < len) {
        int sent = send(s, buffer + sentTotal, len - sentTotal, 0);
        if (sent == SOCKET_ERROR) return SOCKET_ERROR;
        sentTotal += sent;
    }
    return sentTotal;
}

bool sendTLV(SOCKET s, uint8_t tag, const vector<char>& value) {
    TLVHeader hdr{ tag, htonl((uint32_t)value.size()) };
    if (sendAll(s, (const char*)&hdr, sizeof(hdr)) == SOCKET_ERROR) return false;
    if (!value.empty() && sendAll(s, value.data(), (int)value.size()) == SOCKET_ERROR) return false;
    return true;
}

bool recvTLV(SOCKET s, uint8_t& tag, vector<char>& value) {
    TLVHeader hdr;
    if (recvAll(s, (char*)&hdr, sizeof(hdr)) <= 0) return false;
    tag = hdr.tag;
    uint32_t len = ntohl(hdr.length);
    value.resize(len);
    if (len > 0 && recvAll(s, value.data(), (int)len) <= 0) return false;
    return true;
}

bool deserializeMatrix(const vector<char>& buf, int n, vector<vector<int>>& matrix) {
    size_t expected = size_t(n) * n * sizeof(int);
    if (buf.size() != expected) {
        cerr << "[Error] Размер буфера (" << buf.size()
            << ") не равен n*n*sizeof(int) (" << expected << ")\n";
        return false;
    }
    matrix.assign(n, vector<int>(n));
    const char* data = buf.data();
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int netVal;
            memcpy(&netVal, data, sizeof(int));
            matrix[i][j] = ntohl(netVal);
            data += sizeof(int);
        }
    }
    return true;
}

void serializeMatrix(const vector<vector<int>>& matrix, vector<char>& buf) {
    int n = (int)matrix.size();
    buf.resize(n * n * sizeof(int));
    char* data = buf.data();
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int netVal = htonl(matrix[i][j]);
            memcpy(data, &netVal, sizeof(int));
            data += sizeof(int);
        }
    }
}

bool validateMatrix(const vector<vector<int>>& m, int expected) {
    if ((int)m.size() != expected) return false;
    for (auto& row : m)
        if ((int)row.size() != expected) return false;
    return true;
}

void parallelProcessMatrix(vector<vector<int>>& matrix, int numThreads) {
    int n = (int)matrix.size();
    vector<int> minValues(n);
    vector<thread> threads;

    int rowsPerThread = n / numThreads;
    int rem = n % numThreads;
    int start = 0;

    for (int t = 0; t < numThreads; t++) {
        int count = rowsPerThread + (t < rem ? 1 : 0);
        int end = start + count;
        threads.emplace_back([start, end, n, &matrix, &minValues]() {
            for (int i = start; i < end; i++) {
                int col = n - 1 - i;
                int minVal = matrix[0][col];
                for (int j = 1; j < n; j++)
                    if (matrix[j][col] < minVal)
                        minVal = matrix[j][col];
                minValues[i] = minVal;
            }
            });
        start = end;
    }
    for (auto& t : threads) t.join();

    for (int i = 0; i < n; i++) {
        int col = n - 1 - i;
        matrix[i][col] = minValues[i];
    }
}

struct SessionState {
    int n = 0;
    int numThreads = 0;
    vector<vector<int>> matrix;
    vector<vector<int>> resultMatrix;
    bool configReceived = false;
    bool matrixReceived = false;
    bool processingStarted = false;
    bool processingFinished = false;
    mutex mtx;
};

void processingTask(SessionState* state) {
    try {
        vector<vector<int>> localM;
        int threadsCnt, size;
        {
            lock_guard<mutex> lk(state->mtx);
            localM = state->matrix;
            threadsCnt = state->numThreads;
            size = state->n;
        }

        if (threadsCnt <= 0 || threadsCnt > size) {
            cerr << "[Error] Некорректное число потоков: " << threadsCnt << "\n";
            lock_guard<mutex> lk(state->mtx);
            state->processingFinished = true;
            return;
        }
        if (!validateMatrix(localM, size)) {
            cerr << "[Error] Размер матрицы не совпадает с конфигом: ожидалось "
                << size << "x" << size << "\n";
            lock_guard<mutex> lk(state->mtx);
            state->processingFinished = true;
            return;
        }

        auto t0 = high_resolution_clock::now();
        parallelProcessMatrix(localM, threadsCnt);
        auto t1 = high_resolution_clock::now();
        auto dur = duration_cast<milliseconds>(t1 - t0).count();
        cout << "Обробка завершена за " << dur << " мс\n";

        lock_guard<mutex> lk(state->mtx);
        state->resultMatrix = move(localM);
        state->processingFinished = true;
    }
    catch (const exception& e) {
        cerr << "[Exception] в processingTask: " << e.what() << "\n";
        lock_guard<mutex> lk(state->mtx);
        state->processingFinished = true;
    }
    catch (...) {
        cerr << "[Unknown exception] в processingTask\n";
        lock_guard<mutex> lk(state->mtx);
        state->processingFinished = true;
    }
}

void clientHandler(SOCKET clientSock) {
    try {
        cout << "Новий клієнт підключився.\n";
        SessionState state;
        uint8_t tag;
        vector<char> payload;

        while (recvTLV(clientSock, tag, payload)) {
            switch (tag) {
            case TAG_CONFIG: {
                {
                    lock_guard<mutex> lk(state.mtx);
                    if (state.processingStarted && !state.processingFinished) {
                        cerr << "[Error] Невозможно изменить конфиг во время обработки\n";
                        break;
                    }
                }
                if (payload.size() != 8) {
                    cerr << "[Error] Неверный размер CONFIG\n";
                    break;
                }
                uint32_t n_net, th_net;
                memcpy(&n_net, payload.data(), 4);
                memcpy(&th_net, payload.data() + 4, 4);
                int n = ntohl(n_net);
                int threads = ntohl(th_net);
                {
                    lock_guard<mutex> lk(state.mtx);
                    state.n = n;
                    state.numThreads = threads;
                    state.configReceived = true;
                    state.matrixReceived = false;
                    state.processingStarted = false;
                    state.processingFinished = false;
                }
                cout << "Отримано CONFIG: n=" << n << ", threads=" << threads << "\n";
                sendTLV(clientSock, TAG_STATUS_RESP, vector<char>(1, STATUS_NOT_STARTED));
                break;
            }

            case TAG_MATRIX: {
                {
                    lock_guard<mutex> lk(state.mtx);
                    if (state.processingStarted && !state.processingFinished) {
                        cerr << "[Error] Невозможно отправить новую матрицу во время обработки\n";
                        break;
                    }
                    if (!state.configReceived) {
                        cerr << "[Error] Конфиг не встановлено\n";
                        break;
                    }
                }
                if (!deserializeMatrix(payload, state.n, state.matrix)) {
                    break;
                }
                {
                    lock_guard<mutex> lk(state.mtx);
                    state.matrixReceived = true;
                }
                cout << "Матриця отримана (" << state.n << "x" << state.n << ")\n";
                break;
            }

            case TAG_START_PROCESS: {
                {
                    lock_guard<mutex> lk(state.mtx);
                    if (!state.configReceived || !state.matrixReceived) {
                        cerr << "[Error] Недостатньо даних для початку обчислень\n";
                        break;
                    }
                    if (state.processingStarted && !state.processingFinished) {
                        cerr << "[Error] Обчислення вже запущено\n";
                        break;
                    }
                    state.processingStarted = true;
                    state.processingFinished = false;
                }
                cout << "Запуск обчислень...\n";
                thread(processingTask, &state).detach();
                sendTLV(clientSock, TAG_STATUS_RESP, vector<char>(1, STATUS_IN_PROGRESS));
                break;
            }

            case TAG_STATUS_REQUEST: {
                uint8_t status;
                {
                    lock_guard<mutex> lk(state.mtx);
                    if (!state.processingStarted)
                        status = STATUS_NOT_STARTED;
                    else if (!state.processingFinished)
                        status = STATUS_IN_PROGRESS;
                    else
                        status = STATUS_FINISHED;
                }
                if (status != STATUS_FINISHED) {
                    sendTLV(clientSock, TAG_STATUS_RESP, vector<char>(1, status));
                }
                else {
                    vector<char> resultBuf;
                    serializeMatrix(state.resultMatrix, resultBuf);
                    sendTLV(clientSock, TAG_RESULT, resultBuf);
                }
                break;
            }

            default:
                cerr << "[Warning] Невідомий тег: " << (int)tag << "\n";
                break;
            }
        }
        cout << "Клієнт відключився.\n";
    }
    catch (const exception& e) {
        cerr << "[Exception] в clientHandler: " << e.what() << "\n";
    }
    catch (...) {
        cerr << "[Unknown exception] в clientHandler\n";
    }
    closesocket(clientSock);
}

int main() {
    WSADATA ws;
    if (WSAStartup(MAKEWORD(2, 2), &ws) != 0) {
        cerr << "WSAStartup помилка\n";
        return -1;
    }
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) {
        cerr << "Не вдалося створити сокет\n";
        WSACleanup();
        return -1;
    }

    sockaddr_in hint{};
    hint.sin_family = AF_INET;
    hint.sin_port = htons(PORT);
    hint.sin_addr.s_addr = INADDR_ANY;
    if (bind(listenSock, (sockaddr*)&hint, sizeof(hint)) == SOCKET_ERROR) {
        cerr << "Bind помилка\n";
        closesocket(listenSock);
        WSACleanup();
        return -1;
    }
    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        cerr << "Listen помилка\n";
        closesocket(listenSock);
        WSACleanup();
        return -1;
    }

    cout << "Сервер запущено на порті " << PORT << "\n";
    while (true) {
        sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &addrLen);
        if (clientSock == INVALID_SOCKET) {
            cerr << "[Warning] accept() помилка\n";
            continue;
        }
        thread(clientHandler, clientSock).detach();
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}
