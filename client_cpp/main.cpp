// client.cpp
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <random>
#include <iomanip>
#include <cstring>

#pragma comment(lib, "Ws2_32.lib")
using namespace std;

const int PORT = 54000;
const char* SERVER_IP = "127.0.0.1";

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
    int totalReceived = 0;
    while (totalReceived < len) {
        int rec = recv(s, buffer + totalReceived, len - totalReceived, 0);
        if (rec <= 0)
            return rec;
        totalReceived += rec;
    }
    return totalReceived;
}

int sendAll(SOCKET s, const char* buffer, int len) {
    int totalSent = 0;
    while (totalSent < len) {
        int sent = send(s, buffer + totalSent, len - totalSent, 0);
        if (sent == SOCKET_ERROR)
            return SOCKET_ERROR;
        totalSent += sent;
    }
    return totalSent;
}

bool sendTLV(SOCKET s, uint8_t tag, const vector<char>& value) {
    TLVHeader header{ tag, htonl(static_cast<uint32_t>(value.size())) };
    if (sendAll(s, reinterpret_cast<const char*>(&header), sizeof(header)) == SOCKET_ERROR) {
        return false;
    } 
    if (!value.empty() && sendAll(s, value.data(), value.size()) == SOCKET_ERROR) {
        return false;
    }
    return true;
}

bool recvTLV(SOCKET s, uint8_t& tag, vector<char>& value) {
    TLVHeader header;
    if (recvAll(s, reinterpret_cast<char*>(&header), sizeof(header)) <= 0) {
        return false;
    }
    tag = header.tag;
    uint32_t len = ntohl(header.length);
    value.resize(len);
    if (len > 0 && recvAll(s, value.data(), len) <= 0) {
        return false;
    }
    return true;
}

void fillMatrix(vector<vector<int>>& matrix) {
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dis(0, 99);
    int n = matrix.size();
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            matrix[i][j] = dis(gen);
        }
    }
}

void printMatrix(const vector<vector<int>>& matrix) {
    int n = matrix.size();
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            cout << setw(4) << matrix[i][j] << " ";
        }
        cout << endl;
    }
}

void serializeMatrix(const vector<vector<int>>& matrix, vector<char>& buffer) {
    int n = matrix.size();
    buffer.resize(n * n * sizeof(int));
    char* data = buffer.data();
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int netVal = htonl(matrix[i][j]);
            memcpy(data, &netVal, sizeof(int));
            data += sizeof(int);
        }
    }
}

bool deserializeMatrix(const vector<char>& buffer, int n, vector<vector<int>>& matrix) {
    if (buffer.size() != n * n * sizeof(int))
        return false;
    matrix.resize(n, vector<int>(n));
    const char* data = buffer.data();
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

void interactiveClient(SOCKET sock) {
    int n = 0, numThreads = 0;
    vector<vector<int>> matrix;
    bool configSent = false, matrixSent = false;
    bool exitFlag = false;
    while (!exitFlag) {
        cout << "\nМеню:\n";
        cout << "1. Встановити/оновити конфігурацію\n";
        cout << "2. Відправити матрицю\n";
        cout << "3. Запустити обчислення\n";
        cout << "4. Запитати статус/результат\n";
        cout << "5. Завершити роботу\n";
        cout << "Виберіть опцію: ";
        int choice;
        cin >> choice;
        switch (choice) {
        case 1: {
            cout << "Введіть розмір матриці (n x n): ";
            cin >> n;
            cout << "Введіть кількість потоків: ";
            cin >> numThreads;
            vector<char> configPayload(8);
            uint32_t n_net = htonl(n);
            uint32_t threads_net = htonl(numThreads);
            memcpy(configPayload.data(), &n_net, sizeof(uint32_t));
            memcpy(configPayload.data() + 4, &threads_net, sizeof(uint32_t));
            if (sendTLV(sock, TAG_CONFIG, configPayload))
                cout << "Конфігурацію відправлено." << endl;
            else
                cerr << "Помилка надсилання конфігурації." << endl;
            configSent = true;
            matrixSent = false;
            break;
        }
        case 2: {
            if (!configSent) {
                cerr << "Спершу встановіть конфігурацію (опція 1)." << endl;
                break;
            }
            matrix.resize(n, vector<int>(n));
            fillMatrix(matrix);
            if (n <= 10) {
                cout << "Згенерована матриця:\n";
                printMatrix(matrix);
            }
            else {
                cout << "Матриця розміру " << n << "x" << n << " згенерована." << endl;
            }
            vector<char> matrixPayload;
            serializeMatrix(matrix, matrixPayload);
            if (sendTLV(sock, TAG_MATRIX, matrixPayload))
                cout << "Матрицю відправлено." << endl;
            else
                cerr << "Помилка надсилання матриці." << endl;
            matrixSent = true;
            break;
        }
        case 3: {
            if (!configSent || !matrixSent) {
                cerr << "Спершу відправте конфігурацію та матрицю (опції 1 та 2)." << endl;
                break;
            }
            vector<char> cmdPayload(1, 0x01);
            if (sendTLV(sock, TAG_START_PROCESS, cmdPayload))
                cout << "Команда запуску обчислень відправлена." << endl;
            else
                cerr << "Помилка надсилання команди." << endl;
            break;
        }
        case 4: {
            vector<char> empty;
            if (!sendTLV(sock, TAG_STATUS_REQUEST, empty)) {
                cerr << "Помилка надсилання запиту статусу." << endl;
                break;
            }
            uint8_t respTag;
            vector<char> respPayload;
            if (!recvTLV(sock, respTag, respPayload)) {
                cerr << "Помилка отримання відповіді." << endl;
                break;
            }
            if (respTag == TAG_STATUS_RESP) {
                if (respPayload.size() != 1) {
                    cerr << "Невірний формат відповіді статусу." << endl;
                    break;
                }
                uint8_t status = respPayload[0];
                if (status == STATUS_NOT_STARTED)
                    cout << "Статус: Обчислення не запущено." << endl;
                else if (status == STATUS_IN_PROGRESS)
                    cout << "Статус: Обчислення в процесі." << endl;
                else
                    cout << "Невідомий статус." << endl;
            }
            else if (respTag == TAG_RESULT) {
                vector<vector<int>> resultMatrix;
                if (deserializeMatrix(respPayload, n, resultMatrix)) {
                    cout << "Обчислення завершено. Отримано результат:" << endl;
                    if (n <= 10)
                        printMatrix(resultMatrix);
                    else
                        cout << "Матриця розміру " << n << "x" << n << " отримана." << endl;
                }
                else {
                    cerr << "Помилка десеріалізації матриці результату." << endl;
                }
            }
            else {
                cerr << "Отримано невідомий тег відповіді: " << (int)respTag << endl;
            }
            break;
        }
        case 5: {
            exitFlag = true;
            cout << "Завершення роботи." << endl;
            break;
        }
        default:
            cout << "Невірна опція. Спробуйте ще раз." << endl;
            break;
        }
    }
}

int main() {
    WSADATA wsData;
    if (WSAStartup(MAKEWORD(2, 2), &wsData) != 0) {
        cerr << "WSAStartup помилка" << endl;
        return -1;
    }
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        cerr << "Створення сокета невдале" << endl;
        WSACleanup();
        return -1;
    }
    sockaddr_in hint;
    hint.sin_family = AF_INET;
    hint.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &hint.sin_addr);
    if (connect(sock, (sockaddr*)&hint, sizeof(hint)) == SOCKET_ERROR) {
        cerr << "Підключення до сервера невдале" << endl;
        closesocket(sock);
        WSACleanup();
        return -1;
    }
    cout << "З'єднання встановлено з сервером " << SERVER_IP << ":" << PORT << endl;

    interactiveClient(sock);

    closesocket(sock);
    WSACleanup();
    return 0;
}
