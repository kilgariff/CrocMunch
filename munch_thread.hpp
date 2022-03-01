#pragma once

#include <thread>
#include <mutex>
#include <atomic>

//Returns the last Win32 error, in string format. Returns an empty string if there is no error.
inline std::wstring GetLastErrorAsString()
{
    //Get the error message ID, if any.
    DWORD errorMessageID = ::GetLastError();
    if (errorMessageID == 0)
    {
        return std::wstring(); //No error message has been recorded
    }

    LPWSTR messageBuffer = nullptr;

    //Ask Win32 to give us the string version of that message ID.
    //The parameters we pass in, tell Win32 to create the buffer that holds the message for us (because we don't yet know how long the message string will be).
    size_t size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL,
                                 errorMessageID,
                                 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                 (LPWSTR)&messageBuffer,
                                 0,
                                 NULL);

    //Copy the error message into a std::string.
    std::wstring message(messageBuffer, size);

    //Free the Win32's string's buffer.
    LocalFree(messageBuffer);

    return message;
}

struct MunchThread
{
    void prime(std::wstring const physical_drive_path, int disk_number);
    void start();
    void munch(uint8_t * pattern_data, size_t chunk_size);
    void verify(uint8_t * pattern_data, size_t chunk_size, uint8_t expected_value);
    void force_stop();
    
    std::atomic<bool> is_running;
    std::atomic<uint64_t> bytes_munched;
    std::atomic<uint64_t> total_bytes_to_munch;
    std::atomic<uint64_t> stage;

    std::thread thread;

    std::wstring physical_drive_path;
    int disk_number;
    HANDLE drive_handle;
};