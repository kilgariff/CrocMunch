#include "pch.h"
#include "munch_thread.hpp"
#include "winioctl.h" // For IOCTL_STORAGE_READ_CAPACITY 

#include <vector>

void MunchThread::prime(std::wstring const physical_drive_path, int disk_number)
{
    is_running = false;
    bytes_munched = 0;
    stage = 0;
    this->physical_drive_path = physical_drive_path;
    this->disk_number = disk_number;
}

void MunchThread::start()
{
    is_running = true;

    thread = std::thread([&]() {

        // Open the physical drive for R/W with buffering disabled and writing through FS caches.
        drive_handle = CreateFile(physical_drive_path.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING,
                                  NULL);

        if (INVALID_HANDLE_VALUE == drive_handle)
        {
            // TODO: handle error
            MessageBox(nullptr, GetLastErrorAsString().c_str(), L"Error", MB_OK | MB_ICONERROR);
            is_running = false;
            return;
        }

        // Lock and dismount all the volumes on the physical drive.
        std::vector<HANDLE> locked_volumes;
        {
            std::vector<std::wstring> volume_names;
            TCHAR volume_path[MAX_PATH * 4];
            HANDLE volume_find_handle = FindFirstVolume(volume_path, ARRAYSIZE(volume_path));

            if (volume_find_handle == INVALID_HANDLE_VALUE)
            {
                // TODO: handle error
                MessageBox(nullptr, GetLastErrorAsString().c_str(), L"Error", MB_OK | MB_ICONERROR);
                is_running = false;
                return;
            }
            else
            {
                volume_names.emplace_back(std::wstring(volume_path));

                while (true)
                {
                    if (FindNextVolume(volume_find_handle, volume_path, ARRAYSIZE(volume_path)) == FALSE)
                    {
                        if (GetLastError() == ERROR_NO_MORE_FILES)
                        {
                            break;
                        }
                        else
                        {
                            // TODO: handle error
                            MessageBox(nullptr, GetLastErrorAsString().c_str(), L"Error", MB_OK | MB_ICONERROR);
                            is_running = false;
                        }
                    }
                    else
                    {
                        volume_names.emplace_back(std::wstring(volume_path));
                    }
                }
            }

            // Figure out which volumes actually belong to this physical drive...

            DWORD status;
            VOLUME_DISK_EXTENTS disk_extents;

            for (auto const & volume_name : volume_names)
            {
                std::wstring without_bs = volume_name.substr(0, volume_name.find_last_of(L'\\'));
                HANDLE volume_handle = CreateFile(without_bs.c_str(),
                                                  GENERIC_READ,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                  NULL,
                                                  OPEN_EXISTING,
                                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                                                  NULL);

                if (INVALID_HANDLE_VALUE == volume_handle)
                {
                    // TODO: handle error
                    MessageBox(nullptr, GetLastErrorAsString().c_str(), L"Error", MB_OK | MB_ICONERROR);
                    is_running = false;
                    CloseHandle(volume_handle);
                    return;
                }

                // NOTE(ross): This won't work on systems with RAID arrays where the volume exists on multiple
                // physical extents. This is because I'm under time pressure and can't confidently say that I
                // can handle the way this API handles multiple extents.
                // See https://devblogs.microsoft.com/oldnewthing/20040826-00/?p=38043
                if (!DeviceIoControl(volume_handle,
                                     IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                     NULL,
                                     0,
                                     &disk_extents,
                                     sizeof(disk_extents),
                                     &status,
                                     NULL))
                {
                    printf("Error %d attempting to lock device\n", GetLastError());
                    is_running = false;
                    CloseHandle(volume_handle);
                    return;
                }

                if (disk_extents.Extents[0].DiskNumber == disk_number)
                {
                    // And lock + dismount them.
                    if (!DeviceIoControl(volume_handle, FSCTL_LOCK_VOLUME,
                        NULL, 0, NULL, 0, &status, NULL))
                    {
                        printf("Error %d attempting to lock device\n", GetLastError());
                        is_running = false;
                        return;
                    }

                    if (!DeviceIoControl(volume_handle, FSCTL_DISMOUNT_VOLUME,
                        NULL, 0, NULL, 0, &status, NULL))
                    {
                        DWORD err = GetLastError();
                        printf("Error %d attempting to dismount volume, error code\n", err);
                        is_running = false;
                        return;
                    }

                    locked_volumes.emplace_back(volume_handle);
                }
                else
                {
                    CloseHandle(volume_handle);
                }
            }
        }

        STORAGE_READ_CAPACITY read_capacity;
        DWORD bytes_returned;

        bool ioctl_success = DeviceIoControl(drive_handle,
                                             IOCTL_STORAGE_READ_CAPACITY,
                                             NULL,
                                             0,
                                             &read_capacity,
                                             sizeof(read_capacity),
                                             &bytes_returned,
                                             NULL);

        if (ioctl_success == FALSE)
        {
            // TODO: handle error
            MessageBox(nullptr, GetLastErrorAsString().c_str(), L"Error", MB_OK | MB_ICONERROR);
            is_running = false;
            return;
        }

        DISK_GEOMETRY_EX geo = {0};
        ioctl_success = DeviceIoControl(drive_handle,
                                        IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                        NULL,
                                        0,
                                        &geo,
                                        sizeof(geo),
                                        &bytes_returned,
                                        NULL);

        if (ioctl_success == FALSE)
        {
            // TODO: handle error
            MessageBox(nullptr, GetLastErrorAsString().c_str(), L"Error", MB_OK | MB_ICONERROR);
            is_running = false;
            return;
        }

        total_bytes_to_munch = uint64_t(read_capacity.DiskLength.QuadPart);

        size_t const chunk_size = geo.Geometry.BytesPerSector;

        LPVOID scratch_buffer = VirtualAlloc(nullptr, chunk_size, MEM_RESERVE, PAGE_READWRITE);
        scratch_buffer = VirtualAlloc(scratch_buffer, chunk_size, MEM_COMMIT, PAGE_READWRITE);
        uint8_t * pattern_data = static_cast<uint8_t *>(scratch_buffer);

        // Stage 1)
        stage = 1;
        memset(pattern_data, 'a', chunk_size);
        munch(pattern_data, chunk_size);
        if (is_running == false) { return; }

        // Stage 2)
        stage = 2;
        memset(pattern_data, ~'a', chunk_size);
        munch(pattern_data, chunk_size);
        if (is_running == false) { return; }

        // Stage 3)
        stage = 3;
        uint8_t const random_value = char(rand() % 256);
        srand((unsigned int) time(nullptr));
        memset(pattern_data, random_value, chunk_size);
        munch(pattern_data, chunk_size);
        if (is_running == false) { return; }
        
        // Stage 4)
        stage = 4;
        verify(pattern_data, chunk_size, random_value);
        if (is_running == false) { return; }

        VirtualFree(scratch_buffer, 0, MEM_RELEASE);

        for (auto const & volume_handle : locked_volumes)
        {
            CloseHandle(volume_handle);
        }

        CloseHandle(drive_handle);
        drive_handle = INVALID_HANDLE_VALUE;

        is_running = false;
    });
}

void MunchThread::force_stop()
{
    is_running = false;

    if (thread.joinable())
    {
        thread.join();
    }
}

void MunchThread::munch(uint8_t * pattern_data, size_t chunk_size)
{
    bytes_munched = 0;

    while (bytes_munched < total_bytes_to_munch && is_running)
    {
        DWORD to_munch = DWORD(min(chunk_size, total_bytes_to_munch - bytes_munched));

        DWORD written;
        BOOL result = WriteFile(drive_handle, pattern_data, to_munch, &written, nullptr);

        if (written != to_munch || result == FALSE)
        {
            // TODO: handle error
            MessageBox(nullptr, GetLastErrorAsString().c_str(), L"Error", MB_OK | MB_ICONERROR);
            is_running = false;
            return;
        }

        bytes_munched += to_munch;
    }

    SetFilePointer(drive_handle, 0, nullptr, FILE_BEGIN);
}

void MunchThread::verify(uint8_t * pattern_data, size_t chunk_size, uint8_t expected_value)
{
    bytes_munched = 0;

    while (bytes_munched < total_bytes_to_munch && is_running)
    {
        size_t to_verify = min(chunk_size, total_bytes_to_munch - bytes_munched);

        DWORD bytes_read = 0;
        BOOL result = ReadFile(drive_handle, pattern_data, to_verify, &bytes_read, nullptr);

        if (bytes_read != to_verify || result == FALSE)
        {
            // TODO: handle error
            MessageBox(nullptr, GetLastErrorAsString().c_str(), L"Error", MB_OK | MB_ICONERROR);
            is_running = false;
            return;
        }

        for (size_t idx = 0; idx < bytes_read; ++idx)
        {
            if (pattern_data[idx] != expected_value)
            {
                // TODO: handle error
                MessageBox(nullptr, L"Verify failed! Bytes were not munched thoroughly enough. Please restart CrocMunch and try again.", L"Error", MB_OK | MB_ICONERROR);
                is_running = false;
                return;
            }
        }

        bytes_munched += bytes_read;
    }

    SetFilePointer(drive_handle, 0, nullptr, FILE_BEGIN);
}