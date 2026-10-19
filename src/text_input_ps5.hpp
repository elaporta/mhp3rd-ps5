// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "text_input_service.hpp"
#include <array>
#include <cstddef>
#include <utility>

namespace yakumo::text_input {
// Independent declarations from public OpenOrbis/shadPS4 ABI facts and the
// supplied PS5SX2 reference. PS5 compatibility remains a manual hardware gate.
// UTF-16 is explicitly 16-bit; platform wchar_t is not the IME character type.
struct ImeParam {
    std::int32_t user{}; std::uint32_t type{1}; std::uint64_t languages{};
    std::uint32_t enter_label{}, input_method{}; void* filter{};
    std::uint32_t options{0x22}, max_length{}; char16_t* buffer{};
    float x{960},y{300}; std::uint32_t horizontal{1},vertical{1};
    const char16_t* placeholder{}; const char16_t* title{};
    std::array<std::uint8_t,16> reserved{};
};
struct ImeResult { std::uint32_t end{}; std::array<std::uint8_t,12> reserved{}; };
static_assert(sizeof(char16_t)==2&&sizeof(ImeParam)==96&&sizeof(ImeResult)==16);
static_assert(offsetof(ImeParam,filter)==24&&offsetof(ImeParam,buffer)==40&&offsetof(ImeParam,title)==72);
struct ImeApi {
    int (*init)(ImeParam*,void*){}; int (*status)(){};
    int (*result)(ImeResult*){}; int (*term)(){}; int (*abort)(){};
};
std::optional<Failure> checked_ime_imports(ImeApi&,const ImeApi&);
class Ps5Backend final : public Backend {
public:
    using Resolve=std::function<std::optional<Failure>(ImeApi&)>;
    explicit Ps5Backend(Resolve resolve) : resolve_(std::move(resolve)) {}
    void user(int value){user_=value;}
    std::optional<Failure> open(const Request&) override;
    std::optional<Result> poll() override;
    void cancel() override;
    bool active() const override{return running_;}
private:
    Resolve resolve_; ImeApi api_{}; int user_{-1};
    ImeParam param_{};
    // Reserve worst-case surrogate pairs even if a runtime interprets its
    // max_length as scalars. Accepted output is still bounded in UTF-16 units.
    std::array<char16_t,1025> buffer_{};
    std::array<char16_t,129> title_{},prompt_{};
    bool running_{},closing_{},cancelled_{};
    unsigned close_attempts_{},cancel_polls_{};
    Result pending_{};
};
} // namespace yakumo::text_input
