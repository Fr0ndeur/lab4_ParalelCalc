import * as net from "net";
import * as readline from "readline";
import { EventEmitter } from "events";

// Server settings
const PORT = 54000;
const SERVER_IP = "127.0.0.1";

// TLV Tags
const TAG_CONFIG = 0x01;
const TAG_MATRIX = 0x02;
const TAG_START_PROCESS = 0x03;
const TAG_STATUS_REQUEST = 0x04;
const TAG_RESULT = 0x05;
const TAG_STATUS_RESP = 0x06;

// Status codes
const STATUS_NOT_STARTED = 0x00;
const STATUS_IN_PROGRESS = 0x01;
const STATUS_FINISHED = 0x02;

class TLVClient extends EventEmitter {
  private socket: net.Socket;
  private buffer = Buffer.alloc(0);

  constructor(host: string, port: number) {
    super();
    this.socket = net.connect(port, host, () => {
      console.log(`Connected to ${host}:${port}`);
      this.emit("ready");
    });
    this.socket.on("data", (chunk: Buffer) => {
      this.buffer = Buffer.concat([this.buffer, chunk]);
      this.parseBuffer();
    });
    this.socket.on("close", () => {
      console.log("Connection closed");
      process.exit(0);
    });
    this.socket.on("error", (err) => {
      console.error("Socket error:", err.message);
      process.exit(1);
    });
  }

  private parseBuffer() {
    while (this.buffer.length >= 5) {
      const tag = this.buffer.readUInt8(0);
      const length = this.buffer.readUInt32BE(1);
      if (this.buffer.length < 5 + length) break;
      const payload = this.buffer.slice(5, 5 + length);
      this.buffer = this.buffer.slice(5 + length);
      this.emit("message", { tag, payload });
    }
  }

  public sendTLV(tag: number, payload: Buffer) {
    const header = Buffer.alloc(5);
    header.writeUInt8(tag, 0);
    header.writeUInt32BE(payload.length, 1);
    this.socket.write(Buffer.concat([header, payload]));
  }

  public waitForMessage(): Promise<{ tag: number; payload: Buffer }> {
    return new Promise((resolve) => {
      const handler = (msg: { tag: number; payload: Buffer }) => {
        this.off("message", handler);
        resolve(msg);
      };
      this.on("message", handler);
    });
  }

  public close() {
    this.socket.end();
  }
}

async function main() {
  const client = new TLVClient(SERVER_IP, PORT);
  await new Promise((res) => client.once("ready", res));

  const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
  });

  const question = (q: string): Promise<string> =>
    new Promise((resolve) =>
      rl.question(q, (answer) => resolve(answer.trim()))
    );

  let n = 0,
    numThreads = 0;
  let configSent = false,
    matrixSent = false;
  let matrix: number[][] = [];

  while (true) {
    console.log(`
Меню:
1. Встановити/оновити конфігурацію
2. Відправити матрицю
3. Запустити обчислення
4. Запитати статус/результат
5. Завершити роботу
`);
    const choice = await question("Виберіть опцію: ");

    switch (choice) {
      case "1": {
        n = parseInt(await question("Введіть розмір матриці (n x n): "), 10);
        numThreads = parseInt(
          await question("Введіть кількість потоків: "),
          10
        );
        const cfg = Buffer.alloc(8);
        cfg.writeUInt32BE(n, 0);
        cfg.writeUInt32BE(numThreads, 4);
        client.sendTLV(TAG_CONFIG, cfg);
        console.log("Конфігурацію відправлено.");
        configSent = true;
        matrixSent = false;
        break;
      }
      case "2": {
        if (!configSent) {
          console.error("Спершу встановіть конфігурацію (опція 1).");
          break;
        }
        matrix = Array.from({ length: n }, () =>
          Array.from({ length: n }, () => Math.floor(Math.random() * 100))
        );
        if (n <= 10) console.table(matrix);
        else console.log(`Матриця розміру ${n}x${n} згенерована.`);
        const buf = Buffer.alloc(n * n * 4);
        let offset = 0;
        for (let i = 0; i < n; i++) {
          for (let j = 0; j < n; j++) {
            buf.writeUInt32BE(matrix[i][j], offset);
            offset += 4;
          }
        }
        client.sendTLV(TAG_MATRIX, buf);
        console.log("Матрицю відправлено.");
        matrixSent = true;
        break;
      }
      case "3": {
        if (!configSent || !matrixSent) {
          console.error(
            "Спершу відправте конфігурацію та матрицю (опції 1 та 2)."
          );
          break;
        }
        client.sendTLV(TAG_START_PROCESS, Buffer.from([0x01]));
        console.log("Команда запуску обчислень відправлена.");
        break;
      }
      case "4": {
        client.sendTLV(TAG_STATUS_REQUEST, Buffer.alloc(0));
        const { tag, payload } = await client.waitForMessage();
        if (tag === TAG_STATUS_RESP) {
          const status = payload.readUInt8(0);
          if (status === STATUS_NOT_STARTED)
            console.log("Статус: Обчислення не запущено.");
          else if (status === STATUS_IN_PROGRESS)
            console.log("Статус: Обчислення в процесі.");
          else console.log("Статус: Невідомий код", status);
        } else if (tag === TAG_RESULT) {
          const result: number[][] = [];
          let off = 0;
          for (let i = 0; i < n; i++) {
            const row: number[] = [];
            for (let j = 0; j < n; j++) {
              row.push(payload.readUInt32BE(off));
              off += 4;
            }
            result.push(row);
          }
          console.log("Обчислення завершено. Отримано результат:");
          if (n <= 10) console.table(result);
          else console.log(`Матриця розміру ${n}x${n} отримана.`);
        } else {
          console.error("Отримано невідомий тег відповіді:", tag);
        }
        break;
      }
      case "5": {
        console.log("Завершення роботи.");
        rl.close();
        client.close();
        return;
      }
      default:
        console.error("Невірна опція, спробуйте ще раз.");
    }
  }
}

main().catch((err) => console.error(err));
