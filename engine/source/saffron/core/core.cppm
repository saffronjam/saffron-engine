export module Saffron.Core;

import std;

export namespace se
{
    // Fixed-width aliases — short, Go-like spellings.
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;
    using i8 = std::int8_t;
    using i16 = std::int16_t;
    using i32 = std::int32_t;
    using i64 = std::int64_t;
    using f32 = float;
    using f64 = double;

    // A shared reference to a logical resource object (the meta-layer wrappers are
    // passed around as Ref<T> rather than opaque handles). Plain alias, no base class.
    template <typename T>
    using Ref = std::shared_ptr<T>;

    // Rust-style fallible return: every fallible function returns Result<T> and reports
    // failure with Err("message"). Result<T> is exactly std::expected<T, std::string>;
    // success is the value itself (or {} for Result<void>) — no Ok wrapper.
    template <typename T>
    using Result = std::expected<T, std::string>;

    inline auto Err(std::string message) -> std::unexpected<std::string>
    {
        return std::unexpected<std::string>(std::move(message));
    }

    inline constexpr std::string_view EngineName = "Saffron Engine";
    inline constexpr std::string_view EngineVersion = "0.1.0-vulkan";

    // A span of time, in seconds. Plain data; transform with free functions.
    struct TimeSpan
    {
        f32 seconds = 0.0f;
    };

    constexpr auto toMilliseconds(TimeSpan span) -> f32
    {
        return span.seconds * 1000.0f;
    }

    // A stable 64-bit identity. entt::entity values are not stable across runs,
    // so anything serialized carries a Uuid instead.
    struct Uuid
    {
        u64 value = 0;
    };

    auto newUuid() -> Uuid
    {
        static std::mt19937_64 engine{ std::random_device{}() };
        static std::uniform_int_distribution<u64> distribution{ 1, std::numeric_limits<u64>::max() };
        return Uuid{ distribution(engine) };
    }

    // Standard table-based base64 (RFC 4648) of a byte buffer. Used to ship small binary
    // blobs (e.g. thumbnail PNGs) over the JSON control protocol.
    auto base64Encode(const std::vector<u8>& bytes) -> std::string
    {
        static constexpr char table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((bytes.size() + 2) / 3) * 4);
        std::size_t i = 0;
        for (; i + 3 <= bytes.size(); i += 3)
        {
            const u32 n = (u32(bytes[i]) << 16) | (u32(bytes[i + 1]) << 8) | u32(bytes[i + 2]);
            out.push_back(table[(n >> 18) & 0x3F]);
            out.push_back(table[(n >> 12) & 0x3F]);
            out.push_back(table[(n >> 6) & 0x3F]);
            out.push_back(table[n & 0x3F]);
        }
        if (const std::size_t rem = bytes.size() - i; rem == 1)
        {
            const u32 n = u32(bytes[i]) << 16;
            out.push_back(table[(n >> 18) & 0x3F]);
            out.push_back(table[(n >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
        else if (rem == 2)
        {
            const u32 n = (u32(bytes[i]) << 16) | (u32(bytes[i + 1]) << 8);
            out.push_back(table[(n >> 18) & 0x3F]);
            out.push_back(table[(n >> 12) & 0x3F]);
            out.push_back(table[(n >> 6) & 0x3F]);
            out.push_back('=');
        }
        return out;
    }

    void logInfo(std::string_view message)
    {
        std::println("[saffron] {}", message);
    }

    void logWarn(std::string_view message)
    {
        std::println("[saffron] warn: {}", message);
    }

    void logError(std::string_view message)
    {
        std::println("[saffron] error: {}", message);
    }
}
