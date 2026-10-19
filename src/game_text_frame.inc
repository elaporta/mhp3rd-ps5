// SPDX-License-Identifier: GPL-3.0-or-later
// Included in the isolated renderer projection. Keep the pre-input image alive
// because the guest clears its own target before requesting its PSP OSK.
void VulkanRenderer::Impl::cache_text_background() {
    if (!ready || holding) return;
    VkImage source = VK_NULL_HANDLE;
    if (newer_picture_valid) source = pictures[newer_picture].color;
    else if (const auto shown = targets.find(presented_target); shown != targets.end() && shown->second.initialized)
        source = shown->second.color;
    if (!source) return;
    if (!held.color) {
        std::string error;
        if (!create_image(target_extent.width, target_extent.height, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                held.color, held.color_memory, held.color_view, VK_IMAGE_ASPECT_COLOR_BIT, error)) {
            std::cerr << "[osk-background] snapshot allocation failed: " << error << "\n";
            destroy_target(held); held = {}; return;
        }
    }
    const auto record = [&](VkCommandBuffer commands) {
        transition(commands, source, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(commands, held.color, held.initialized ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {target_extent.width, target_extent.height, 1u};
        vkCmdCopyImage(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, held.color,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
        transition(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        transition(commands, held.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    };
    // Normally append one GPU copy to the current frame: no per-frame CPU
    // readback or queue wait. The idle fallback uses upstream's one-time path.
    if (recording) { end_pass(); record(command_buffer); }
    else run_commands(record);
    held.initialized = true;
    text_capture_frame = frames;
}
void VulkanRenderer::hold_frame(bool hold) {
    if (!impl_ || !impl_->ready) return;
    Impl &impl = *impl_;
    if (!hold) {
        impl.text_hold_attempted = false;
        if (impl.holding) impl.text_recovery.begin(yakumo::game_platform::milliseconds(), impl.frames);
        return;
    }
    impl.text_recovery.cancel();
    if (hold == impl.holding || (hold && impl.text_hold_attempted)) return;
    if (hold) impl.text_hold_attempted = true;
    if (hold && (!impl.held.initialized || impl.frames - impl.text_capture_frame > 120u)) impl.cache_text_background();
    impl.holding = hold && impl.held.initialized;
    std::cout << "[osk-background] hold=" << impl.holding << " cached=" << impl.held.initialized
              << " age_frames=" << impl.frames - impl.text_capture_frame << " source=pre-input-snapshot\n";
    // Retain the allocation for the next input edge. Resize/shutdown own
    // destruction after GPU completion, including snapshots that were not held.
}

// Called after the guest flip has submitted its commands. Never read pixels
// from a command buffer still being recorded, or mistake the held image for
// evidence that the live guest has recovered.
void VulkanRenderer::recover_text_background() {
    if (!impl_ || !impl_->ready) return;
    Impl &impl = *impl_;
    if (!impl.text_recovery.active()) return;
    const auto now = yakumo::game_platform::milliseconds();
    const bool expired = impl.text_recovery.expired(now);
    if (!expired) {
        if (!impl.text_recovery.due(now, impl.frames)) return;
        std::vector<std::uint8_t> pixels;
        std::uint32_t width{}, height{};
        if (!read_frame(pixels, width, height, true)) return;
        const auto sample = yakumo::game_text_background::measure(pixels, width, height);
        if (!sample || !sample->nonblack) return;
    }
    impl.holding = false;
    impl.text_recovery.cancel();
    std::cout << "[osk-background] recovery=" << (expired ? "timeout" : "live-nonblack-sample")
              << " hold=0 sampled_bytes=256 final_scanout=UNMEASURED\n";
}
