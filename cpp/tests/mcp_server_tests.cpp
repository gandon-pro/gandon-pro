#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "gandon/mcp_server.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct HttpResponse {
    int status{};
    std::string headers;
    std::string body;
};

std::size_t count_occurrences(const std::string& text, const std::string& sub) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(sub, pos)) != std::string::npos) {
        ++count;
        pos += sub.size();
    }
    return count;
}

HttpResponse send_http(int port, const std::string& raw_request) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(sock != INVALID_SOCKET);

    DWORD timeout_ms = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);

    int rc = SOCKET_ERROR;
    for (int attempt = 0; attempt < 25; ++attempt) {
        rc = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    assert(rc == 0);

    std::size_t sent = 0;
    while (sent < raw_request.size()) {
        const int n = send(sock, raw_request.data() + sent, static_cast<int>(raw_request.size() - sent), 0);
        assert(n > 0);
        sent += static_cast<std::size_t>(n);
    }

    std::string raw;
    char buf[4096];
    while (true) {
        const int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, static_cast<std::size_t>(n));
    }
    closesocket(sock);

    const auto split = raw.find("\r\n\r\n");
    assert(split != std::string::npos);

    HttpResponse resp;
    resp.headers = raw.substr(0, split);
    resp.body = raw.substr(split + 4);

    const auto sp1 = resp.headers.find(' ');
    assert(sp1 != std::string::npos);
    resp.status = std::stoi(resp.headers.substr(sp1 + 1, 3));
    return resp;
}

HttpResponse post_json_resp(int port, const std::string& path, const std::string& json_body,
                            const std::string& extra_headers = {}) {
    std::string req = "POST " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(json_body.size()) + "\r\n" +
                      extra_headers +
                      "Connection: close\r\n\r\n" + json_body;
    return send_http(port, req);
}

std::string post_json(int port, const std::string& path, const std::string& json_body,
                      const std::string& extra_headers = {}) {
    const auto resp = post_json_resp(port, path, json_body, extra_headers);
    assert(resp.status == 200);
    return resp.body;
}

} // namespace

int main() {
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    constexpr int kTestPort = 18766;
    bool info_called = false;
    bool renamed_called = false;

    gandon::McpHostCallbacks cb;
    cb.info_json = [&]() -> std::optional<std::string> {
        info_called = true;
        return std::string("{\"binary\":\"sample.exe\",\"arch\":\"x64\"}");
    };
    cb.list_functions = []() -> std::vector<gandon::McpFunctionInfo> {
        return {{0x140001000ull, "sub_140001000"}, {0x140001100ull, "main"}};
    };
    cb.resolve_address = [](const std::string& s) -> std::optional<std::uint64_t> {
        if (s == "main") return 0x140001100ull;
        return 0x140001000ull;
    };
    cb.rename = [&](std::uint64_t addr, const std::string& name) -> bool {
        renamed_called = (addr == 0x140001100ull && name == "entry_fn");
        return renamed_called;
    };

    gandon::McpServer server;
    server.set_callbacks(cb);

    gandon::McpServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = kTestPort;

    std::string err;
    assert(server.start(cfg, err));
    assert(server.is_running());

    // 1. Health endpoint
    const auto health = send_http(kTestPort,
        "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    assert(health.status == 200);
    assert(health.body.find("\"status\":\"ok\"") != std::string::npos);

    // 2. CORS preflight on /mcp
    const auto opts = send_http(kTestPort,
        "OPTIONS /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    assert(opts.status == 204);
    assert(opts.headers.find("Access-Control-Allow-Origin: *") != std::string::npos);

    // 3. GET /mcp -> 405 Method Not Allowed (Streamable HTTP spec)
    const auto get_mcp = send_http(kTestPort,
        "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    assert(get_mcp.status == 405);

    // 4. initialize + Mcp-Session-Id header
    const auto init = post_json_resp(kTestPort, "/mcp",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{}}}");
    assert(init.status == 200);
    assert(init.headers.find("Mcp-Session-Id:") != std::string::npos);
    assert(init.body.find("2025-03-26") != std::string::npos);

    // 5. tools/list must expose all 25 gandon_* tools
    const auto list_body = post_json(kTestPort, "/mcp",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}");
    assert(count_occurrences(list_body, "\"name\":\"gandon_") == 25);

    // 6. tools/call gandon_info and gandon_rename via callbacks
    const auto info_body = post_json(kTestPort, "/mcp",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"gandon_info\",\"arguments\":{}}}");
    assert(info_called);
    assert(info_body.find("sample.exe") != std::string::npos);

    const auto ren_body = post_json(kTestPort, "/mcp",
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"gandon_rename\",\"arguments\":{\"address\":\"main\",\"name\":\"entry_fn\"}}}");
    assert(renamed_called);
    assert(ren_body.find("entry_fn") != std::string::npos);

    // 7. Batch JSON-RPC request
    const auto batch_body = post_json(kTestPort, "/",
        "[{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"ping\"},"
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"gandon_list_functions\",\"arguments\":{}}}]");
    assert(!batch_body.empty() && batch_body.front() == '[');
    assert(batch_body.find("sub_140001000") != std::string::npos);

    server.stop();
    assert(!server.is_running());

    // 8. Restart with bearer token and verify case-insensitive auth + DELETE /mcp
    cfg.token = "secret123";
    assert(server.start(cfg, err));

    const auto unauth = post_json_resp(kTestPort, "/mcp",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
    assert(unauth.status == 401);

    const auto auth_ok = post_json(kTestPort, "/mcp",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}",
        "Authorization: bearer secret123\r\n");
    assert(auth_ok.find("\"result\":{}") != std::string::npos);

    const auto del_resp = send_http(kTestPort,
        "DELETE /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer secret123\r\nConnection: close\r\n\r\n");
    assert(del_resp.status == 200);

    server.stop();
    WSACleanup();
    std::cout << "mcp_server tests passed (25 tools, HTTP/1.1, batch, auth, clean stop)\n";
    return 0;
}
