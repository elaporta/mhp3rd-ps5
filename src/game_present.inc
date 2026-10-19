// SPDX-License-Identifier: GPL-3.0-or-later
// Platform presentation seam. GE recording, textures and targets remain upstream.
    const auto require = [](VkResult rc, const char* operation) {
        if (rc != VK_SUCCESS) throw psprecomp::Error(std::string(operation) + " rc=" + std::to_string(rc));
    };
    if (main_frame) { end_gpu_segment(commands); slots[slot].gpu_timer_pending = gpu_timer_used; }
    std::uint32_t image_index = 0;
    const bool display = surface != VK_NULL_HANDLE && (source != VK_NULL_HANDLE || !game_frame);
    const VkSemaphore acquire = acquire_semaphore(commands);
    if (display) {
        if (swapchain_dirty || swapchain == VK_NULL_HANDLE) recreate_swapchain();
        require(vkAcquireNextImageKHR(device, swapchain, 5'000'000'000ull, acquire, VK_NULL_HANDLE, &image_index),
                "vkAcquireNextImageKHR");
        VkImage target = swapchain_images.at(image_index);
        transition(commands, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (source) {
            transition(commands, source, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            record_game_blit(commands, source, target);
            transition(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        transition(commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    require(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    std::array<VkCommandBuffer, 2> batch{};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = frame_batch(commands, batch);
    submit.pCommandBuffers = batch.data();
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (display) {
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquire;
        submit.pWaitDstStageMask = &wait_stage;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &render_finished.at(image_index);
    }
    require(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
    if (main_frame) recording = false;
    if (display) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1; present.pWaitSemaphores = &render_finished.at(image_index);
        present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &image_index;
        require(vkQueuePresentKHR(queue, &present), "vkQueuePresentKHR");
        yakumo::game_platform::count_present();
    }
