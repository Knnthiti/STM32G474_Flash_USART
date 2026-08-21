#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

enum {
    PC_CMD_START = 0x08,
    PC_CMD_SENDING = 0x68,
    PC_CMD_FINISHED = 0x9A,
    PC_CMD_WAIT = 0x21,

    STM_ACK_READY = 0x4A,
    STM_ACK_PAUSE = 0x6A,
    STM_ACK_FINISHED = 0x56,

    FIRMWARE_VERSION = 0x67,
    PAYLOAD_SIZE = 1016,
    PACKET_SIZE = 1024,
    ERASED_FLASH_BYTE = 0xFF,
    DEFAULT_BAUDRATE = 115200,

    ACK_POLL_INTERVAL_MS = 50,
    START_ACK_RETRIES = 6000,
    PACKET_ACK_RETRIES = 1000,
    FLASH_WRITE_ACK_RETRIES = 6000,
    FINISH_ACK_RETRIES = 6000
};

enum {
    ID_COMBO_PORT = 1001,
    ID_BUTTON_REFRESH,
    ID_EDIT_FILE,
    ID_BUTTON_BROWSE,
    ID_STATIC_STATUS,
    ID_PROGRESS,
    ID_STATIC_PROGRESS,
    ID_EDIT_LOG,
    ID_BUTTON_START,
    ID_BUTTON_RESET
};

#define WM_FLASH_LOG      (WM_APP + 1)
#define WM_FLASH_STATUS   (WM_APP + 2)
#define WM_FLASH_PROGRESS (WM_APP + 3)
#define WM_FLASH_DONE     (WM_APP + 4)

typedef struct {
    HWND hwnd;
    char port[64];
    char firmware_path[MAX_PATH];
} WorkerArgs;

static HWND g_hwnd_main;
static HWND g_hwnd_title_label;
static HWND g_hwnd_port_label;
static HWND g_hwnd_port_combo;
static HWND g_hwnd_refresh_button;
static HWND g_hwnd_file_label;
static HWND g_hwnd_file_edit;
static HWND g_hwnd_browse_button;
static HWND g_hwnd_status_title_label;
static HWND g_hwnd_status_label;
static HWND g_hwnd_progress;
static HWND g_hwnd_progress_label;
static HWND g_hwnd_log;
static HWND g_hwnd_start_button;
static HWND g_hwnd_reset_button;
static HANDLE g_worker_thread;
static volatile LONG g_cancel_requested;
static BOOL g_busy;

static uint32_t software_crc32_c_style(const uint8_t *data, size_t len);
static void build_packet(uint8_t command, const uint8_t *payload, size_t payload_len,
                         uint8_t pad_byte, uint8_t packet[PACKET_SIZE]);
static DWORD WINAPI firmware_thread_proc(LPVOID param);
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0U) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}

static bool is_cancelled(void)
{
    return InterlockedCompareExchange(&g_cancel_requested, 0, 0) != 0;
}

static bool file_exists(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0U);
}

static bool has_bin_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    return (dot != NULL) && (lstrcmpiA(dot, ".bin") == 0);
}

static void trim_crlf(char *text)
{
    size_t len = strlen(text);
    while ((len > 0U) && ((text[len - 1U] == '\r') || (text[len - 1U] == '\n'))) {
        text[len - 1U] = '\0';
        len--;
    }
}

static void format_last_error(char *buffer, size_t buffer_size, const char *prefix)
{
    DWORD code = GetLastError();
    char system_message[384] = {0};

    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        system_message,
        (DWORD)sizeof(system_message),
        NULL);

    trim_crlf(system_message);

    if (system_message[0] == '\0') {
        snprintf(buffer, buffer_size, "%s Error %lu.", prefix, (unsigned long)code);
    } else {
        snprintf(buffer, buffer_size, "%s %s", prefix, system_message);
    }
}

static void post_done(HWND hwnd, bool success, const char *fmt, ...)
{
    char *text = (char *)malloc(2048U);
    va_list args;

    if (text == NULL) {
        return;
    }

    va_start(args, fmt);
#if defined(_MSC_VER)
    vsnprintf_s(text, 2048U, _TRUNCATE, fmt, args);
#else
    vsnprintf(text, 2048U, fmt, args);
#endif
    va_end(args);
    text[2047] = '\0';

    if (!PostMessageA(hwnd, WM_FLASH_DONE, (WPARAM)(success ? TRUE : FALSE),
                      (LPARAM)text)) {
        free(text);
    }
}

static void post_log(HWND hwnd, const char *fmt, ...)
{
    char *text = (char *)malloc(2048U);
    va_list args;

    if (text == NULL) {
        return;
    }

    va_start(args, fmt);
#if defined(_MSC_VER)
    vsnprintf_s(text, 2048U, _TRUNCATE, fmt, args);
#else
    vsnprintf(text, 2048U, fmt, args);
#endif
    va_end(args);
    text[2047] = '\0';

    if (!PostMessageA(hwnd, WM_FLASH_LOG, 0, (LPARAM)text)) {
        free(text);
    }
}

static void post_status(HWND hwnd, const char *fmt, ...)
{
    char *text = (char *)malloc(512U);
    va_list args;

    if (text == NULL) {
        return;
    }

    va_start(args, fmt);
#if defined(_MSC_VER)
    vsnprintf_s(text, 512U, _TRUNCATE, fmt, args);
#else
    vsnprintf(text, 512U, fmt, args);
#endif
    va_end(args);
    text[511] = '\0';

    if (!PostMessageA(hwnd, WM_FLASH_STATUS, 0, (LPARAM)text)) {
        free(text);
    }
}

static void post_progress(HWND hwnd, size_t done, size_t total)
{
    int percent = 0;

    if (total > 0U) {
        percent = (int)((done * 100U) / total);
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    PostMessageA(hwnd, WM_FLASH_PROGRESS, (WPARAM)percent, 0);
}

static void append_log_text(const char *text)
{
    int len;

    if (g_hwnd_log == NULL) {
        return;
    }

    len = GetWindowTextLengthA(g_hwnd_log);
    SendMessageA(g_hwnd_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(g_hwnd_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageA(g_hwnd_log, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
}

static void set_busy(BOOL busy)
{
    g_busy = busy;

    EnableWindow(g_hwnd_start_button, !busy);
    EnableWindow(g_hwnd_refresh_button, !busy);
    EnableWindow(g_hwnd_browse_button, !busy);
    EnableWindow(g_hwnd_port_combo, !busy);
    EnableWindow(g_hwnd_file_edit, !busy);
    SetWindowTextA(g_hwnd_reset_button, busy ? "Cancel" : "Reset");
}

static void set_progress_percent(int percent)
{
    char label[16];

    SendMessageA(g_hwnd_progress, PBM_SETPOS, (WPARAM)percent, 0);
    snprintf(label, sizeof(label), "%d%%", percent);
    SetWindowTextA(g_hwnd_progress_label, label);
}

static void reset_log(void)
{
    SetWindowTextA(g_hwnd_log, "");
}

static void apply_default_font(HWND hwnd)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
}

static void add_port_if_present(int number)
{
    char port_name[16];
    char target[512];

    snprintf(port_name, sizeof(port_name), "COM%d", number);
    if (QueryDosDeviceA(port_name, target, (DWORD)sizeof(target)) != 0U) {
        SendMessageA(g_hwnd_port_combo, CB_ADDSTRING, 0, (LPARAM)port_name);
    }
}

static void refresh_ports(void)
{
    char current[64] = {0};
    int count;
    int selected = CB_ERR;

    GetWindowTextA(g_hwnd_port_combo, current, (int)sizeof(current));
    SendMessageA(g_hwnd_port_combo, CB_RESETCONTENT, 0, 0);

    for (int i = 1; i <= 256; i++) {
        add_port_if_present(i);
    }

    count = (int)SendMessageA(g_hwnd_port_combo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        char item[64] = {0};
        SendMessageA(g_hwnd_port_combo, CB_GETLBTEXT, (WPARAM)i, (LPARAM)item);
        if (lstrcmpiA(item, current) == 0) {
            selected = i;
            break;
        }
    }

    if ((selected == CB_ERR) && (count > 0)) {
        selected = 0;
    }

    if (selected != CB_ERR) {
        SendMessageA(g_hwnd_port_combo, CB_SETCURSEL, (WPARAM)selected, 0);
    } else {
        SetWindowTextA(g_hwnd_port_combo, "");
        append_log_text("No COM ports found. Click Refresh after connecting STM32.");
    }
}

static void remove_file_name(char *path)
{
    char *slash = strrchr(path, '\\');
    if (slash != NULL) {
        *slash = '\0';
    }
}

static void set_default_firmware_path(void)
{
    char module_path[MAX_PATH] = {0};
    char candidate[MAX_PATH] = {0};
    char parent[MAX_PATH] = {0};
    char cwd[MAX_PATH] = {0};

    GetModuleFileNameA(NULL, module_path, (DWORD)sizeof(module_path));
    remove_file_name(module_path);

    snprintf(candidate, sizeof(candidate), "%s\\App.bin", module_path);
    if (file_exists(candidate)) {
        SetWindowTextA(g_hwnd_file_edit, candidate);
        return;
    }

    safe_copy(parent, sizeof(parent), module_path);
    remove_file_name(parent);
    snprintf(candidate, sizeof(candidate), "%s\\Flash_program\\App.bin", parent);
    if (file_exists(candidate)) {
        SetWindowTextA(g_hwnd_file_edit, candidate);
        return;
    }

    GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd);
    snprintf(candidate, sizeof(candidate), "%s\\Flash_program\\App.bin", cwd);
    if (file_exists(candidate)) {
        SetWindowTextA(g_hwnd_file_edit, candidate);
    }
}

static void browse_firmware_file(void)
{
    OPENFILENAMEA ofn;
    char path[MAX_PATH] = {0};

    GetWindowTextA(g_hwnd_file_edit, path, (int)sizeof(path));

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd_main;
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)sizeof(path);
    ofn.lpstrFilter = "Binary firmware (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = "Select firmware .bin file";

    if (GetOpenFileNameA(&ofn)) {
        SetWindowTextA(g_hwnd_file_edit, path);
        append_log_text("Selected firmware file.");
        append_log_text(path);
    }
}

static void get_selected_port(char *port, size_t port_size)
{
    int index = (int)SendMessageA(g_hwnd_port_combo, CB_GETCURSEL, 0, 0);

    port[0] = '\0';
    if (index != CB_ERR) {
        SendMessageA(g_hwnd_port_combo, CB_GETLBTEXT, (WPARAM)index, (LPARAM)port);
    } else {
        GetWindowTextA(g_hwnd_port_combo, port, (int)port_size);
    }

    for (size_t i = 0U; port[i] != '\0'; i++) {
        if ((port[i] == ' ') || (port[i] == '-')) {
            port[i] = '\0';
            break;
        }
    }
}

static uint32_t software_crc32_c_style(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < len; i += 4U) {
        uint32_t word = 0U;

        for (size_t b = 0U; (b < 4U) && ((i + b) < len); b++) {
            word |= ((uint32_t)data[i + b]) << (8U * b);
        }

        crc ^= word;
        for (uint8_t j = 0U; j < 32U; j++) {
            if ((crc & 0x80000000U) != 0U) {
                crc = (crc << 1U) ^ 0x04C11DB7U;
            } else {
                crc <<= 1U;
            }
        }
    }

    return crc;
}

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void build_packet(uint8_t command, const uint8_t *payload, size_t payload_len,
                         uint8_t pad_byte, uint8_t packet[PACKET_SIZE])
{
    uint32_t crc;

    if (payload_len > PAYLOAD_SIZE) {
        payload_len = PAYLOAD_SIZE;
    }

    memset(packet, 0, PACKET_SIZE);
    packet[0] = command;
    packet[1] = FIRMWARE_VERSION;
    put_u16_le(&packet[2], (uint16_t)PACKET_SIZE);

    memset(&packet[4], pad_byte, PAYLOAD_SIZE);
    if ((payload != NULL) && (payload_len > 0U)) {
        memcpy(&packet[4], payload, payload_len);
    }

    crc = software_crc32_c_style(packet, PACKET_SIZE - 4U);
    put_u32_le(&packet[PACKET_SIZE - 4U], crc);
}

static bool read_firmware_file(const char *path, uint8_t **data, size_t *size,
                               char *err, size_t err_size)
{
    HANDLE file;
    LARGE_INTEGER file_size;
    uint8_t *buffer;
    size_t total_read = 0U;

    *data = NULL;
    *size = 0U;

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        format_last_error(err, err_size, "Cannot open firmware file:");
        return false;
    }

    if (!GetFileSizeEx(file, &file_size)) {
        format_last_error(err, err_size, "Cannot read firmware file size:");
        CloseHandle(file);
        return false;
    }

    if (file_size.QuadPart <= 0) {
        snprintf(err, err_size, "Firmware file is empty.");
        CloseHandle(file);
        return false;
    }

    if (file_size.QuadPart > 0xFFFFFFFFLL) {
        snprintf(err, err_size, "Firmware file is larger than 4 GB.");
        CloseHandle(file);
        return false;
    }

    buffer = (uint8_t *)malloc((size_t)file_size.QuadPart);
    if (buffer == NULL) {
        snprintf(err, err_size, "Not enough memory to load firmware file.");
        CloseHandle(file);
        return false;
    }

    while (total_read < (size_t)file_size.QuadPart) {
        DWORD chunk = 0;
        DWORD to_read = (DWORD)((size_t)file_size.QuadPart - total_read);
        if (to_read > 65536U) {
            to_read = 65536U;
        }

        if (!ReadFile(file, buffer + total_read, to_read, &chunk, NULL)) {
            format_last_error(err, err_size, "Cannot read firmware file:");
            free(buffer);
            CloseHandle(file);
            return false;
        }

        if (chunk == 0U) {
            snprintf(err, err_size, "Unexpected end of firmware file.");
            free(buffer);
            CloseHandle(file);
            return false;
        }

        total_read += chunk;
    }

    CloseHandle(file);
    *data = buffer;
    *size = total_read;
    return true;
}

static HANDLE open_serial_port(const char *port, char *err, size_t err_size)
{
    char device_path[96];
    HANDLE serial;
    DCB dcb;
    COMMTIMEOUTS timeouts;

    if (strncmp(port, "\\\\.\\", 4U) == 0) {
        safe_copy(device_path, sizeof(device_path), port);
    } else {
        snprintf(device_path, sizeof(device_path), "\\\\.\\%s", port);
    }

    serial = CreateFileA(device_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (serial == INVALID_HANDLE_VALUE) {
        format_last_error(err, err_size, "Cannot open COM port:");
        return INVALID_HANDLE_VALUE;
    }

    SetupComm(serial, 4096, 4096);
    PurgeComm(serial, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(serial, &dcb)) {
        format_last_error(err, err_size, "Cannot read COM settings:");
        CloseHandle(serial);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = DEFAULT_BAUDRATE;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(serial, &dcb)) {
        format_last_error(err, err_size, "Cannot apply COM settings:");
        CloseHandle(serial);
        return INVALID_HANDLE_VALUE;
    }

    ZeroMemory(&timeouts, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = 20;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 5000;
    SetCommTimeouts(serial, &timeouts);

    EscapeCommFunction(serial, SETDTR);
    EscapeCommFunction(serial, SETRTS);
    return serial;
}

static bool send_packet(HANDLE serial, const uint8_t packet[PACKET_SIZE],
                        char *err, size_t err_size)
{
    DWORD total_written = 0U;

    while (total_written < PACKET_SIZE) {
        DWORD written = 0U;
        if (!WriteFile(serial, packet + total_written, PACKET_SIZE - total_written,
                       &written, NULL)) {
            format_last_error(err, err_size, "Serial write failed:");
            return false;
        }

        if (written == 0U) {
            snprintf(err, err_size, "Serial write timeout.");
            return false;
        }

        total_written += written;
    }

    FlushFileBuffers(serial);
    return true;
}

static bool is_known_stm_ack(uint8_t value)
{
    return (value == STM_ACK_READY) ||
           (value == STM_ACK_PAUSE) ||
           (value == STM_ACK_FINISHED);
}

static int find_valid_ack_frame_start(const uint8_t *buffer, size_t len)
{
    if (len < PACKET_SIZE) {
        return -1;
    }

    for (size_t start = 0U; start <= (len - PACKET_SIZE); start++) {
        if (!is_known_stm_ack(buffer[start])) {
            continue;
        }
        if ((buffer[start + 2U] == 0x00U) && (buffer[start + 3U] == 0x04U)) {
            return (int)start;
        }
    }

    return -1;
}

static bool ack_is_expected(uint8_t ack, const uint8_t *expected, size_t expected_count)
{
    for (size_t i = 0U; i < expected_count; i++) {
        if (ack == expected[i]) {
            return true;
        }
    }
    return false;
}

static int wait_for_stm32(HANDLE serial, const uint8_t *expected, size_t expected_count,
                          int timeout_retries, HWND hwnd, char *err, size_t err_size)
{
    uint8_t rx_buffer[PACKET_SIZE * 4U];
    size_t rx_len = 0U;

    for (int retry = 0; retry < timeout_retries; retry++) {
        DWORD errors = 0U;
        COMSTAT stat;

        if (is_cancelled()) {
            snprintf(err, err_size, "Operation reset by user.");
            return -1;
        }

        ZeroMemory(&stat, sizeof(stat));
        if (!ClearCommError(serial, &errors, &stat)) {
            format_last_error(err, err_size, "Serial read status failed:");
            return -2;
        }

        if (stat.cbInQue > 0U) {
            uint8_t temp[PACKET_SIZE];
            DWORD to_read = stat.cbInQue;
            DWORD read_count = 0U;

            if (to_read > (DWORD)sizeof(temp)) {
                to_read = (DWORD)sizeof(temp);
            }

            if (!ReadFile(serial, temp, to_read, &read_count, NULL)) {
                format_last_error(err, err_size, "Serial read failed:");
                return -2;
            }

            if (read_count > 0U) {
                if ((rx_len + read_count) > sizeof(rx_buffer)) {
                    size_t drop = (rx_len + read_count) - sizeof(rx_buffer);
                    if (drop >= rx_len) {
                        rx_len = 0U;
                    } else {
                        memmove(rx_buffer, rx_buffer + drop, rx_len - drop);
                        rx_len -= drop;
                    }
                    post_log(hwnd, "[!] Dropped %u byte(s) while receiving ACK.",
                             (unsigned int)drop);
                }

                memcpy(rx_buffer + rx_len, temp, read_count);
                rx_len += read_count;

                while (rx_len >= PACKET_SIZE) {
                    int start = find_valid_ack_frame_start(rx_buffer, rx_len);
                    uint8_t ack;

                    if (start < 0) {
                        if (rx_len > (PACKET_SIZE - 1U)) {
                            size_t drop = rx_len - (PACKET_SIZE - 1U);
                            memmove(rx_buffer, rx_buffer + drop, rx_len - drop);
                            rx_len -= drop;
                            post_log(hwnd,
                                     "[!] Dropped %u byte(s) while searching for ACK frame.",
                                     (unsigned int)drop);
                        }
                        break;
                    }

                    if (start > 0) {
                        memmove(rx_buffer, rx_buffer + start, rx_len - (size_t)start);
                        rx_len -= (size_t)start;
                        post_log(hwnd, "[!] Ignored %d byte(s) before ACK frame.", start);
                    }

                    ack = rx_buffer[0];
                    memmove(rx_buffer, rx_buffer + PACKET_SIZE, rx_len - PACKET_SIZE);
                    rx_len -= PACKET_SIZE;

                    if (ack_is_expected(ack, expected, expected_count)) {
                        return (int)ack;
                    }

                    post_log(hwnd, "[!] Unexpected ACK received: 0x%02X", ack);
                }
            }
        }

        Sleep(ACK_POLL_INTERVAL_MS);
    }

    snprintf(err, err_size, "Timeout while waiting for STM32 ACK.");
    return 0;
}

static bool wait_until_stm32_ready(HANDLE serial, int timeout_retries, HWND hwnd,
                                   const char *pause_status, const char *pause_log,
                                   const char *timeout_message,
                                   char *err, size_t err_size)
{
    const uint8_t ready_or_pause[] = {STM_ACK_READY, STM_ACK_PAUSE};
    const uint8_t ready_only[] = {STM_ACK_READY};
    int ack = wait_for_stm32(serial, ready_or_pause, ARRAY_COUNT(ready_or_pause),
                             timeout_retries, hwnd, err, err_size);

    if (ack == STM_ACK_PAUSE) {
        uint8_t packet[PACKET_SIZE];

        post_status(hwnd, "%s", pause_status);
        post_log(hwnd, "%s", pause_log);

        build_packet(PC_CMD_WAIT, NULL, 0U, 0x00U, packet);
        if (!send_packet(serial, packet, err, err_size)) {
            return false;
        }

        ack = wait_for_stm32(serial, ready_only, ARRAY_COUNT(ready_only),
                             FLASH_WRITE_ACK_RETRIES, hwnd, err, err_size);

        if (ack != STM_ACK_READY) {
            safe_copy(err, err_size, timeout_message);
            return false;
        }

        post_log(hwnd, "Received READY (0x%02X) after WAIT", STM_ACK_READY);
    }

    if (ack != STM_ACK_READY) {
        safe_copy(err, err_size, timeout_message);
        return false;
    }

    return true;
}

static DWORD WINAPI firmware_thread_proc(LPVOID param)
{
    WorkerArgs *args = (WorkerArgs *)param;
    HWND hwnd = args->hwnd;
    uint8_t *firmware = NULL;
    size_t firmware_size = 0U;
    size_t total_chunks = 0U;
    HANDLE serial = INVALID_HANDLE_VALUE;
    char err[512] = {0};
    bool success = false;
    char done_message[512] = {0};

    if (!read_firmware_file(args->firmware_path, &firmware, &firmware_size,
                            err, sizeof(err))) {
        goto cleanup;
    }

    total_chunks = (firmware_size + PAYLOAD_SIZE - 1U) / PAYLOAD_SIZE;
    post_log(hwnd, "Loaded firmware: %u bytes (%u packets)",
             (unsigned int)firmware_size, (unsigned int)total_chunks);

    if (is_cancelled()) {
        safe_copy(err, sizeof(err), "Operation reset by user.");
        goto cleanup;
    }

    serial = open_serial_port(args->port, err, sizeof(err));
    if (serial == INVALID_HANDLE_VALUE) {
        goto cleanup;
    }

    post_status(hwnd, "Connected to %s", args->port);
    post_log(hwnd, "Connected to %s at %u baud", args->port, DEFAULT_BAUDRATE);

    {
        uint8_t packet[PACKET_SIZE];
        uint8_t start_payload[4];
        uint32_t firmware_size32 = (uint32_t)firmware_size;

        put_u32_le(start_payload, firmware_size32);
        post_status(hwnd, "Sending start command");
        post_log(hwnd, "Sending START (0x%02X), firmware version 0x%02X",
                 PC_CMD_START, FIRMWARE_VERSION);
        build_packet(PC_CMD_START, start_payload, sizeof(start_payload), 0x00U, packet);

        if (!send_packet(serial, packet, err, sizeof(err))) {
            goto cleanup;
        }
    }

    for (size_t chunk_index = 0U; chunk_index < total_chunks; chunk_index++) {
        uint8_t packet[PACKET_SIZE];
        size_t start_index;
        size_t chunk_size;

        if (is_cancelled()) {
            safe_copy(err, sizeof(err), "Operation reset by user.");
            goto cleanup;
        }

        if (!wait_until_stm32_ready(
                serial,
                (chunk_index == 0U) ? START_ACK_RETRIES : PACKET_ACK_RETRIES,
                hwnd,
                "STM32 is writing flash",
                "STM32 buffer is full (0x6A), sending WAIT (0x21)",
                "Timeout while waiting for STM32 READY.",
                err,
                sizeof(err))) {
            snprintf(done_message, sizeof(done_message), "Timeout at packet %u/%u.",
                     (unsigned int)(chunk_index + 1U), (unsigned int)total_chunks);
            if (err[0] == '\0') {
                safe_copy(err, sizeof(err), done_message);
            }
            goto cleanup;
        }

        start_index = chunk_index * PAYLOAD_SIZE;
        chunk_size = firmware_size - start_index;
        if (chunk_size > PAYLOAD_SIZE) {
            chunk_size = PAYLOAD_SIZE;
        }

        build_packet(PC_CMD_SENDING, firmware + start_index, chunk_size,
                     ERASED_FLASH_BYTE, packet);

        if (!send_packet(serial, packet, err, sizeof(err))) {
            goto cleanup;
        }

        post_status(hwnd, "Sending packet %u/%u",
                    (unsigned int)(chunk_index + 1U), (unsigned int)total_chunks);
        post_log(hwnd, "[%u/%u] Sent %u bytes",
                 (unsigned int)(chunk_index + 1U), (unsigned int)total_chunks,
                 (unsigned int)chunk_size);
        post_progress(hwnd, chunk_index + 1U, total_chunks);
    }

    post_status(hwnd, "Waiting for final READY");
    post_log(hwnd, "No firmware data remains. Waiting for READY before FINISHED (0x%02X)",
             PC_CMD_FINISHED);

    if (!wait_until_stm32_ready(
            serial,
            PACKET_ACK_RETRIES,
            hwnd,
            "STM32 is writing final flash block",
            "STM32 buffer is full (0x6A), sending final WAIT (0x21)",
            "STM32 is not ready to accept FINISHED command.",
            err,
            sizeof(err))) {
        goto cleanup;
    }

    {
        const uint8_t finished_ack[] = {STM_ACK_FINISHED};
        uint8_t packet[PACKET_SIZE];
        int ack;

        post_log(hwnd, "Sending FINISHED (0x%02X)", PC_CMD_FINISHED);
        build_packet(PC_CMD_FINISHED, NULL, 0U, 0x00U, packet);
        if (!send_packet(serial, packet, err, sizeof(err))) {
            goto cleanup;
        }

        post_status(hwnd, "Waiting for final confirmation");
        post_log(hwnd, "Waiting for final success ACK (0x%02X)", STM_ACK_FINISHED);

        ack = wait_for_stm32(serial, finished_ack, ARRAY_COUNT(finished_ack),
                             FINISH_ACK_RETRIES, hwnd, err, sizeof(err));
        if (ack != STM_ACK_FINISHED) {
            safe_copy(err, sizeof(err), "Failed to receive final confirmation from STM32.");
            goto cleanup;
        }
    }

    post_progress(hwnd, total_chunks, total_chunks);
    post_status(hwnd, "Success");
    post_log(hwnd, "Success! Firmware successfully updated.");
    success = true;

cleanup:
    if (serial != INVALID_HANDLE_VALUE) {
        CloseHandle(serial);
        post_log(hwnd, "Serial port closed");
    }

    free(firmware);

    if (!success) {
        if (is_cancelled()) {
            safe_copy(done_message, sizeof(done_message), "Operation reset by user.");
        } else if (err[0] != '\0') {
            safe_copy(done_message, sizeof(done_message), err);
        } else {
            safe_copy(done_message, sizeof(done_message), "Firmware update failed.");
        }
    } else {
        safe_copy(done_message, sizeof(done_message), "Firmware update completed.");
    }

    post_done(hwnd, success, "%s", done_message);
    free(args);
    return 0;
}

static void start_flash(void)
{
    WorkerArgs *args;
    DWORD thread_id = 0U;
    char port[64] = {0};
    char firmware_path[MAX_PATH] = {0};

    if (g_busy) {
        return;
    }

    get_selected_port(port, sizeof(port));
    GetWindowTextA(g_hwnd_file_edit, firmware_path, (int)sizeof(firmware_path));

    if (port[0] == '\0') {
        MessageBoxA(g_hwnd_main, "Please select a COM port.", "Missing COM Port",
                    MB_ICONERROR | MB_OK);
        return;
    }

    if (firmware_path[0] == '\0') {
        MessageBoxA(g_hwnd_main, "Please select a .bin firmware file.",
                    "Missing Firmware", MB_ICONERROR | MB_OK);
        return;
    }

    if (!has_bin_extension(firmware_path)) {
        MessageBoxA(g_hwnd_main, "Please select a .bin firmware file.",
                    "Invalid Firmware", MB_ICONERROR | MB_OK);
        return;
    }

    if (!file_exists(firmware_path)) {
        MessageBoxA(g_hwnd_main, "Firmware file not found.", "File Not Found",
                    MB_ICONERROR | MB_OK);
        return;
    }

    args = (WorkerArgs *)calloc(1U, sizeof(*args));
    if (args == NULL) {
        MessageBoxA(g_hwnd_main, "Not enough memory to start worker thread.",
                    "Firmware Update Error", MB_ICONERROR | MB_OK);
        return;
    }

    args->hwnd = g_hwnd_main;
    safe_copy(args->port, sizeof(args->port), port);
    safe_copy(args->firmware_path, sizeof(args->firmware_path), firmware_path);

    InterlockedExchange(&g_cancel_requested, 0);
    set_busy(TRUE);
    set_progress_percent(0);
    SetWindowTextA(g_hwnd_status_label, "Starting");
    append_log_text("");
    append_log_text("--- Starting firmware update ---");

    g_worker_thread = CreateThread(NULL, 0, firmware_thread_proc, args, 0, &thread_id);
    if (g_worker_thread == NULL) {
        free(args);
        set_busy(FALSE);
        MessageBoxA(g_hwnd_main, "Cannot create worker thread.",
                    "Firmware Update Error", MB_ICONERROR | MB_OK);
    }
}

static void reset_or_cancel(void)
{
    if (g_busy) {
        InterlockedExchange(&g_cancel_requested, 1);
        SetWindowTextA(g_hwnd_status_label, "Reset requested");
        append_log_text("Reset requested. Stopping current operation...");
        return;
    }

    SetWindowTextA(g_hwnd_status_label, "Ready");
    set_progress_percent(0);
    reset_log();
    set_default_firmware_path();
    refresh_ports();
}

static HWND create_child(const char *class_name, const char *text, DWORD style,
                         DWORD ex_style, int id)
{
    HWND child = CreateWindowExA(
        ex_style, class_name, text, style | WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, g_hwnd_main, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);

    apply_default_font(child);
    return child;
}

static void create_controls(HWND hwnd)
{
    g_hwnd_main = hwnd;

    g_hwnd_title_label = create_child("STATIC", "STM32G474 UART Firmware Updater",
                                      SS_LEFT, 0, -1);

    g_hwnd_port_label = create_child("STATIC", "COM Port", SS_LEFT, 0, -1);
    g_hwnd_port_combo = create_child("COMBOBOX", "",
                                     CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
                                     0, ID_COMBO_PORT);
    g_hwnd_refresh_button = create_child("BUTTON", "Refresh",
                                         BS_PUSHBUTTON, 0, ID_BUTTON_REFRESH);

    g_hwnd_file_label = create_child("STATIC", "Firmware .bin", SS_LEFT, 0, -1);
    g_hwnd_file_edit = create_child("EDIT", "",
                                    ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_EDIT_FILE);
    g_hwnd_browse_button = create_child("BUTTON", "Browse",
                                        BS_PUSHBUTTON, 0, ID_BUTTON_BROWSE);

    g_hwnd_status_title_label = create_child("STATIC", "Status", SS_LEFT, 0, -1);
    g_hwnd_status_label = create_child("STATIC", "Ready", SS_LEFT, 0, ID_STATIC_STATUS);

    g_hwnd_progress = create_child(PROGRESS_CLASSA, "",
                                   PBS_SMOOTH, 0, ID_PROGRESS);
    SendMessageA(g_hwnd_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    g_hwnd_progress_label = create_child("STATIC", "0%", SS_RIGHT, 0, ID_STATIC_PROGRESS);

    g_hwnd_log = create_child("EDIT", "",
                              ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                              WS_EX_CLIENTEDGE, ID_EDIT_LOG);

    g_hwnd_start_button = create_child("BUTTON", "Start Flash",
                                       BS_DEFPUSHBUTTON, 0, ID_BUTTON_START);
    g_hwnd_reset_button = create_child("BUTTON", "Reset",
                                       BS_PUSHBUTTON, 0, ID_BUTTON_RESET);

    refresh_ports();
    set_default_firmware_path();
}

static void layout_controls(HWND hwnd)
{
    RECT rc;
    int w;
    int h;
    int margin = 16;
    int label_w = 92;
    int button_w = 112;
    int row_h = 26;
    int x_label = margin;
    int x_field = margin + label_w;
    int field_w;
    int x_button;
    int y = 16;
    int log_h;

    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    x_button = w - margin - button_w;
    field_w = x_button - x_field - 10;
    if (field_w < 200) {
        field_w = 200;
    }

    log_h = h - 254;
    if (log_h < 120) {
        log_h = 120;
    }

    MoveWindow(g_hwnd_title_label, margin, y, w - (margin * 2), 28, TRUE);

    MoveWindow(g_hwnd_port_label, x_label, 54, label_w, row_h, TRUE);
    MoveWindow(g_hwnd_port_combo, x_field, 51, field_w, 180, TRUE);
    MoveWindow(g_hwnd_refresh_button, x_button, 51, button_w, row_h, TRUE);

    MoveWindow(g_hwnd_file_label, x_label, 90, label_w, row_h, TRUE);
    MoveWindow(g_hwnd_file_edit, x_field, 87, field_w, row_h, TRUE);
    MoveWindow(g_hwnd_browse_button, x_button, 87, button_w, row_h, TRUE);

    MoveWindow(g_hwnd_status_title_label, x_label, 126, label_w, row_h, TRUE);
    MoveWindow(g_hwnd_status_label, x_field, 126, w - x_field - margin, row_h, TRUE);

    MoveWindow(g_hwnd_progress, margin, 162, w - (margin * 2) - 72, row_h, TRUE);
    MoveWindow(g_hwnd_progress_label, w - margin - 58, 164, 58, row_h, TRUE);

    MoveWindow(g_hwnd_log, margin, 198, w - (margin * 2), log_h, TRUE);
    MoveWindow(g_hwnd_start_button, w - margin - (button_w * 2) - 8, h - 42,
               button_w, 28, TRUE);
    MoveWindow(g_hwnd_reset_button, w - margin - button_w, h - 42, button_w, 28, TRUE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd);
        layout_controls(hwnd);
        return 0;

    case WM_SIZE:
        layout_controls(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lparam;
        mmi->ptMinTrackSize.x = 720;
        mmi->ptMinTrackSize.y = 460;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_BUTTON_REFRESH:
            refresh_ports();
            return 0;
        case ID_BUTTON_BROWSE:
            browse_firmware_file();
            return 0;
        case ID_BUTTON_START:
            start_flash();
            return 0;
        case ID_BUTTON_RESET:
            reset_or_cancel();
            return 0;
        default:
            break;
        }
        break;

    case WM_FLASH_LOG: {
        char *text = (char *)lparam;
        append_log_text(text);
        free(text);
        return 0;
    }

    case WM_FLASH_STATUS: {
        char *text = (char *)lparam;
        SetWindowTextA(g_hwnd_status_label, text);
        free(text);
        return 0;
    }

    case WM_FLASH_PROGRESS:
        set_progress_percent((int)wparam);
        return 0;

    case WM_FLASH_DONE: {
        char *text = (char *)lparam;
        BOOL success = (BOOL)wparam;

        if (g_worker_thread != NULL) {
            CloseHandle(g_worker_thread);
            g_worker_thread = NULL;
        }

        set_busy(FALSE);
        SetWindowTextA(g_hwnd_status_label, success ? "Success" : text);
        append_log_text(text);
        if (!success && (lstrcmpiA(text, "Operation reset by user.") != 0)) {
            MessageBoxA(hwnd, text, "Firmware Update Error", MB_ICONERROR | MB_OK);
        }
        free(text);
        return 0;
    }

    case WM_CLOSE:
        if (g_busy) {
            int answer = MessageBoxA(
                hwnd,
                "Firmware update is running. Stop it and close the program?",
                "Update Running",
                MB_ICONQUESTION | MB_YESNO);
            if (answer != IDYES) {
                return 0;
            }
            InterlockedExchange(&g_cancel_requested, 1);
            if (g_worker_thread != NULL) {
                WaitForSingleObject(g_worker_thread, 2000);
            }
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_worker_thread != NULL) {
            CloseHandle(g_worker_thread);
            g_worker_thread = NULL;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance,
                   LPSTR cmd_line, int show_cmd)
{
    const char class_name[] = "FlashProgramCWindow";
    WNDCLASSA wc;
    HWND hwnd;
    INITCOMMONCONTROLSEX icc;
    MSG msg;

    (void)prev_instance;
    (void)cmd_line;

    ZeroMemory(&icc, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassA(&wc)) {
        MessageBoxA(NULL, "Cannot register window class.", "Flash Program C",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    hwnd = CreateWindowExA(
        0,
        class_name,
        "STM32G474 UART Firmware Updater",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        760,
        520,
        NULL,
        NULL,
        instance,
        NULL);

    if (hwnd == NULL) {
        MessageBoxA(NULL, "Cannot create main window.", "Flash Program C",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
