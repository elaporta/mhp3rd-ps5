// SPDX-License-Identifier: GPL-3.0-or-later
// Included in the projected Impl and in the failure-injection fixture.
    const auto require = [](VkResult rc, const char* operation) {
        if (rc != VK_SUCCESS) throw psprecomp::Error(std::string(operation)+" rc="+std::to_string(rc));
    };
    native_present_succeeded = false;
    if (main_frame) { end_gpu_segment(commands); slots[slot].gpu_timer_pending=gpu_timer_used; }
    const VkSemaphore acquire=acquire_semaphore(commands);
    std::uint32_t image_index=0;
    VkResult acquired=VK_NOT_READY;
    if (acquired_image) {
        // present_between already owns this image and its acquire semaphore.
        image_index=*acquired_image;
        acquired_image.reset();
        acquired=VK_SUCCESS;
    } else if (surface!=VK_NULL_HANDLE && (source!=VK_NULL_HANDLE || !game_frame)) {
        if (swapchain_dirty || swapchain==VK_NULL_HANDLE) recreate_swapchain();
        if (swapchain!=VK_NULL_HANDLE) {
            const auto start=perf::Clock::now();
            acquired=vkAcquireNextImageKHR(device,swapchain,5'000'000'000ull,acquire,VK_NULL_HANDLE,&image_index);
            perf::add_wait_time(perf::Clock::now()-start,perf::Stall::Acquire);
        }
    }
    if (acquired==VK_ERROR_OUT_OF_DATE_KHR) swapchain_dirty=true;
    if (acquired==VK_SUBOPTIMAL_KHR) swapchain_check=true;
    const bool display=acquired==VK_SUCCESS || acquired==VK_SUBOPTIMAL_KHR;
    if (!display && acquired!=VK_NOT_READY && acquired!=VK_TIMEOUT && acquired!=VK_ERROR_OUT_OF_DATE_KHR)
        require(acquired,"vkAcquireNextImageKHR");
    if (display) {
        VkImage target=swapchain_images.at(image_index);
        transition(commands,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (source) {
            transition(commands,source,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            record_game_blit(commands,source,target);
            transition(commands,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        if (game_frame && overlay_visible) record_overlay(commands,target);
        transition(commands,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    require(vkEndCommandBuffer(commands),"vkEndCommandBuffer");
    std::array<VkCommandBuffer,2> batch{};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount=frame_batch(commands,batch); submit.pCommandBuffers=batch.data();
    const VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (display) {
        submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&acquire; submit.pWaitDstStageMask=&wait_stage;
        submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&render_finished.at(image_index);
    }
    require(vkQueueSubmit(queue,1,&submit,fence),"vkQueueSubmit");
    if (main_frame) recording=false;
    if (display) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount=1; present.pWaitSemaphores=&render_finished.at(image_index);
        present.swapchainCount=1; present.pSwapchains=&swapchain; present.pImageIndices=&image_index;
        const auto start=perf::Clock::now();
        const auto rc=vkQueuePresentKHR(queue,&present);
        perf::add_wait_time(perf::Clock::now()-start,perf::Stall::Present);
        if (rc==VK_ERROR_OUT_OF_DATE_KHR) swapchain_dirty=true;
        else if (rc==VK_SUBOPTIMAL_KHR) swapchain_check=true;
        else require(rc,"vkQueuePresentKHR");
        if (rc==VK_SUCCESS || rc==VK_SUBOPTIMAL_KHR) {
            native_present_succeeded=true;
            yakumo::game_platform::count_present();
            yakumo::game_performance::note_present();
        }
    }
