#include "app/Editor.h"

#ifdef _WIN32
#include <Windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
#else
int main()
#endif
{
    weasel::Editor editor;
    editor.run();
    return 0;
}
