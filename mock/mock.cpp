#include "Extension.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

#include <cstdlib>
#include <cwchar>

#include <errhandlingapi.h>
#include <libloaderapi.h>
#include <winuser.h>

using namespace std::chrono_literals;

using std::cerr;
using std::endl;

namespace fs = std::filesystem;

typedef wchar_t *(*entrypoint)(wchar_t *, const InfoForExtension *);

int main(int argc, char *argv[])
{
        if (argc < 2) {
                cerr << "err: no dll given; usage: " << argv[0] << " <dll>" << endl;
                return 1;
        }

        auto const dll_path = fs::path(argv[1]).make_preferred();

        cerr << "info: loading " << dll_path << endl;
        HMODULE mod = LoadLibrary(dll_path.c_str());
        if (mod == NULL) {
                DWORD err = GetLastError();
                auto err_msg = std::system_category().message(err);

                cerr << "err: unable to load " << dll_path << ": [" << err << "] " << err_msg << endl;
                return 2;
        }
        cerr << "info: success" << endl;

        auto func = reinterpret_cast<entrypoint>(
                        reinterpret_cast<void *>(
                                GetProcAddress(mod, "OnNewSentence")
                        )
        );
        if (func == NULL) {
                cerr << "err: unable to find dll entrypoint" << endl;
                return 3;
        }

        InfoForExtension info{"current select", 1};
        wchar_t word[20];


        while (IDOK == MessageBox(NULL, L"Send message?", L"Mock", MB_OKCANCEL)) {
                std::wmemcpy(word, L"FOOBAR", 7); // swprintf non-conforming in mingw
                cerr << "info: sending msg to dll" << endl;
                func(word, &info);
        }

        return 0;
}
