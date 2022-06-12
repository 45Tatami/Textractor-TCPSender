#include "extension.h"
#include "resource.h"

#include <atomic>
#include <codecvt>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <string>

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
#define CONFIG_FILE_NAME L"tcpsender.config"

HMODULE hmod = NULL;
HWND win_hndl = NULL;

UINT const WM_USR_LOG = WM_APP + 1;
UINT const WM_USR_TOGGLE_CONNECT = WM_APP + 2;
UINT const WM_USR_LOAD_CONFIG = WM_APP + 3;

HANDLE comm_thread;
wstring remote = L"localhost:30501";
wstring config_file_path;

// Mutex/cv protects following vars
mutex conn_mut;
std::condition_variable conn_cv;
std::atomic<bool> comm_thread_run;
std::atomic<bool> want_connect;
std::atomic<bool> config_initialized;
std::deque<wstring> msg_q;

SOCKET _connect();
bool _send(SOCKET &, string const &);

wstring getEditBoxText(HWND win_hndl, int item) {
	if (win_hndl == NULL)
		return L"";

	HWND edit = GetDlgItem(win_hndl, item);
	if (edit == NULL) {
		MessageBox(NULL, L"Could not get editbox handle", L"Error", 0);
		return L"";
	}

	int len = GetWindowTextLength(edit);
	if (len == 0)
		return L"";

	wchar_t* buf = (wchar_t*)GlobalAlloc(GPTR, (len + 1) * sizeof(wchar_t));
	if (buf == NULL)
		return L"";

	GetDlgItemText(win_hndl, item, buf, len + 1);

	wstring tmp = wstring{ buf };
	GlobalFree(buf);

	return tmp;
}

void log(string const& msg)
{
	// Async to allow logging from dialog thread
	// Freed on message handling
	char* buf = (char *) GlobalAlloc(GPTR, msg.length() + 1);
	msg.copy(buf, msg.length());
	PostMessage(win_hndl, WM_USR_LOG, NULL, (LPARAM) buf);
}

void log(wstring const& msg)
{
	string tmp =
		wstring_convert<codecvt_utf8_utf16<wchar_t>>{}.to_bytes(msg);
	log(tmp);
}

void toggle_want_connect()
{
	PostMessage(win_hndl, WM_USR_TOGGLE_CONNECT, NULL, NULL);
}

void save_config(wstring const& filepath, wstring const& remote, bool connect)
{
	std::wofstream f{filepath, std::ios_base::trunc};
	if (f.good()) {
		f << remote.c_str() << "\n";
		f << connect;
	}
	else {
		MessageBox(NULL, L"Could not open config file for writing", L"Error", 0);
	}
}

/**
 * Connect to remote and wait for messages in queue to send until comm_thread_run is false
 */
DWORD WINAPI comm_loop(LPVOID lpParam)
{
	(void)lpParam;

	using namespace std::chrono_literals;
	WSADATA wsaData;

	log("Starting comm loop");

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		log("Could not initialize WSA. Exit");
		return 1;
	}

	SOCKET sock = INVALID_SOCKET;

	// Note: The communication thread is one big if/elseif/else. We keep the
	// mutex locked except when waiting for an event after which we will loop
	// again to see what happened and on long operations like connection
	// attempts or sending data.
	// This allows protecting muliple variables without performance problems.
	unique_lock<mutex> lk{conn_mut};

	while (!config_initialized) {
		conn_cv.wait(lk);
	}

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

	return 0;
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
	case WM_USR_LOG:
	{
		wstring tmp = getEditBoxText(hWnd, IDC_LOG);
		if (tmp.length() > 0)
			tmp.append(L"\r\n");

		// Remove older text to not slow down
		if (tmp.length() > 4000) {
			tmp.erase(0, 2000);
		}

		char* buf = (char *) lParam;
		tmp.append(wstring_convert<codecvt_utf8_utf16<wchar_t>>().from_bytes(buf));
		GlobalFree(buf);

		SetDlgItemText(hWnd, IDC_LOG, tmp.c_str());
		SendMessage(GetDlgItem(hWnd, IDC_LOG), EM_LINESCROLL, 0, INT_MAX);
		return true;
	}
	case WM_USR_TOGGLE_CONNECT:
	{
		lock_guard<mutex> conn_lk{ conn_mut };

		want_connect = !want_connect;

		HWND edit = GetDlgItem(hWnd, IDC_REMOTE);

		if (want_connect) {
			SetDlgItemText(hWnd, IDC_BTN_SUBMIT, L"Disconnect");
			SendMessage(edit, EM_SETREADONLY, TRUE, NULL);
		}
		else {
			SetDlgItemText(hWnd, IDC_BTN_SUBMIT, L"Connect");
			SendMessage(edit, EM_SETREADONLY, FALSE, NULL);
		}

		save_config(config_file_path, remote, want_connect);

		conn_cv.notify_one();
		return true;
	}
	case WM_USR_LOAD_CONFIG:
	{
		lock_guard<mutex> conn_lk{ conn_mut };

		log(L"Loading config: " + wstring{(wchar_t*) lParam});
		std::wifstream f{(wchar_t *) lParam};
		if (f.fail()) {
			log("Config file does not exist.");
			goto config_done;
		}

		std::getline(f, remote);
		SetDlgItemText(win_hndl, IDC_REMOTE, remote.c_str());

		bool connect;
		f >> connect;
		if (connect)
			toggle_want_connect();

config_done:
		comm_thread_run = true;
		config_initialized = true;
		conn_cv.notify_one();

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
		// We need to create the Window here since otherwise it will be owned
		// by some worker thread
		// But try to do as few things as possible
		hmod = hModule;

		// Create window
		win_hndl = CreateDialogParam(hmod, MAKEINTRESOURCE(IDD_DIALOG1),
			NULL, DialogProc, 0);

		if (win_hndl == NULL) {
			MessageBox(NULL, L"Could not open plugin dialog", L"Error", 0);
			return false;
		}
		ShowWindow(win_hndl, SW_NORMAL);

		wchar_t* buf;

		// Get config path
		DWORD buf_sz = (GetCurrentDirectory(0, NULL) + 1) * sizeof(wchar_t);
		buf = (wchar_t*)GlobalAlloc(GPTR, buf_sz + 4);
		if (buf == NULL)
			return false;

		GetCurrentDirectory(buf_sz, buf);

		config_file_path = std::filesystem::path{wstring{buf}} / CONFIG_FILE_NAME;
		GlobalFree(buf);

		PostMessage(win_hndl, WM_USR_LOAD_CONFIG,
			NULL, (LPARAM) config_file_path.c_str());

		// Start communication thread
		comm_thread = CreateThread(NULL, 0, comm_loop, NULL, 0, NULL);
	}
	break;
	case DLL_PROCESS_DETACH:
	{
		// Signal and wait for cleanup of comm thread would be good but
		// join/WaitForSingleObject does not work in DLL_PROCESS_DETACH

		// unique_lock<mutex> lk{ conn_mut };
		// comm_thread_run = false;
		// conn_cv.notify_one();
		// lk.unlock();

		// if (comm_thread.joinable())
		//	comm_thread.join();

		DestroyWindow(win_hndl);
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
