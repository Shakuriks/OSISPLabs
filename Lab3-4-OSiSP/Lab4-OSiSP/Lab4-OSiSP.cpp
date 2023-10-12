#include <iostream>
#include <windows.h>
#include <thread>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <filesystem>
#include <tchar.h>
#include <fstream>
#include <vector>

#define ID_COPY_BUTTON 1003


struct CopyInfo {
    std::wstring sourcePath;
    std::wstring destPath;
    HWND hCopyWnd;
    HWND hProgressBar;
    HWND hPauseButton;
    HWND hCancelButton;
    HWND hStatus;
    bool isPaused;
    bool isCancelled;
    int progress;
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void CopyFileToDestinationAsync(CopyInfo* copyInfo);
void CreateCopyWindow(CopyInfo* copyInfo);
void TogglePause(CopyInfo* copyInfo);
void CancelCopy(CopyInfo* copyInfo);
void UpdateStatusAndProgress(CopyInfo* copyInfo, const std::wstring& statusText, int progress);
LPVOID MapFileToMemory(const std::wstring& filePath, DWORDLONG& fileSize);
bool ChooseSourceFile(CopyInfo* copyInfo);
bool ChooseDestinationFolder(CopyInfo* copyInfo);

HWND hWnd;
HWND hCopyButton;

int currentButtonID = ID_COPY_BUTTON + 1;
static int nextWindowX = 500; // Начальное положение X
static int nextWindowY = 300; // Начальное положение Y



std::vector<HWND> hPauseButtons;
std::vector<HWND> hCancelButtons;
std::vector<CopyInfo*> copyInfos;
std::vector<std::thread> copyThreads; // Вектор для хранения потоков

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WindowProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, _T("AsyncCopyWindow"), NULL };
    RegisterClassEx(&wc);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    hWnd = CreateWindow(wc.lpszClassName, _T("Асинхронное копирование файлов"), WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, 100, 100, 150, 150, GetDesktopWindow(), NULL, wc.hInstance, NULL);

    hCopyButton = CreateWindow(_T("BUTTON"), _T("Копировать"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 30, 40, 80, 30, hWnd, (HMENU)ID_COPY_BUTTON, hInstance, NULL);

    ShowWindow(hWnd, SW_SHOWDEFAULT);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Дождитесь завершения всех потоков перед завершением программы
    for (std::thread& thread : copyThreads) {
        thread.join();
    }

    return 0;
}

// Функция для создания кнопки с динамическим ID
HWND CreateDynamicButton(const TCHAR* buttonText, int x, HWND parentWindow) {

    HWND button = CreateWindow(_T("BUTTON"), buttonText, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, 20, 90, 20, parentWindow, (HMENU)currentButtonID, GetModuleHandle(NULL), NULL);
    // Увеличиваем текущий ID для следующей кнопки
    currentButtonID++;
    return button;
}

HWND CreateProgressBar(HWND parentWindow) {
    HWND progressBar = CreateWindow(PROGRESS_CLASS, NULL, WS_CHILD | WS_VISIBLE, 10, 20, 255, 20, parentWindow, NULL, GetModuleHandle(NULL), NULL);
    SendMessage(progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100)); // Устанавливаем диапазон прогресса (0-100)
    SendMessage(progressBar, PBM_SETPOS, 0, 0); // Устанавливаем начальное значение прогресса
    return progressBar;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    CopyInfo* copyInfo = nullptr;

    // Ищем CopyInfo с соответствующим hCopyWnd
    for (CopyInfo* info : copyInfos) {
        if (info->hCopyWnd == hwnd) {
            copyInfo = info;
            break;
        }
    }
    switch (uMsg) {
    case WM_CREATE:
        return 0;

    break;

    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            if (LOWORD(wParam) == ID_COPY_BUTTON) {
                // Создаем новую копию структуры CopyInfo для каждой операции копирования
                CopyInfo* copyInfo = new CopyInfo;
                copyInfo->isPaused = false;
                copyInfo->isCancelled = false;
                copyInfo->progress = 0;

                // Ваши диалоги выбора исходного файла и папки назначения
                if (ChooseSourceFile(copyInfo) && ChooseDestinationFolder(copyInfo)) {
                    // Создаем полный путь к новому файлу в выбранной папке
                    TCHAR szFilePath[MAX_PATH];
                    std::wstring sourceFileName = PathFindFileName(copyInfo->sourcePath.c_str());
                    PathCombine(szFilePath, copyInfo->destPath.c_str(), sourceFileName.c_str());

                    // Проверяем, существует ли файл уже в папке назначения
                    if (PathFileExists(szFilePath)) {
                        MessageBox(NULL, L"Файл с таким именем уже существует в папке назначения.", L"Ошибка", MB_ICONERROR);
                        delete copyInfo; // Освобождаем память, если операция не началась
                    }
                    else {
                        CreateCopyWindow(copyInfo);
                        // Создаем поток для копирования файла
                        std::thread copyThread(CopyFileToDestinationAsync, copyInfo);
                        copyThreads.push_back(std::move(copyThread));
                    }
                }
                else {
                    delete copyInfo; // Освобождаем память, если операция не началась
                }
            }
            else {
                
                // Проверьте, находится ли кнопка в массиве hPauseButtons
                for (HWND hPauseButton : hPauseButtons) {
                    if (hPauseButton == (HWND)lParam) {
                        // Обработка события для кнопки паузы
                        int index = std::distance(hPauseButtons.begin(), std::find(hPauseButtons.begin(), hPauseButtons.end(), hPauseButton));
                        CopyInfo* copyInfo = copyInfos[index]; // Получите соответствующую структуру CopyInfo
                        TogglePause(copyInfo);
                        break;
                    }
                }

                // Проверьте, находится ли кнопка в массиве hCancelButtons
                for (HWND hCancelButton : hCancelButtons) {
                    if (hCancelButton == (HWND)lParam) {
                        // Обработка события для кнопки отмены
                        int index = std::distance(hCancelButtons.begin(), std::find(hCancelButtons.begin(), hCancelButtons.end(), hCancelButton));
                        CopyInfo* copyInfo = copyInfos[index]; // Получите соответствующую структуру CopyInfo
                        CancelCopy(copyInfo);
                        break;
                    }
                }
            }
            return 0;
        }
    case WM_CLOSE:
        if (hwnd == hWnd) {
            DestroyWindow(hWnd);
        }
        else {
            if (copyInfo->isCancelled || copyInfo->progress == 100) {
                // Если операция копирования завершена или отменена, закрываем окно
                DestroyWindow(hwnd);
            }
            else {
                // В противном случае, запускаем диалоговое окно 
                MessageBox(hwnd, L"Операция копирования не завершена. Отмените ее и только потом попробуйте закрыть окно.", L"Уведомление", MB_ICONWARNING);
            }
        }
        return 0;
    case WM_DESTROY:
        if (hwnd == hWnd) {
            PostQuitMessage(0);
        }
        else {
            // Найдите CopyInfo, связанную с этим окном
            for (size_t i = 0; i < copyInfos.size(); i++) {
                if (copyInfos[i]->hCopyWnd == hwnd) {
                    // Удаляем окно и освобождаем ресурсы
                    copyInfo = copyInfos[i];
                    copyInfos.erase(copyInfos.begin() + i);
                    hPauseButtons.erase(hPauseButtons.begin() + i);
                    hCancelButtons.erase(hCancelButtons.begin() + i);
                    copyThreads[i].join();
                    copyThreads.erase(copyThreads.begin() + i);
                    delete copyInfo;
                    break;
                }
            }
        }
        return 0;
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

void CreateCopyWindow(CopyInfo* copyInfo) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WindowProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, _T("AsyncCopyWindow"), NULL };
    RegisterClassEx(&wc);

    int windowWidth = 500;
    int windowHeight = 120;

    // Извлекаем название и расширение файла из sourcePath
    std::wstring sourceFileName = PathFindFileName(copyInfo->sourcePath.c_str());
    std::wstring destFolderName = PathFindFileName(copyInfo->destPath.c_str());
    std::wstring title = _T("Копирование ") + sourceFileName + _T(" в ") + destFolderName;

    copyInfo->hCopyWnd = CreateWindow(wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, nextWindowX, nextWindowY, windowWidth, windowHeight, NULL, NULL, wc.hInstance, NULL);
    copyInfo->hProgressBar = CreateProgressBar(copyInfo->hCopyWnd);
    copyInfo->hPauseButton = CreateDynamicButton(_T("Пауза"),  275, copyInfo->hCopyWnd);
    copyInfo->hCancelButton = CreateDynamicButton(_T("Отмена"), 380, copyInfo->hCopyWnd);
    copyInfo->hStatus = CreateWindow(STATUSCLASSNAME, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, copyInfo->hCopyWnd, NULL, wc.hInstance, NULL);

    nextWindowX += 20; // Увеличьте координаты X для следующего окна
    nextWindowY += 20; // Увеличьте координаты Y для следующего окна

    hPauseButtons.push_back(copyInfo->hPauseButton);
    hCancelButtons.push_back(copyInfo->hCancelButton);
    copyInfos.push_back(copyInfo);

    ShowWindow(copyInfo->hCopyWnd, SW_SHOW);
    UpdateWindow(copyInfo->hCopyWnd);
}

void CopyFileToDestinationAsync(CopyInfo * copyInfo) {
    EnableWindow(copyInfo->hPauseButton, TRUE);
    EnableWindow(copyInfo->hCancelButton, TRUE);

    // Определение размера файла для вычисления прогресса
    DWORDLONG  fileSize;
    LPVOID mappedData = MapFileToMemory(copyInfo->sourcePath, fileSize);
    if (mappedData == nullptr) {
        UpdateStatusAndProgress(copyInfo, L"Не удалось открыть исходный файл.", 0);
        return;
    }

    // Извлекаем название и расширение файла из sourcePath
    std::wstring sourceFileName = PathFindFileName(copyInfo->sourcePath.c_str());

    // Создаем полный путь к новому файлу в выбранной папке
    TCHAR szFilePath[MAX_PATH];
    PathCombine(szFilePath, copyInfo->destPath.c_str(), sourceFileName.c_str());

    // Создаем поток для записи данных в файл
    std::ofstream destStream(szFilePath, std::ios::binary);

    const int bufferSize = 1024; // Размер буфера для копирования
    char buffer[bufferSize];

    // Переменная для отслеживания скопированных байтов
    DWORDLONG  copiedBytes = 0;

    while (!copyInfo->isCancelled) {
        if (!copyInfo->isPaused) {
            DWORDLONG  bytesToCopy = static_cast<DWORDLONG>((bufferSize < (fileSize - copiedBytes)) ? bufferSize : (fileSize - copiedBytes));
            if (bytesToCopy > 0) {
                memcpy(buffer, static_cast<char*>(mappedData) + copiedBytes, bytesToCopy);
                destStream.write(buffer, bytesToCopy);
                copiedBytes += bytesToCopy;
                copyInfo->progress = static_cast<int>((copiedBytes * 100) / fileSize);
                UpdateStatusAndProgress(copyInfo, L"Копирование",  copyInfo->progress);

                if (copiedBytes == fileSize) {
                    copyInfo->progress = 100;
                    UpdateStatusAndProgress(copyInfo, L"Копирование завершено успешно.", 100);
                    break;
                }
            }
            else {
                UpdateStatusAndProgress(copyInfo, L"Ошибка при копировании файла.", 0);
                break;
            }
        }
        else {
            SetWindowText(copyInfo->hStatus, L"Копирование приостановлено.");
        }
    }

    UnmapViewOfFile(mappedData); // Освободите отображение после использования
    destStream.close();

    EnableWindow(copyInfo->hPauseButton, FALSE);
    EnableWindow(copyInfo->hCancelButton, FALSE);
}

    void TogglePause(CopyInfo* copyInfo) {
    copyInfo->isPaused = !copyInfo->isPaused;
    HWND hPauseButton = copyInfo->hPauseButton;

    if (copyInfo->isPaused) {
        SendMessage(hPauseButton, WM_SETTEXT, 0, (LPARAM)L"Возобновить");
    }
    else {
        SendMessage(hPauseButton, WM_SETTEXT, 0, (LPARAM)L"Пауза");
    }
}

// Функция для отмены копирования
void CancelCopy(CopyInfo* copyInfo) {
    copyInfo->isCancelled = true;
    UpdateStatusAndProgress(copyInfo, L"Копирование отменено", 0);
}

// Функция для обновления статуса и индикации хода выполнения
void UpdateStatusAndProgress(CopyInfo* copyInfo, const std::wstring& statusText, int progress) {
    SetWindowText(copyInfo->hStatus, statusText.c_str());
    SendMessage(copyInfo->hProgressBar, PBM_SETPOS, progress, 0);
}

// Функция для отображения файла в память
LPVOID MapFileToMemory(const std::wstring& filePath, DWORDLONG& fileSize) {
    HANDLE hFile = CreateFile(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    DWORD highSize;
    fileSize = GetFileSize(hFile, &highSize);

    HANDLE hMapping = CreateFileMapping(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (hMapping == nullptr) {
        CloseHandle(hFile);
        return nullptr;
    }

    LPVOID mappedData = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (mappedData == nullptr) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return nullptr;
    }

    CloseHandle(hMapping);
    CloseHandle(hFile);

    return mappedData;
}

// Функция для выбора исходного файла
bool ChooseSourceFile(CopyInfo* copyInfo) {
    OPENFILENAME ofn;
    TCHAR szFile[MAX_PATH] = { 0 };

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = szFile;
    ofn.lpstrFile[0] = '\0';
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = _T("All Files\0*.*\0");
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileName(&ofn)) {
        copyInfo->sourcePath = ofn.lpstrFile;
        return true;
    }

    return false;
}

// Функция для выбора папки назначения
bool ChooseDestinationFolder(CopyInfo* copyInfo) {
    BROWSEINFO bi = { 0 };
    TCHAR szFolder[MAX_PATH];

    bi.lpszTitle = _T("Выберите папку назначения");
    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);

    if (pidl != NULL) {
        SHGetPathFromIDList(pidl, szFolder);
        copyInfo->destPath = szFolder;
        return true;
    }

    return false;
}