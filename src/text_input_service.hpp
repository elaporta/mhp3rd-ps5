// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace yakumo::text_input {
enum class Outcome { Confirmed, Cancelled, Error };
enum class KeyboardKind : std::uint32_t { Text=0, BasicLatin=1, Url=2, Email=3, Number=4 };
struct Failure { std::string operation; int code{}; };
struct Request {
    std::string title, prompt, initial; // Strict UTF-8; no embedded NUL.
    std::size_t max_characters{512}, max_utf16_units{512}, max_utf8_bytes{2048};
    bool allow_empty{true};
    bool multiline{};
    KeyboardKind keyboard{KeyboardKind::Text}; // Presentation hint, never validation.
    std::function<bool(char32_t)> allowed;
};
struct Result {
    std::uint64_t id{};
    Outcome outcome{Outcome::Error};
    std::string text; // Present only for Confirmed.
    Failure failure;
};
bool decode_utf8(std::string_view, std::u16string&);
bool decode_utf16(std::u16string_view, std::string&);
bool validate(const Request&, std::string_view);
bool hunter_name_character(char32_t);
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::optional<Failure> open(const Request&) = 0;
    virtual std::optional<Result> poll() = 0;
    virtual void cancel() = 0;
    virtual bool active() const = 0;
};
struct Submission { std::uint64_t id{}; std::optional<Failure> failure; };
// Single owner thread. A future bridge must marshal messages onto that thread.
class Service {
public:
    explicit Service(Backend& backend) : backend_(&backend) {}
    Submission submit(Request);
    void tick();
    std::optional<Result> take(std::uint64_t);
    void cancel(std::uint64_t);
    void shutdown();
    bool blocks_input() const;
    bool active() const{return active_||backend_->active();}
    // Even the release sample stays neutral to the guest.
    bool suppress_sample(bool physically_neutral);
    bool set_backend(Backend&); // Idle-only dependency injection.
private:
    Backend* backend_;
    Request request_;
    std::uint64_t next_{}, active_{};
    bool drain_{}, cancelling_{};
    std::optional<Result> result_;
};
Service& service();
void initialize_ps5(int user);
} // namespace yakumo::text_input
