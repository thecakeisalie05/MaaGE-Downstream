/* Private Vulkan effect resources. Slot ownership follows the existing frame fence. */
static void maage_vulkan_effect_barrier(maage_vulkan_renderer *native, VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect, VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags source, VkPipelineStageFlags destination, VkAccessFlags read, VkAccessFlags write) {
    VkImageMemoryBarrier barrier = {0};
    (void) native;
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcAccessMask = read;
    barrier.dstAccessMask = write;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, source, destination, 0, 0, NULL, 0, NULL, 1, &barrier);
}
static maage_status maage_vulkan_effect_pass(maage_renderer *renderer) {
    maage_vulkan_renderer *native = renderer->platform;
    maage_vulkan_rendering *gpu = native->rendering;
    VkAttachmentDescription attachment = {0};
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {0};
    VkSubpassDependency dependencies[2] = {{0}};
    VkRenderPassCreateInfo info = {0};
    attachment.format = VK_FORMAT_D32_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &reference;
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    return maage_vulkan_result(vkCreateRenderPass(native->device, &info, NULL, &gpu->shadow_pass));
}
static void maage_vulkan_effect_destroy(maage_vulkan_renderer *native, maage_vulkan_effects *e) {
    if (e->shadow_framebuffer) vkDestroyFramebuffer(native->device, e->shadow_framebuffer, NULL);
    if (e->scatter_framebuffer) vkDestroyFramebuffer(native->device, e->scatter_framebuffer, NULL);
    maage_vulkan_image_destroy(native, &e->descriptor);
    maage_vulkan_image_destroy(native, &e->shadow);
    maage_vulkan_image_destroy(native, &e->opaque_depth);
    maage_vulkan_image_destroy(native, &e->opaque_color);
    maage_vulkan_image_destroy(native, &e->scatter);
    maage_vulkan_buffer_destroy(native, &e->uniforms);
    maage_vulkan_buffer_destroy(native, &e->lights);
    memset(e, 0, sizeof(*e));
}
static maage_status maage_vulkan_effect_framebuffer(maage_renderer *renderer, VkRenderPass pass, VkImageView view, unsigned int width, unsigned int height, VkFramebuffer *output) {
    maage_vulkan_renderer *native = renderer->platform;
    VkFramebufferCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = 1;
    info.pAttachments = &view;
    info.width = width;
    info.height = height;
    info.layers = 1;
    return maage_vulkan_result(vkCreateFramebuffer(native->device, &info, NULL, output));
}
static maage_status maage_vulkan_effect_prepare(maage_renderer *renderer, maage_vulkan_effects *e, const maage_render_frame_plan *plan, VkCommandBuffer command) {
    maage_vulkan_renderer *native = renderer->platform;
    maage_vulkan_rendering *gpu = native->rendering;
    static const unsigned int resolutions[4] = {1024, 1536, 2048, 4096};
    unsigned int cascades = plan->effects.shadows && plan->effect_frame.weather[2] > 0 ? (plan->effects.shadow_quality == 0 ? 2 : plan->effects.shadow_quality == 1 ? 3
                                                                                                                                                                    : 4)
                                                                                       : 0;
    unsigned int resolution = cascades ? resolutions[plan->effects.shadow_quality] : 1;
    unsigned int shadow_width, shadow_height;
    int copies = plan->effects.water || plan->effects.underwater || (plan->effects.god_rays && plan->effect_frame.weather[2] > 0);
    unsigned int width = copies ? native->extent.width : 1, height = copies ? native->extent.height : 1;
    unsigned int divisor = plan->effects.ray_quality ? 2 : 4;
    unsigned int sw = plan->effects.god_rays && plan->effect_frame.weather[2] > 0 ? (native->extent.width + divisor - 1) / divisor : 1;
    unsigned int sh = sw > 1 ? (native->extent.height + divisor - 1) / divisor : 1;
    size_t light_count = plan->effects.dynamic_lights ? plan->effect_frame.light_count : 0;
    unsigned long budget = 128;
    const char *configured = getenv("MAAGE_SHADER_SHADOW_MIB");
    if (configured && *configured) {
        char *end;
        unsigned long value = strtoul(configured, &end, 10);
        if (!*end && value >= 4 && value <= 4096) budget = value;
    }
    while (cascades && (resolution * 2U > native->properties.limits.maxImageDimension2D || resolution * 2U > native->properties.limits.maxFramebufferWidth || resolution * (cascades > 2 ? 2U : 1U) > native->properties.limits.maxFramebufferHeight || (VkDeviceSize) resolution * resolution * 4U * (cascades > 2 ? 4U : 2U) * MAAGE_VULKAN_FRAME_SLOTS > (VkDeviceSize) budget * 1024U * 1024U))
        resolution /= 2U;
    shadow_width = cascades ? resolution * 2 : 1;
    shadow_height = cascades > 2 ? resolution * 2 : resolution;
    if (!e->descriptor.descriptor) MAAGE_VULKAN_TRY(maage_vulkan_descriptor_allocate(renderer, gpu->effect_descriptors, &e->descriptor));
    MAAGE_VULKAN_TRY(maage_vulkan_host_buffer(renderer, &e->uniforms, sizeof(e->values), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT));
    if (light_count > native->properties.limits.maxStorageBufferRange / sizeof(maage_render_light)) return MAAGE_VULKAN_ERROR(MAAGE_STATUS_INVALID_ARGUMENT);
    MAAGE_VULKAN_TRY(maage_vulkan_host_buffer(renderer, &e->lights, light_count ? light_count * sizeof(maage_render_light) : sizeof(maage_render_light), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    if (!e->shadow.image || resolution != e->resolution || cascades != e->cascades) {
        if (e->shadow_framebuffer) vkDestroyFramebuffer(native->device, e->shadow_framebuffer, NULL);
        e->shadow_framebuffer = VK_NULL_HANDLE;
        maage_vulkan_image_destroy(native, &e->shadow);
        MAAGE_VULKAN_TRY(maage_vulkan_image_create(renderer, shadow_width, shadow_height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &e->shadow));
        MAAGE_VULKAN_TRY(maage_vulkan_effect_framebuffer(renderer, gpu->shadow_pass, e->shadow.view, shadow_width, shadow_height, &e->shadow_framebuffer));
        e->resolution = resolution;
        e->cascades = cascades;
        e->shadow_initialized = 0;
        maage_log_debug("maage.client.render", "Shader shadow slot: %u cascades, %u texels per cascade, %lu MiB shared budget.", cascades, resolution, budget);
    }
    if (!e->opaque_depth.image || width != e->width || height != e->height) {
        maage_vulkan_image_destroy(native, &e->opaque_depth);
        maage_vulkan_image_destroy(native, &e->opaque_color);
        MAAGE_VULKAN_TRY(maage_vulkan_image_create(renderer, width, height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &e->opaque_depth));
        MAAGE_VULKAN_TRY(maage_vulkan_image_create(renderer, width, height, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &e->opaque_color));
        e->width = width;
        e->height = height;
        e->initialized = 0;
    }
    if (!e->scatter.image || sw != e->scatter_width || sh != e->scatter_height) {
        if (e->scatter_framebuffer) vkDestroyFramebuffer(native->device, e->scatter_framebuffer, NULL);
        e->scatter_framebuffer = VK_NULL_HANDLE;
        maage_vulkan_image_destroy(native, &e->scatter);
        MAAGE_VULKAN_TRY(maage_vulkan_image_create(renderer, sw, sh, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &e->scatter));
        MAAGE_VULKAN_TRY(maage_vulkan_effect_framebuffer(renderer, gpu->scatter_pass, e->scatter.view, sw, sh, &e->scatter_framebuffer));
        e->scatter_width = sw;
        e->scatter_height = sh;
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue black = {{0}};
        maage_vulkan_effect_barrier(native, command, e->scatter.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(command, e->scatter.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        maage_vulkan_effect_barrier(native, command, e->scatter.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    if (!e->initialized) {
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkClearDepthStencilValue far_depth = {1.0f, 0};
        VkClearColorValue black = {{0}};
        maage_vulkan_effect_barrier(native, command, e->opaque_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearDepthStencilImage(command, e->opaque_depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_depth, 1, &range);
        maage_vulkan_effect_barrier(native, command, e->opaque_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        maage_vulkan_effect_barrier(native, command, e->opaque_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(command, e->opaque_color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        maage_vulkan_effect_barrier(native, command, e->opaque_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        e->initialized = 1;
    }
    if (!e->shadow_initialized) {
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkClearDepthStencilValue far_depth = {1.0f, 0};
        maage_vulkan_effect_barrier(native, command, e->shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearDepthStencilImage(command, e->shadow.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_depth, 1, &range);
        maage_vulkan_effect_barrier(native, command, e->shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        e->shadow_initialized = 1;
    }
    MAAGE_VULKAN_TRY(maage_render_effect_uniforms_prepare(plan, native->extent.width, native->extent.height, cascades, resolution, &e->values));
    memcpy(e->uniforms.mapped, &e->values, sizeof(e->values));
    if (light_count) memcpy(e->lights.mapped, plan->effect_frame.lights, light_count * sizeof(maage_render_light));
    MAAGE_VULKAN_TRY(maage_vulkan_flush(renderer, &e->uniforms));
    MAAGE_VULKAN_TRY(maage_vulkan_flush(renderer, &e->lights));
    {
        VkDescriptorBufferInfo buffers[2] = {{e->uniforms.buffer, 0, sizeof(e->values)}, {e->lights.buffer, 0, light_count ? light_count * sizeof(maage_render_light) : sizeof(maage_render_light)}};
        VkDescriptorImageInfo images[4] = {{gpu->nearest, e->shadow.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}, {gpu->nearest, e->opaque_depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {gpu->nearest, e->opaque_color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {gpu->nearest, e->scatter.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[6] = {{0}};
        for (unsigned int i = 0; i < 6; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = e->descriptor.descriptor;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                                                           : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (i < 2) writes[i].pBufferInfo = &buffers[i];
            else writes[i].pImageInfo = &images[i - 2];
        }
        vkUpdateDescriptorSets(native->device, 6, writes, 0, NULL);
    }
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu->layout, 2, 1, &e->descriptor.descriptor, 0, NULL);
    return MAAGE_VULKAN_OK;
}
static void maage_vulkan_effect_shadows(maage_renderer *renderer, VkCommandBuffer command, maage_vulkan_effects *e, const maage_render_frame_plan *plan) {
    maage_vulkan_renderer *native = renderer->platform;
    maage_vulkan_rendering *gpu = native->rendering;
    size_t mesh_count = plan->shadow_casters ? plan->shadow_caster_count : plan->mesh_count;
    const maage_render_mesh *const *meshes = plan->shadow_casters ? plan->shadow_casters : plan->meshes;
    if (!e->cascades) return;
    VkClearValue clear = {{{0}}};
    clear.depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = gpu->shadow_pass;
    begin.framebuffer = e->shadow_framebuffer;
    begin.renderArea.extent.width = e->resolution * 2;
    begin.renderArea.extent.height = e->resolution * (e->cascades > 2 ? 2 : 1);
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    for (unsigned int cascade = 0; cascade < e->cascades; ++cascade) {
        VkViewport viewport = {(float) ((cascade % 2) * e->resolution), (float) ((cascade / 2) * e->resolution), (float) e->resolution, (float) e->resolution, 0, 1};
        VkRect2D scissor = {{(int32_t) ((cascade % 2) * e->resolution), (int32_t) ((cascade / 2) * e->resolution)}, {e->resolution, e->resolution}};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        for (size_t index = 0; index < mesh_count + plan->instance_count; ++index) {
            const maage_render_instance *instance = index < mesh_count ? NULL : &plan->instances[index - mesh_count];
            const maage_render_mesh *mesh = instance ? instance->mesh : meshes[index];
            maage_vulkan_mesh *geometry = mesh->platform;
            maage_vulkan_image *texture = mesh->texture->platform;
            maage_render_uniforms push = {0};
            float model[16], cull[16];
            VkDeviceSize offset = 0;
            if (!mesh->geometry.solid_count) continue;
            for (unsigned int i = 0; i < 16; ++i)
                model[i] = instance ? instance->transform[i] : (i % 5 == 0 ? 1.0f : 0);
            for (unsigned int column = 0; column < 4; ++column)
                for (unsigned int row = 0; row < 4; ++row) {
                    cull[column * 4 + row] = 0;
                    for (unsigned int k = 0; k < 4; ++k)
                        cull[column * 4 + row] += e->values.shadows[cascade][k * 4 + row] * model[column * 4 + k];
                }
            if (!maage_render_effect_bounds_visible(mesh->geometry.bounds, cull)) continue;
            memcpy(push.matrix, e->values.shadows[cascade], sizeof(push.matrix));
            memcpy((unsigned char *) &push + sizeof(push.matrix), model, sizeof(model));
            maage_vulkan_bind(native, command, gpu->shadow_pipeline, texture->descriptor, &push);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu->layout, 1, 1, &geometry->surface_descriptor.descriptor, 0, NULL);
            vkCmdBindVertexBuffers(command, 0, 1, &geometry->vertices.buffer, &offset);
            maage_vulkan_indices(native, command, geometry->indices.buffer, 0, mesh->geometry.solid_count, maage_render_gpu_mesh_index_bytes(mesh));
        }
    }
    vkCmdEndRenderPass(command);
}
static void maage_vulkan_effect_opaque(maage_vulkan_renderer *native, VkCommandBuffer command, const maage_render_frame_plan *plan, unsigned int slot) {
    for (size_t index = 0; index < plan->mesh_count + plan->instance_count; ++index) {
        maage_render_mesh_cursor cursor = {0};
        maage_render_uniforms push;
        cursor.instance = index < plan->mesh_count ? NULL : &plan->instances[index - plan->mesh_count];
        const maage_render_mesh *mesh = cursor.instance ? cursor.instance->mesh : plan->meshes[index];
        maage_render_draw_uniforms(plan, 0, mesh, &cursor, &push);
        maage_vulkan_mesh_draw(native, command, mesh, MAAGE_RENDER_SOLID, slot, &push);
    }
}
static void maage_vulkan_effect_copy(maage_renderer *renderer, VkCommandBuffer command, maage_vulkan_frame_data *frame) {
    maage_vulkan_renderer *native = renderer->platform;
    maage_vulkan_effects *e = &frame->effects;
    for (unsigned int i = 0; i < 2; ++i) {
        VkImage source = i ? frame->depth.image : frame->world.image, destination = i ? e->opaque_depth.image : e->opaque_color.image;
        VkImageAspectFlags aspect = i ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageLayout attachment = i ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkImageLayout sampled = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkPipelineStageFlags stage = i ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkAccessFlags access = i ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkImageCopy region = {0};
        region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = aspect;
        region.srcSubresource.layerCount = region.dstSubresource.layerCount = 1;
        region.extent.width = e->width;
        region.extent.height = e->height;
        region.extent.depth = 1;
        maage_vulkan_effect_barrier(native, command, source, aspect, attachment, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, VK_PIPELINE_STAGE_TRANSFER_BIT, access, VK_ACCESS_TRANSFER_READ_BIT);
        maage_vulkan_effect_barrier(native, command, destination, aspect, sampled, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        maage_vulkan_effect_barrier(native, command, source, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, attachment, VK_PIPELINE_STAGE_TRANSFER_BIT, stage, VK_ACCESS_TRANSFER_READ_BIT, access);
        maage_vulkan_effect_barrier(native, command, destination, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, sampled, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
}
static void maage_vulkan_effect_scatter(maage_renderer *renderer, VkCommandBuffer command, maage_vulkan_frame_data *frame) {
    maage_vulkan_renderer *native = renderer->platform;
    maage_vulkan_rendering *gpu = native->rendering;
    maage_vulkan_effects *e = &frame->effects;
    if (e->values.quality[1] < 0.5f) return;
    VkClearValue clear = {{{0}}};
    VkRenderPassBeginInfo begin = {0};
    VkViewport viewport = {0, 0, (float) e->scatter_width, (float) e->scatter_height, 0, 1};
    VkRect2D scissor = {{0, 0}, {e->scatter_width, e->scatter_height}};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = gpu->scatter_pass;
    begin.framebuffer = e->scatter_framebuffer;
    begin.renderArea = scissor;
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    /* The completed previous use of this slot cannot overlap its next write. */
    maage_vulkan_effect_barrier(native, command, e->scatter.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    maage_vulkan_fullscreen(native, command, gpu->scatter_pipeline, &frame->world, 0, 0, 0);
    vkCmdEndRenderPass(command);
}
