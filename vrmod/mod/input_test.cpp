// hd2vr_inputtest — sends a relative mouse move from inside the game process when
// vrmod\inputtest_cmd.txt appears ("dx dy"), to test whether the game accepts in-process input.
#include <windows.h>
#include <cstdio>

static DWORD WINAPI Worker(LPVOID) {
    const wchar_t* path = L"C:\\HD2VR\\vrmod\\inputtest_cmd.txt";
    for (;;) {
        FILE* f = nullptr;
        if (!_wfopen_s(&f, path, L"r") && f) {
            int dx = 0, dy = 0;
            fscanf_s(f, "%d %d", &dx, &dy);
            fclose(f);
            DeleteFileW(path);
            for (int i = 0; i < 10; ++i) {
                INPUT in{};
                in.type = INPUT_MOUSE;
                in.mi.dx = dx / 10;
                in.mi.dy = dy / 10;
                in.mi.dwFlags = MOUSEEVENTF_MOVE;
                SendInput(1, &in, sizeof(in));
                Sleep(15);
            }
        }
        Sleep(100);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
    }
    return TRUE;
}
