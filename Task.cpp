#include <windows.h>
#include <iostream>
#include <string>
#include <cstdint>

enum Status : uint8_t {
    ST_OK    = 1,  // 001
    ST_FAIL  = 2,  // 010
    ST_UNDEF = 4   // 100
};

// Кодуємо у 1 байт: 4 біти статусу + 4 біти маленького результату
uint8_t encode_exit(Status st, int smallValue) {
    if (smallValue < 0) smallValue = 0;
    if (smallValue > 15) smallValue = 15; // максимум 4 біти
    uint8_t code = (static_cast<uint8_t>(smallValue) << 4) |
                   (static_cast<uint8_t>(st) & 0x0F);
    return code;
}

struct FnDecoded {
    Status status;
    bool   hasValue;
    double value;
};

FnDecoded decode_exit(uint8_t code) {
    FnDecoded r;
    uint8_t stBits    = code & 0x0F;
    uint8_t valueBits = (code >> 4) & 0x0F;

    r.value    = static_cast<double>(valueBits);
    r.hasValue = (stBits == ST_OK);

    switch (stBits) {
        case ST_OK:    r.status = ST_OK;    break;
        case ST_FAIL:  r.status = ST_FAIL;  break;
        case ST_UNDEF: r.status = ST_UNDEF; break;
        default:       r.status = ST_UNDEF; break;
    }
    return r;
}

// ----- Обчислення fn1(x) у дочірньому процесі -----
void run_fn1_child(int x) {
    if (x < 0) {
        uint8_t code = encode_exit(ST_FAIL, 0);
        ExitProcess(code);
    } else if (x > 10) {
        uint8_t code = encode_exit(ST_UNDEF, 0);
        ExitProcess(code);
    } else {
        int result = x + 1; // невеликий результат
        uint8_t code = encode_exit(ST_OK, result);
        ExitProcess(code);
    }
}

// ----- Обчислення fn2(x) у дочірньому процесі -----
void run_fn2_child(int x) {
    if (x < 0) {
        uint8_t code = encode_exit(ST_FAIL, 0);
        ExitProcess(code);
    } else if (x > 12) {
        uint8_t code = encode_exit(ST_UNDEF, 0);
        ExitProcess(code);
    } else {
        int result = 2 * x;
        uint8_t code = encode_exit(ST_OK, result);
        ExitProcess(code);
    }
}

std::string statusToString(Status s) {
    switch (s) {
        case ST_OK:    return "OK";
        case ST_FAIL:  return "FAIL";
        case ST_UNDEF: return "UNDEFINED";
        default:       return "UNKNOWN";
    }
}

int main(int argc, char* argv[]) {
    // --- Режим дочірнього процесу: Task fn1 x або Task fn2 x ---
    if (argc >= 3) {
        std::string mode = argv[1];
        int x = 0;
        try {
            x = std::stoi(argv[2]);
        } catch (...) {
            uint8_t code = encode_exit(ST_FAIL, 0);
            ExitProcess(code);
        }

        if (mode == "fn1") {
            run_fn1_child(x);
            return 0; // не дійде, бо ExitProcess
        } else if (mode == "fn2") {
            run_fn2_child(x);
            return 0;
        }
        // якщо невідомий режим – вважаємо FAIL
        uint8_t code = encode_exit(ST_FAIL, 0);
        ExitProcess(code);
    }

    // --- Режим головного процесу Mn ---
    std::cout << "Модель системи Mn з fn1 та fn2 (процеси + статус завершення, Windows)\n";
    std::cout << "Введіть ціле x (або q для виходу):\n";

    while (true) {
        std::cout << "\n> ";
        std::string input;
        if (!std::getline(std::cin, input)) break;
        if (input == "q" || input == "Q") break;

        int x;
        try {
            x = std::stoi(input);
        } catch (...) {
            std::cout << "Невірне значення, введіть ціле число або q.\n";
            continue;
        }

        // Отримаємо ім'я/шлях до поточного exe
        std::string exeName = argv[0];
        if (exeName.empty()) {
            exeName = "Task.exe"; // fallback
        }

        // Формуємо командні рядки для дочірніх процесів
        std::string cmd1 = "\"" + exeName + "\" fn1 " + std::to_string(x);
        std::string cmd2 = "\"" + exeName + "\" fn2 " + std::to_string(x);

        STARTUPINFOA si1{};
        PROCESS_INFORMATION pi1{};
        si1.cb = sizeof(si1);

        STARTUPINFOA si2{};
        PROCESS_INFORMATION pi2{};
        si2.cb = sizeof(si2);

        // CreateProcess вимагає змінюваний буфер для командного рядка
        char buf1[256];
        char buf2[256];
        strncpy(buf1, cmd1.c_str(), sizeof(buf1) - 1);
        buf1[sizeof(buf1) - 1] = '\0';
        strncpy(buf2, cmd2.c_str(), sizeof(buf2) - 1);
        buf2[sizeof(buf2) - 1] = '\0';

        BOOL ok1 = CreateProcessA(
            NULL,      // додаток — з командного рядка
            buf1,      // командний рядок
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &si1,
            &pi1
        );

        if (!ok1) {
            std::cerr << "Не вдалося створити процес fn1, код помилки: "
                      << GetLastError() << "\n";
            continue;
        }

        BOOL ok2 = CreateProcessA(
            NULL,
            buf2,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &si2,
            &pi2
        );

        if (!ok2) {
            std::cerr << "Не вдалося створити процес fn2, код помилки: "
                      << GetLastError() << "\n";
            // закриємо перший процес
            TerminateProcess(pi1.hProcess, encode_exit(ST_FAIL, 0));
            CloseHandle(pi1.hThread);
            CloseHandle(pi1.hProcess);
            continue;
        }

        bool done1 = false, done2 = false;
        DWORD rawStatus1 = 0, rawStatus2 = 0;

        // --- Опитування (polling) стану процесів ---
        while (!(done1 && done2)) {
            bool anyFinished = false;

            if (!done1) {
                DWORD waitRes = WaitForSingleObject(pi1.hProcess, 0); // не блокуємося
                if (waitRes == WAIT_OBJECT_0) {
                    anyFinished = true;
                    DWORD exitCode;
                    if (GetExitCodeProcess(pi1.hProcess, &exitCode)) {
                        rawStatus1 = exitCode;
                    } else {
                        rawStatus1 = encode_exit(ST_FAIL, 0);
                    }
                    done1 = true;
                }
            }

            if (!done2) {
                DWORD waitRes = WaitForSingleObject(pi2.hProcess, 0);
                if (waitRes == WAIT_OBJECT_0) {
                    anyFinished = true;
                    DWORD exitCode;
                    if (GetExitCodeProcess(pi2.hProcess, &exitCode)) {
                        rawStatus2 = exitCode;
                    } else {
                        rawStatus2 = encode_exit(ST_FAIL, 0);
                    }
                    done2 = true;
                }
            }

            if (!anyFinished) {
                Sleep(100); // трошки почекати, щоб не крутити цикл надто часто
            }
        }

        CloseHandle(pi1.hThread);
        CloseHandle(pi1.hProcess);
        CloseHandle(pi2.hThread);
        CloseHandle(pi2.hProcess);

        // --- Декодуємо результати ---
        FnDecoded r1 = decode_exit(static_cast<uint8_t>(rawStatus1 & 0xFF));
        FnDecoded r2 = decode_exit(static_cast<uint8_t>(rawStatus2 & 0xFF));

        int combinedStatusBits = static_cast<int>(r1.status) |
                                 static_cast<int>(r2.status);

        double sum = 0.0;
        if (r1.hasValue) sum += r1.value;
        if (r2.hasValue) sum += r2.value;

        std::string msg = "Результати для x = " + std::to_string(x) + ":\n";

        msg += "  fn1: статус = " + statusToString(r1.status);
        if (r1.hasValue) msg += ", значення = " + std::to_string(r1.value);
        msg += "\n";

        msg += "  fn2: статус = " + statusToString(r2.status);
        if (r2.hasValue) msg += ", значення = " + std::to_string(r2.value);
        msg += "\n";

        msg += "  Побітова диз'юнкція статусів = " +
               std::to_string(combinedStatusBits) + "\n";
        msg += "  Сума дійсних значень (fn1 + fn2) = " +
               std::to_string(sum) + "\n";

        std::cout << msg;
    }

    std::cout << "Завершення роботи.\n";
    return 0;
}

#ifdef _WIN32
// Оголошуємо main, щоб його можна було викликати з WinMain
int main(int argc, char* argv[]);

// Мінімальна обгортка для Windows, щоб задовольнити лінкер, який хоче WinMain
#include <windows.h>

extern "C" int __argc;
extern "C" char **__argv;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}
#endif
