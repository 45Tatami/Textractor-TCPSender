#include "extension.h"
#include "resource.h"

#include <atomic>
#include <codecvt>
#include <condition_variable>
#include <deque>
#include <locale>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <strsafe.h>

using std::lock_guard;
using std::mutex;
using std::unique_lock;
using std::string;
using std::wstring;
using std::wstring_convert;
using std::codecvt_utf8_utf16;

#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")
#pragma comment (lib, "AdvApi32.lib")

#define MSG_Q_CAP 10
#define CONFIG_APP_NAME L"TCPSend"
#define CONFIG_ENTRY_REMOTE L"Remote"
#define CONFIG_ENTRY_CONNECT L"WantConnect"
#define CONFIG_FILE_NAME L"Textractor.ini"

std::thread comm_thread;
wstring remote = L"localhost:30501";
wstring config_file_path;
HWND hwnd = NULL;

// Mutex/cv protects following vars
mutex conn_mut;
std::condition_variable conn_cv;
std::atomic<bool> comm_thread_run;
std::atomic<bool> want_connect;
std::deque<wstring> msg_q;

SOCKET _connect();
bool _send(SOCKET &, string const &);

wstring getEditBoxText(HWND hndl, int item) {
	if (hwnd == NULL)
		return L"";

	int len = GetWindowTextLength(GetDlgItem(hndl, item));
	if (len == 0)
		return L"";

	wchar_t* buf = (wchar_t*)GlobalAlloc(GPTR, (len + 1) * sizeof(wchar_t));
	if (buf == NULL)
		return L"";

	GetDlgItemText(hndl, item, buf, len + 1);

	wstring tmp = wstring{ buf };
	GlobalFree(buf);

	return tmp;
}

void log(string const& msg)
{
	if (hwnd == NULL)
		return;

	wstring cur = getEditBoxText(hwnd, IDC_LOG);
	if (cur.length() > 0)
		cur += L"\r\n";

	wstring tmp =
		cur + wstring_convert<codecvt_utf8_utf16<wchar_t>>().from_bytes(msg);
	SetDlgItemText(hwnd, IDC_LOG, tmp.c_str());
	SendMessage(GetDlgItem(hwnd, IDC_LOG), EM_LINESCROLL, 0, INT_MAX);
}

void log(wstring const& msg)
{
	string tmp =
		wstring_convert<codecvt_utf8_utf16<wchar_t>>{}.to_bytes(msg);
	log(tmp);
}

void write_config_val(LPCSTR key, LPCSTR val)
{

}

void toggle_want_connect()
{
	unique_lock<mutex> conn_lk{conn_mut};

	want_connect = !want_connect;

	if (hwnd == NULL)
		return;

	HWND edit = GetDlgItem(hwnd, IDC_REMOTE);

	if (want_connect) {
		SetDlgItemText(hwnd, IDC_BTN_SUBMIT, L"Disconnect");
		SendMessage(edit, EM_SETREADONLY, TRUE, NULL);
	} else {
		SetDlgItemText(hwnd, IDC_BTN_SUBMIT, L"Connect");
		SendMessage(edit, EM_SETREADONLY, FALSE, NULL);
	}

	conn_cv.notify_one();
}

/**
 * Connect to remote and wait for messages in queue to send until comm_thread_run is false
 */
void comm_loop()
{
	using namespace std::chrono_literals;
	WSADATA wsaData;

	log("Starting comm loop");

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		log("Could not initialize WSA. Exit");
		return;
	}

	SOCKET sock = INVALID_SOCKET;

	// Note: The communication thread is one big if/elseif/else. We keep the
	// mutex locked except when waiting for an event after which we will loop
	// again to see what happened and on long operations like connection
	// attempts or sending data.
	// This allows protecting muliple variables without performance problems.
	comm_thread_run = true;
	unique_lock<mutex> lk{conn_mut};
	while (comm_thread_run) {
		// If we are not connected, try to connect if wanted, wait if we don't
		if (sock == INVALID_SOCKET) {
			if (want_connect) {
				lk.unlock(); // Don't lock for connect
				sock = _connect();
				lk.lock();
				if (sock == INVALID_SOCKET) {
					log("Connection failed. Retrying soon.");
					conn_cv.wait_for(lk, 1000ms);
				} else {
					log("Successfully connected");
				}
			} else {
				conn_cv.wait(lk);
			}
		// If we are connected, but shouldn't be, disconnect
		} else if (!want_connect) {
			log("Disconnecting");
			closesocket(sock);
			sock = INVALID_SOCKET;
		// If we are connected but there's no data available, wait
		} else if (msg_q.empty()) {
			conn_cv.wait(lk);
		// We are connected and there is data available
		} else {
			// Remove first element, unlock, push back on error
			wstring msg = msg_q.front();
			msg_q.pop_front();

			lk.unlock();

			string msg_utf8 =
				wstring_convert<codecvt_utf8_utf16<wchar_t>>{}.to_bytes(msg);
			log("Sending '" + msg_utf8 + "'");

			if (!_send(sock, msg_utf8)) {
				log("Error sending");
				closesocket(sock);
				sock = INVALID_SOCKET;
				lk.lock();
				if (msg_q.size() < MSG_Q_CAP)
					msg_q.push_front(msg);
				lk.unlock();
			}

			lk.lock();
		}
	}

	log("Comm cleanup and exit");

	closesocket(sock);
	WSACleanup();
}

INT_PTR CALLBACK DialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
	{
		SetDlgItemText(hWnd, IDC_REMOTE, remote.c_str());
		return true;
	}
	case WM_COMMAND:
	{
		switch (LOWORD(wParam))
		{
		case IDC_BTN_SUBMIT:
		{
			remote = getEditBoxText(hWnd, IDC_REMOTE);
			toggle_want_connect();

			break;
		}
		default:
			return false;
		}
		return true;
	}
	default:
		return false;
	}
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		wchar_t* buf;

		// Get config path
		DWORD buf_sz = (GetCurrentDirectory(0, NULL) + 1) * sizeof(wchar_t);
		buf = (wchar_t*)GlobalAlloc(GPTR, buf_sz + 4);
		if (buf == NULL)
			return false;

		GetCurrentDirectory(buf_sz, buf);

		config_file_path = wstring{buf} + CONFIG_FILE_NAME;
		GlobalFree(buf);

		// Get configured remote
		buf = (wchar_t*)GlobalAlloc(GPTR, 1000 * sizeof(wchar_t));
		if (buf == NULL)
			return false;

		GetPrivateProfileString(CONFIG_APP_NAME, CONFIG_ENTRY_REMOTE,
			remote.c_str(), buf, 1000, config_file_path.c_str());
		remote = wstring{buf};

		GlobalFree(buf);

		// Get configured connection state
		UINT w = GetPrivateProfileInt(
			CONFIG_APP_NAME, CONFIG_ENTRY_CONNECT,
			want_connect, config_file_path.c_str());

		// Create window
		hwnd = CreateDialogParam(hModule, MAKEINTRESOURCE(IDD_DIALOG1),
			FindWindow(NULL, L"Textractor"), DialogProc, 0);

		if (hwnd == NULL) {
			MessageBox(NULL, L"Could not open plugin dialog", L"Error", 0);
			return false;
		}

		if (w)
			toggle_want_connect();

		// Start communication thread
		comm_thread = std::thread{comm_loop};
	}
	break;
	case DLL_PROCESS_DETACH:
	{
		unique_lock<mutex> lk{conn_mut};
		comm_thread_run = false;
		lk.unlock();

		conn_cv.notify_one();

		if (comm_thread.joinable())
			comm_thread.join();

		if (hwnd != NULL)
			CloseWindow(hwnd);

		WritePrivateProfileString(
			CONFIG_APP_NAME, CONFIG_ENTRY_CONNECT,
			(want_connect ? L"1" : L"0"), config_file_path.c_str());

		WritePrivateProfileString(
			CONFIG_APP_NAME, CONFIG_ENTRY_REMOTE,
			remote.c_str(), config_file_path.c_str());
	}
	break;
	}
	return true;
}

SOCKET _connect() {
	SOCKET sock;
	struct addrinfo* result = NULL,
		* ptr = NULL,
		hints;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	string remote_ch =
		wstring_convert<codecvt_utf8_utf16<wchar_t>>{}.to_bytes(remote);

	log("Connecting to " + remote_ch);

	string::size_type pos = remote_ch.rfind(":");
	string port = pos == string::npos ? "30501" : remote_ch.substr(pos + 1);
	int res = getaddrinfo(remote_ch.substr(0, pos).c_str(), port.c_str(), &hints, &result);
	if (res != 0) {
		return INVALID_SOCKET;
	}

	for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
		sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
		if (sock == INVALID_SOCKET) {
			return sock;
		}

		res = connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen);
		if (res == SOCKET_ERROR) {
			closesocket(sock);
			sock = INVALID_SOCKET;
			continue;
		}
		break;
	}

	freeaddrinfo(result);

	return sock;
}

bool _send(SOCKET &sock, string const &msg) {
	int len = (int) msg.length();
	int buf_len = len + 4;
	char* buf = new char[buf_len + 1];

	strcpy_s(buf + 4, buf_len - 4 + 1, msg.c_str());
	*((uint32_t*)buf) = len;

	for (int sent = 0, ret = 0; sent < buf_len; sent += ret) {
		ret = send(sock, buf + sent, buf_len - sent, 0);
		if (ret == SOCKET_ERROR) {
			delete[] buf;
			return false;
		}
	}
	delete[] buf;
	return true;
}

/*
   Param sentence: sentence received by Textractor (UTF-16). Can be modified, Textractor will receive this modification only if true is returned.
   Param sentenceInfo: contains miscellaneous info about the sentence (see README).
   Return value: whether the sentence was modified.
   Textractor will display the sentence after all extensions have had a chance to process and/or modify it.
   The sentence will be destroyed if it is empty or if you call Skip().
   This function may be run concurrently with itself: please make sure it's thread safe.
   It will not be run concurrently with DllMain.
   */
bool ProcessSentence(wstring & sentence, SentenceInfo sentenceInfo)
{
	if (sentenceInfo["current select"]) {
		log("Received sentence");
		lock_guard<mutex> lock{conn_mut};

		if (msg_q.size() >= MSG_Q_CAP)
			msg_q.pop_front();

		msg_q.push_back(wstring{ sentence });
		conn_cv.notify_one();
	}

	return false;
}
