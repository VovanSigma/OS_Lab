#include <windows.h>
#include <iostream>
#include <string>
#include <cstdint>

// Функція для нормального виводу 
void print_ukr(const std::string& utf8)
{
    int len16 = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    std::wstring wstr(len16, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], len16);

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteConsoleW(h, wstr.c_str(), len16 - 1, &written, NULL);
}

// Коди статусів
enum Status : uint8_t {
    ST_OK    = 1,
    ST_FAIL  = 2,
    ST_UNDEF = 4
};

// Кодуємо у 1 байт: 4 біти статусу + 4 біти маленького результату
uint8_t encode_exit(Status st, int smallValue) {
    if (smallValue < 0) smallValue = 0;
    if (smallValue > 15) smallValue = 15;
    return (static_cast<uint8_t>(smallValue) << 4) |
           (static_cast<uint8_t>(st) & 0x0F);
}

struct FnDecoded {
    Status status;
    bool   hasValue;
    double value;
};

// Декодуємо байт
FnDecoded decode_exit(uint8_t code) {
    FnDecoded r;
    uint8_t stBits    = code & 0x0F;
    uint8_t valueBits = (code >> 4) & 0x0F;

    r.value    = valueBits;
    r.hasValue = (stBits == ST_OK);

    switch (stBits) {
        case ST_OK:    r.status = ST_OK;    break;
        case ST_FAIL:  r.status = ST_FAIL;  break;
        case ST_UNDEF: r.status = ST_UNDEF; break;
        default:       r.status = ST_UNDEF; break;
    }
    return r;
}

std::string statusToString(Status s) {
    switch ( s ) {
        case ST_OK:    return "OK";
        case ST_FAIL:  return "FAIL";
        case ST_UNDEF: return "UNDEFINED";
        default:       return "UNKNOWN";
    }
}

// Дочірні процеси fn1 та fn2
void run_fn1_child(int x) {
    if (x < 0) ExitProcess(encode_exit(ST_FAIL, 0));
    if (x > 10) ExitProcess(encode_exit(ST_UNDEF, 0));
    int result = x + 1;
    ExitProcess(encode_exit(ST_OK, result));
}

void run_fn2_child(int x) {
    if (x < 0) ExitProcess(encode_exit(ST_FAIL, 0));
    if (x > 12) ExitProcess(encode_exit(ST_UNDEF, 0));
    int result = 2 * x;
    ExitProcess(encode_exit(ST_OK, result));
}

// Головний процес Mn
int main(int argc, char* argv[]) 
{
    // Додаємо нормальне кодування консолі
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Режим дочірнього процесу
    if (argc >= 3) {
        std::string mode = argv[1];
        int x = 0;
        try { x = std::stoi(argv[2]); }
        catch (...) { ExitProcess(encode_exit(ST_FAIL, 0)); }

        if (mode == "fn1") run_fn1_child(x);
        if (mode == "fn2") run_fn2_child(x);

        ExitProcess(encode_exit(ST_FAIL, 0));
    }

    // Головний режим
    print_ukr("Модель системи Mn з fn1 та fn2 (процеси + статус завершення, Windows)\n");
    print_ukr("Введіть ціле число x (або q для виходу):\n");

    while (true) {
        print_ukr("\n> ");
        std::string input;
        if (!std::getline(std::cin, input)) break;

        if (input == "q" || input == "Q") break;

        int x = 0;
        try { x = std::stoi(input); }
        catch (...) {
            print_ukr("Невірне значення, введіть число або q.\n");
            continue;
        }

        std::string exe = argv[0];
        std::string cmd1 = "\"" + exe + "\" fn1 " + std::to_string(x);
        std::string cmd2 = "\"" + exe + "\" fn2 " + std::to_string(x);

        STARTUPINFOA si1{}, si2{};
        PROCESS_INFORMATION pi1{}, pi2{};
        si1.cb = sizeof(si1);
        si2.cb = sizeof(si2);

        char buf1[256], buf2[256];
        strcpy(buf1, cmd1.c_str());
        strcpy(buf2, cmd2.c_str());

        BOOL ok1 = CreateProcessA(NULL, buf1, NULL, NULL, FALSE, 0, NULL, NULL, &si1, &pi1);
        if (!ok1) {
            print_ukr("Помилка створення процесу fn1.\n");
            continue;
        }

        BOOL ok2 = CreateProcessA(NULL, buf2, NULL, NULL, FALSE, 0, NULL, NULL, &si2, &pi2);
        if (!ok2) {
            print_ukr("Помилка створення процесу fn2.\n");
            TerminateProcess(pi1.hProcess, encode_exit(ST_FAIL, 0));
            CloseHandle(pi1.hThread);
            CloseHandle(pi1.hProcess);
            continue;
        }

        DWORD raw1 = 0, raw2 = 0;
        bool done1 = false, done2 = false;

        // ОПИТУВАННЯ процесів
        while (!(done1 && done2)) {
            if (!done1 && WaitForSingleObject(pi1.hProcess, 0) == WAIT_OBJECT_0) {
                GetExitCodeProcess(pi1.hProcess, &raw1);
                done1 = true;
            }

            if (!done2 && WaitForSingleObject(pi2.hProcess, 0) == WAIT_OBJECT_0) {
                GetExitCodeProcess(pi2.hProcess, &raw2);
                done2 = true;
            }

            if (!done1 || !done2) Sleep(50);
        }

        CloseHandle(pi1.hThread);
        CloseHandle(pi1.hProcess);
        CloseHandle(pi2.hThread);
        CloseHandle(pi2.hProcess);

        // ДЕКОДУВАННЯ
        FnDecoded r1 = decode_exit(raw1 & 0xFF);
        FnDecoded r2 = decode_exit(raw2 & 0xFF);

        int combined = r1.status | r2.status;
        double sum = (r1.hasValue ? r1.value : 0) + 
                     (r2.hasValue ? r2.value : 0);

        // Формуємо український вивід
        std::string msg;
        msg += "Результати для x = " + std::to_string(x) + ":\n";
        msg += "  fn1: статус = " + statusToString(r1.status);
        if (r1.hasValue) msg += ", значення = " + std::to_string(r1.value);
        msg += "\n";

        msg += "  fn2: статус = " + statusToString(r2.status);
        if (r2.hasValue) msg += ", значення = " + std::to_string(r2.value);
        msg += "\n";

        msg += "  Побітова диз'юнкція статусів = " + std::to_string(combined) + "\n";
        msg += "  Сума значень = " + std::to_string(sum) + "\n";

        print_ukr(msg);
    }

    print_ukr("Завершення роботи.\n");
    return 0;
}

