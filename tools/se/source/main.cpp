#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    using json = nlohmann::json;

    std::string socketPath()
    {
        if (const char* override = std::getenv("SAFFRON_CONTROL_SOCK"))
        {
            return override;
        }
        if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"))
        {
            return std::string(runtime) + "/saffron-control.sock";
        }
        return "/tmp/saffron-control-" + std::to_string(static_cast<unsigned>(getuid())) + ".sock";
    }

    // true/false/null, integer, float, explicit json ({ [ "), else a bare string.
    json coerce(const std::string& token)
    {
        if (token == "true") { return true; }
        if (token == "false") { return false; }
        if (token == "null") { return nullptr; }
        if (!token.empty() && (token.front() == '{' || token.front() == '[' || token.front() == '"'))
        {
            json value = json::parse(token, nullptr, false);
            if (!value.is_discarded())
            {
                return value;
            }
        }
        char* end = nullptr;
        const long long asInt = std::strtoll(token.c_str(), &end, 10);
        if (end != token.c_str() && *end == '\0')
        {
            return asInt;
        }
        const double asDouble = std::strtod(token.c_str(), &end);
        if (end != token.c_str() && *end == '\0')
        {
            return asDouble;
        }
        return token;
    }

    // Positionals go to params["args"]; --flag value / --flag=value / bare --flag map
    // to params[flag]. Commands read positionals or flags via the same key.
    json buildParams(int argc, char** argv, int start)
    {
        json params = json::object();
        json positional = json::array();
        for (int i = start; i < argc; i = i + 1)
        {
            std::string arg = argv[i];
            if (arg.rfind("--", 0) == 0)
            {
                std::string key = arg.substr(2);
                const std::size_t eq = key.find('=');
                if (eq != std::string::npos)
                {
                    params[key.substr(0, eq)] = coerce(key.substr(eq + 1));
                }
                else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                {
                    i = i + 1;
                    params[key] = coerce(argv[i]);
                }
                else
                {
                    params[key] = true;
                }
            }
            else
            {
                positional.push_back(coerce(arg));
            }
        }
        if (!positional.empty())
        {
            params["args"] = positional;
        }
        return params;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: se <command> [positional...] [--flag value]\n");
        return 2;
    }

    json request;
    request["cmd"] = argv[1];
    request["params"] = buildParams(argc, argv, 2);
    request["id"] = 1;

    const std::string path = socketPath();
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        std::perror("se: socket");
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        std::fprintf(stderr, "se: cannot connect to %s: %s\n", path.c_str(), std::strerror(errno));
        ::close(fd);
        return 1;
    }

    std::string line = request.dump();
    line.push_back('\n');
    static_cast<void>(::send(fd, line.data(), line.size(), MSG_NOSIGNAL));

    std::string reply;
    char buffer[4096];
    while (reply.find('\n') == std::string::npos)
    {
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0)
        {
            break;
        }
        reply.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(fd);

    json response = json::parse(reply, nullptr, false);
    if (response.is_discarded())
    {
        std::fprintf(stderr, "se: malformed reply\n");
        return 1;
    }
    if (response.value("ok", false))
    {
        std::printf("%s\n", response.value("result", json::object()).dump(2).c_str());
        return 0;
    }
    std::fprintf(stderr, "se: %s\n", response.value("error", std::string{ "error" }).c_str());
    return 1;
}
