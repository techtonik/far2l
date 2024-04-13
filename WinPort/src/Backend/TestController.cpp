#include "TestController.h"
#include "Backend.h"
#include "CheckedCast.hpp"
#include "os_call.hpp"
#include "WinPort.h"
#include <LocalSocket.h>

static void StrCpyZeroFill(char *dst, size_t dst_len, const std::string &src)
{
	for (size_t i = 0, j = src.size(); i < dst_len; ++i) {
		dst[i] = (i < j) ? src[i] : 0;
	}
}

TestController::TestController(const std::string &id)
	: _ipc_server(id)
{
	StartThread();
}

TestController::~TestController()
{
	_stop = true;
	WaitThread();
}

void *TestController::ThreadProc()
{
	const auto &ipc_client = InMyTemp(StrPrintf("test/%u", getpid()).c_str());
	fprintf(stderr, "TestController: loop started, ipc_client='%s'\n", ipc_client.c_str());
	try {
		ClientLoop(ipc_client);
	} catch (std::exception &e) {
		fprintf(stderr, "TestController: %s\n", e.what());
	}
	if (unlink(ipc_client.c_str()) == -1) {
		perror("TestController - unlink");
	}
	fprintf(stderr, "TestController: loop stopped\n");

	return nullptr;
}

void TestController::ClientLoop(const std::string &ipc_client)
{
	LocalSocketClient sock(LocalSocket::DATAGRAM, _ipc_server, ipc_client);
	sock.Send(&_buf, ClientDispatchStatus());
	for (;;) {
		size_t len = sock.Recv(&_buf, sizeof(_buf));
		if (len < sizeof(_buf.cmd)) {
			throw std::runtime_error(StrPrintf("too small len %lu", (unsigned long)len));
		}
		fprintf(stderr, "TestController: got command %u\n", _buf.cmd);

		switch (_buf.cmd) {
			case TEST_CMD_DETACH:
				return;

			case TEST_CMD_STATUS:
				len = ClientDispatchStatus();
				break;

			case TEST_CMD_READ_CELL:
				len = ClientDispatchReadCell(len);
				break;

			case TEST_CMD_WAIT_STRING:
				len = ClientDispatchWaitString(len);
				break;

			case TEST_CMD_SEND_KEY:
				len = ClientDispatchSendKey(len);
				break;

			default:
				throw std::runtime_error(StrPrintf("bad command %u", _buf.cmd));
		}
		if (len) {
			sock.Send(&_buf, len);
		}
	}
}

size_t TestController::ClientDispatchStatus()
{
	const auto &title = StrWide2MB(g_winport_con_out->GetTitle());
	UCHAR cur_height{};
	bool cur_visible{};
	COORD cur_pos = g_winport_con_out->GetCursor(cur_height, cur_visible);
	unsigned int width{}, height{};
	g_winport_con_out->GetSize(width, height);
	_buf.rep_status.version = TEST_PROTOCOL_VERSION;
	_buf.rep_status.cur_height = cur_height;
	_buf.rep_status.cur_visible = cur_visible ? 1 : 0;
	_buf.rep_status.cur_x = cur_pos.X;
	_buf.rep_status.cur_y = cur_pos.Y;
	_buf.rep_status.width = width;
	_buf.rep_status.height = height;
	StrCpyZeroFill(_buf.rep_status.title, ARRAYSIZE(_buf.rep_status.title), title);
	return sizeof(_buf.rep_status);
}

size_t TestController::ClientDispatchReadCell(size_t len)
{
	if (len < sizeof(TestRequestReadCell)) {
		throw std::runtime_error(StrPrintf("len=%lu < sizeof(TestRequestReadCell)", (unsigned long)len));
	}

	CHAR_INFO ci{};
	if (g_winport_con_out->Read(ci,
			COORD{
				CheckedCast<SHORT>(_buf.req_read_cell.x),
				CheckedCast<SHORT>(_buf.req_read_cell.y)
			}
		)) {
		_buf.rep_read_cell.attributes = ci.Attributes;
		std::string str;
		if (CI_USING_COMPOSITE_CHAR(ci)) {
			Wide2MB(WINPORT(CompositeCharLookup)(ci.Char.UnicodeChar), str);
		} else {
			const wchar_t wc = (wchar_t)ci.Char.UnicodeChar;
			Wide2MB(&wc, 1, str);
		}
		StrCpyZeroFill(_buf.rep_read_cell.str, ARRAYSIZE(_buf.rep_read_cell.str), str);

	} else {
		ZeroFill(_buf.rep_read_cell);
	}
	return sizeof(_buf.rep_read_cell);
}

size_t TestController::ClientDispatchWaitString(size_t len)
{
	if (len < sizeof(TestRequestWaitString)) {
		throw std::runtime_error(StrPrintf("len=%lu < sizeof(TestRequestWaitString)", (unsigned long)len));
	}
	std::vector<CHAR_INFO> data(size_t(_buf.req_wait_str.width) * _buf.req_wait_str.height);
	if (!data.empty()) {
		std::vector<std::wstring> str_match_vec;
		//_buf.req_wait_str.str[ARRAYSIZE(_buf.req_wait_str.str) - 1] = 0;
		for (uint32_t ofs = 0; ofs < ARRAYSIZE(_buf.req_wait_str.str) && _buf.req_wait_str.str[ofs];) {
			const size_t l = strnlen(&_buf.req_wait_str.str[ofs], ARRAYSIZE(_buf.req_wait_str.str) - ofs);
			str_match_vec.emplace_back();
			MB2Wide(&_buf.req_wait_str.str[ofs], l, str_match_vec.back());
			ofs+= l + 1;
		}

		std::wstring str;
		std::vector<uint32_t> ofs;
		unsigned int change_id = 0;
		uint32_t msec_remain = _buf.req_wait_str.timeout;
		clock_t cl_begin = GetProcessUptimeMSec();
		do {
			change_id = g_winport_con_out->WaitForChange(change_id, msec_remain);
			if (msec_remain != (uint32_t)-1) {
				const clock_t cl_now = GetProcessUptimeMSec();
				const uint32_t msec_passed = uint32_t(
					(cl_now >= cl_begin) ? cl_now - cl_begin : ((clock_t)-1 - cl_begin) + cl_now
				);
				cl_begin = cl_now;
				if (msec_remain <= msec_passed) {
					msec_remain = 0;
				} else {
					msec_remain-= msec_passed;
				}
			}
			COORD data_size{
				CheckedCast<SHORT>(_buf.req_wait_str.width),
				CheckedCast<SHORT>(_buf.req_wait_str.height)
			};
			COORD data_pos{0, 0};
			SMALL_RECT screen_rect{
				CheckedCast<SHORT>(_buf.req_wait_str.left),
				CheckedCast<SHORT>(_buf.req_wait_str.top),
				CheckedCast<SHORT>(_buf.req_wait_str.left + _buf.req_wait_str.width - 1),
				CheckedCast<SHORT>(_buf.req_wait_str.top + _buf.req_wait_str.height - 1)
			};
			memset(data.data(), 0, data.size() * sizeof(CHAR_INFO));
			g_winport_con_out->Read(data.data(), data_size, data_pos, screen_rect);
			for (uint32_t y = 0; y < _buf.req_wait_str.height; ++y) {
				str.clear();
				ofs.clear();
				for (uint32_t x = 0; x < _buf.req_wait_str.width; ++x) {
					const auto &ci = data[y * uint32_t(_buf.req_wait_str.width) + x];
					if (CI_USING_COMPOSITE_CHAR(ci)) {
						str+= WINPORT(CompositeCharLookup)(ci.Char.UnicodeChar);
					} else if (ci.Char.UnicodeChar) {
						str+= (wchar_t)ci.Char.UnicodeChar;
					}
					while (ofs.size() < str.size()) {
						ofs.emplace_back(x);
					}
				}
				for (size_t i = 0; i < str_match_vec.size(); ++i) {
					const size_t pos = str.find(str_match_vec[i].c_str());
					if (pos != std::wstring::npos) {
						ASSERT(pos < ofs.size());
						_buf.rep_wait_str.i = (uint32_t)i;
						_buf.rep_wait_str.x = _buf.req_wait_str.left + ofs[pos];
						_buf.rep_wait_str.y = _buf.req_wait_str.top + y;
						return sizeof(_buf.rep_wait_str);
					}
				}
			}
		} while (msec_remain);
	}
	_buf.rep_wait_str.i = -1;
	_buf.rep_wait_str.x = -1;
	_buf.rep_wait_str.y = -1;
	return sizeof(_buf.rep_wait_str);
}

size_t TestController::ClientDispatchSendKey(size_t len)
{
	if (len < sizeof(TestRequestSendKey)) {
		throw std::runtime_error(StrPrintf("len=%lu < sizeof(TestRequestSendKey)", (unsigned long)len));
	}
	INPUT_RECORD ir{};
	ir.EventType = KEY_EVENT;
	ir.Event.KeyEvent.bKeyDown = _buf.req_send_key.pressed ? TRUE : FALSE;
	ir.Event.KeyEvent.wRepeatCount = 1;
	ir.Event.KeyEvent.wVirtualKeyCode = _buf.req_send_key.key_code;
	ir.Event.KeyEvent.wVirtualScanCode = _buf.req_send_key.scan_code;
	ir.Event.KeyEvent.uChar.UnicodeChar = _buf.req_send_key.chr;
	ir.Event.KeyEvent.dwControlKeyState = _buf.req_send_key.controls;
	g_winport_con_in->Enqueue(&ir, 1);
	return 0;
}