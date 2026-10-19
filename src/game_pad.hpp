// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "input/presets.hpp"
#include "input/chords.hpp"
#include "kernel/fast_forward.hpp"
#include <array>
#include <vector>
namespace yakumo::game_pad {
inline mhp3rd::fast_forward::Guards fast_forward_guards(mhp3rd::fast_forward::Guards guards,bool text_capture) {
    guards.menu|=text_capture;
    return guards;
}
inline bool reserved(mhp3rd::input::Binding b) {
    using namespace mhp3rd::input;
    return b==pad(PadInput::LeftTrigger)||b==pad(PadInput::RightTrigger)||
        b==pad(PadInput::Back)||b==pad(PadInput::Touchpad)||
        b==pad(PadInput::LeftShoulder)||b==pad(PadInput::RightShoulder);
}
inline bool editable_input(mhp3rd::input::Binding b) {
    using namespace mhp3rd::input;
    const int index=pad_input_of(b);
    return index>=0 && index<=int(PadInput::DpadRight) && !reserved(b) && index!=int(PadInput::Guide);
}
inline mhp3rd::input::Layout native_layout(mhp3rd::input::Layout layout) {
    using namespace mhp3rd::input;
    // Reserved native controls cannot also fire a saved trigger binding/combo.
    // The native SDK exposes the whole touchpad click as Back. Reserve both
    // names so saved desktop aliases/chords cannot also send Select or attacks.
    const auto remove_reserved=[&](Slots& slots){
        Slots kept{};std::size_t count=0;
        for(const auto& chord:slots) {
            bool contains=false;for(auto b:chord.inputs)contains|=b!=kNone&&!editable_input(b);
#if YAKUMO_GAME_WEB
            bool left=false,right=false;
            for(auto b:chord.inputs){left|=b==pad(PadInput::LeftStick);right|=b==pad(PadInput::RightStick);}
            contains|=left&&right;
#endif
            if(!contains && !chord.empty())kept[count++]=chord;
        }
        slots=kept;
    };
    for(auto& slots:layout.pad)remove_reserved(slots);
    for(auto& combo:layout.combos)remove_reserved(combo.pad);
    return layout;
}
struct AssignedActions {
    mhp3rd::input::PadState state{};
    std::uint32_t lead_requested{};
};
inline AssignedActions assigned_actions(const std::array<mhp3rd::input::Action,5>& assignments,
                                         const std::array<bool,5>& pressed) {
    using namespace mhp3rd::input;
    static const Bindings empty{};
    std::vector<std::size_t> targets;
    for(std::size_t i=0;i<assignments.size();++i)
        if(pressed[i]&&assignments[i]!=Action::Count)targets.push_back(static_cast<std::size_t>(assignments[i]));
    const Table table{empty,{},true};
    return {state_of(table,targets),lead_requested(table,targets)};
}
}
