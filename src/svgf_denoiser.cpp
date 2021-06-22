#include "svgf_denoiser.h"
#include "g_buffer.h"
#include "common_resources.h"
#include "utilities.h"
#include <macros.h>
#include <profiler.h>
#include <imgui.h>

SVGFDenoiser::SVGFDenoiser(std::weak_ptr<dw::vk::Backend> backend, CommonResources* common_resources, GBuffer* g_buffer, std::string name, uint32_t input_width, uint32_t input_height, uint32_t filter_iterations) :
    m_name(name), m_backend(backend), m_common_resources(common_resources), m_g_buffer(g_buffer), m_input_width(input_width), m_input_height(input_height), m_a_trous_filter_iterations(filter_iterations)
{
    auto vk_backend = backend.lock();
    auto extents    = vk_backend->swap_chain_extents();

    m_scale = float(extents.width) / float(m_input_width);

    create_reprojection_resources();
    create_filter_moments_resources();
    create_a_trous_filter_resources();
}

SVGFDenoiser::~SVGFDenoiser()
{
}

dw::vk::DescriptorSet::Ptr SVGFDenoiser::output_ds()
{
    return m_a_trous_read_ds[m_read_idx];
}

void SVGFDenoiser::denoise(dw::vk::CommandBuffer::Ptr cmd_buf, dw::vk::DescriptorSet::Ptr input)
{
    DW_SCOPED_SAMPLE("SVGF Denoiser", cmd_buf);

    clear_images(cmd_buf);
    reprojection(cmd_buf, input);
    if (m_use_moments_filtering)
        filter_moments(cmd_buf);
    a_trous_filter(cmd_buf);
}

void SVGFDenoiser::gui()
{
    ImGui::SliderInt("A-Trous Filter Radius", &m_a_trous_radius, 1, 2);
    ImGui::SliderInt("A-Trous Filter Iterations", &m_a_trous_filter_iterations, 1, 5);
    ImGui::SliderInt("A-Trous Filter Feedback Tap", &m_a_trous_feedback_iteration, 0, 4);
    ImGui::SliderFloat("Alpha", &m_alpha, 0.0f, 1.0f);
    ImGui::SliderFloat("Moments Alpha", &m_moments_alpha, 0.0f, 1.0f);
    ImGui::Checkbox("Use filter output for reprojection", &m_use_spatial_for_feedback);
    ImGui::Checkbox("Filter Moments", &m_use_moments_filtering);
}

void SVGFDenoiser::create_reprojection_resources()
{
    auto vk_backend = m_backend.lock();

    {
        dw::vk::DescriptorSetLayout::Desc desc;

        desc.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
        desc.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
        desc.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);

        m_reprojection_write_ds_layout = dw::vk::DescriptorSetLayout::create(vk_backend, desc);
    }

    {
        dw::vk::DescriptorSetLayout::Desc desc;

        desc.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
        desc.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
        desc.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);

        m_reprojection_read_ds_layout = dw::vk::DescriptorSetLayout::create(vk_backend, desc);
    }

    for (int i = 0; i < 2; i++)
    {
        m_reprojection_write_ds[i]      = vk_backend->allocate_descriptor_set(m_reprojection_write_ds_layout);
        m_reprojection_read_ds[i]       = vk_backend->allocate_descriptor_set(m_reprojection_read_ds_layout);
        m_reprojection_color_read_ds[i] = vk_backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);
    }

    m_prev_reprojection_read_ds = vk_backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);

    {
        dw::vk::PipelineLayout::Desc desc;

        desc.add_descriptor_set_layout(m_reprojection_write_ds_layout);
        desc.add_descriptor_set_layout(m_g_buffer->ds_layout());
        desc.add_descriptor_set_layout(m_g_buffer->ds_layout());
        desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);
        desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);
        desc.add_descriptor_set_layout(m_reprojection_read_ds_layout);

        desc.add_push_constant_range(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReprojectionPushConstants));

        m_reprojection_pipeline_layout = dw::vk::PipelineLayout::create(vk_backend, desc);

        dw::vk::ShaderModule::Ptr module = dw::vk::ShaderModule::create_from_file(vk_backend, "shaders/svgf_reprojection.comp.spv");

        dw::vk::ComputePipeline::Desc comp_desc;

        comp_desc.set_pipeline_layout(m_reprojection_pipeline_layout);
        comp_desc.set_shader_stage(module, "main");

        m_reprojection_pipeline = dw::vk::ComputePipeline::create(vk_backend, comp_desc);
    }

    for (int i = 0; i < 2; i++)
    {
        m_reprojection_image[i] = dw::vk::Image::create(vk_backend, VK_IMAGE_TYPE_2D, m_input_width, m_input_height, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
        m_reprojection_image[i]->set_name(m_name + " Reprojection Color");

        m_reprojection_view[i] = dw::vk::ImageView::create(vk_backend, m_reprojection_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
        m_reprojection_view[i]->set_name(m_name + " Reprojection Color");

        m_moments_image[i] = dw::vk::Image::create(vk_backend, VK_IMAGE_TYPE_2D, m_input_width, m_input_height, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
        m_moments_image[i]->set_name(m_name + " Reprojection Moments");

        m_moments_view[i] = dw::vk::ImageView::create(vk_backend, m_moments_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
        m_moments_view[i]->set_name(m_name + " Reprojection Moments");

        m_history_length_image[i] = dw::vk::Image::create(vk_backend, VK_IMAGE_TYPE_2D, m_input_width, m_input_height, 1, 1, 1, VK_FORMAT_R16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
        m_history_length_image[i]->set_name(m_name + " Reprojection History");

        m_history_length_view[i] = dw::vk::ImageView::create(vk_backend, m_history_length_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
        m_history_length_view[i]->set_name(m_name + " Reprojection History");
    }

    m_prev_reprojection_image = dw::vk::Image::create(vk_backend, VK_IMAGE_TYPE_2D, m_input_width, m_input_height, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
    m_prev_reprojection_image->set_name(m_name + " Previous Reprojection");

    m_prev_reprojection_view = dw::vk::ImageView::create(vk_backend, m_prev_reprojection_image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
    m_prev_reprojection_view->set_name(m_name + " Previous Reprojection");

    // Reprojection write
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(6);
        write_datas.reserve(6);

        for (int i = 0; i < 2; i++)
        {
            {
                VkDescriptorImageInfo storage_image_info;

                storage_image_info.sampler     = VK_NULL_HANDLE;
                storage_image_info.imageView   = m_reprojection_view[i]->handle();
                storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                image_infos.push_back(storage_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 0;
                write_data.dstSet          = m_reprojection_write_ds[i]->handle();

                write_datas.push_back(write_data);
            }

            {
                VkDescriptorImageInfo storage_image_info;

                storage_image_info.sampler     = VK_NULL_HANDLE;
                storage_image_info.imageView   = m_moments_view[i]->handle();
                storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                image_infos.push_back(storage_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 1;
                write_data.dstSet          = m_reprojection_write_ds[i]->handle();

                write_datas.push_back(write_data);
            }

            {
                VkDescriptorImageInfo storage_image_info;

                storage_image_info.sampler     = VK_NULL_HANDLE;
                storage_image_info.imageView   = m_history_length_view[i]->handle();
                storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                image_infos.push_back(storage_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 2;
                write_data.dstSet          = m_reprojection_write_ds[i]->handle();

                write_datas.push_back(write_data);
            }
        }

        vkUpdateDescriptorSets(vk_backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // Reprojection read
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(6);
        write_datas.reserve(6);

        for (int i = 0; i < 2; i++)
        {
            {
                VkDescriptorImageInfo sampler_image_info;

                sampler_image_info.sampler     = vk_backend->nearest_sampler()->handle();
                sampler_image_info.imageView   = m_reprojection_view[i]->handle();
                sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                image_infos.push_back(sampler_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 0;
                write_data.dstSet          = m_reprojection_read_ds[i]->handle();

                write_datas.push_back(write_data);
            }

            {
                VkDescriptorImageInfo sampler_image_info;

                sampler_image_info.sampler     = vk_backend->nearest_sampler()->handle();
                sampler_image_info.imageView   = m_moments_view[i]->handle();
                sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                image_infos.push_back(sampler_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 1;
                write_data.dstSet          = m_reprojection_read_ds[i]->handle();

                write_datas.push_back(write_data);
            }

            {
                VkDescriptorImageInfo sampler_image_info;

                sampler_image_info.sampler     = vk_backend->nearest_sampler()->handle();
                sampler_image_info.imageView   = m_history_length_view[i]->handle();
                sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                image_infos.push_back(sampler_image_info);

                DW_ZERO_MEMORY(write_data);

                write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write_data.descriptorCount = 1;
                write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write_data.pImageInfo      = &image_infos.back();
                write_data.dstBinding      = 2;
                write_data.dstSet          = m_reprojection_read_ds[i]->handle();

                write_datas.push_back(write_data);
            }
        }

        vkUpdateDescriptorSets(vk_backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // Reprojection Color Read
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(2);
        write_datas.reserve(2);

        for (int i = 0; i < 2; i++)
        {
            VkDescriptorImageInfo sampler_image_info;

            sampler_image_info.sampler     = vk_backend->nearest_sampler()->handle();
            sampler_image_info.imageView   = m_reprojection_view[i]->handle();
            sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            image_infos.push_back(sampler_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 0;
            write_data.dstSet          = m_reprojection_color_read_ds[i]->handle();

            write_datas.push_back(write_data);
        }

        vkUpdateDescriptorSets(vk_backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // Previous Reprojection read
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(1);
        write_datas.reserve(1);

        VkDescriptorImageInfo sampler_image_info;

        sampler_image_info.sampler     = vk_backend->nearest_sampler()->handle();
        sampler_image_info.imageView   = m_prev_reprojection_view->handle();
        sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        image_infos.push_back(sampler_image_info);

        DW_ZERO_MEMORY(write_data);

        write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_data.descriptorCount = 1;
        write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write_data.pImageInfo      = &image_infos.back();
        write_data.dstBinding      = 0;
        write_data.dstSet          = m_prev_reprojection_read_ds->handle();

        write_datas.push_back(write_data);

        vkUpdateDescriptorSets(vk_backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }
}

void SVGFDenoiser::create_filter_moments_resources()
{
    auto vk_backend = m_backend.lock();

    m_filter_moments_write_ds = vk_backend->allocate_descriptor_set(m_common_resources->storage_image_ds_layout);
    m_filter_moments_read_ds  = vk_backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);

    dw::vk::PipelineLayout::Desc desc;

    desc.add_descriptor_set_layout(m_common_resources->storage_image_ds_layout);
    desc.add_descriptor_set_layout(m_g_buffer->ds_layout());
    desc.add_descriptor_set_layout(m_reprojection_read_ds_layout);

    desc.add_push_constant_range(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FilterMomentsPushConstants));

    m_filter_moments_pipeline_layout = dw::vk::PipelineLayout::create(vk_backend, desc);

    dw::vk::ShaderModule::Ptr module = dw::vk::ShaderModule::create_from_file(vk_backend, "shaders/svgf_filter_moments.comp.spv");

    dw::vk::ComputePipeline::Desc comp_desc;

    comp_desc.set_pipeline_layout(m_filter_moments_pipeline_layout);
    comp_desc.set_shader_stage(module, "main");

    m_filter_moments_pipeline = dw::vk::ComputePipeline::create(vk_backend, comp_desc);

    m_filter_moments_image = dw::vk::Image::create(vk_backend, VK_IMAGE_TYPE_2D, m_input_width, m_input_height, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
    m_filter_moments_image->set_name(m_name + " Filter Moments");

    m_filter_moments_view = dw::vk::ImageView::create(vk_backend, m_filter_moments_image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
    m_filter_moments_view->set_name(m_name + " Filter Moments");

    // Filter Moments write
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(1);
        write_datas.reserve(1);

        VkDescriptorImageInfo storage_image_info;

        storage_image_info.sampler     = VK_NULL_HANDLE;
        storage_image_info.imageView   = m_filter_moments_view->handle();
        storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        image_infos.push_back(storage_image_info);

        DW_ZERO_MEMORY(write_data);

        write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_data.descriptorCount = 1;
        write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write_data.pImageInfo      = &image_infos.back();
        write_data.dstBinding      = 0;
        write_data.dstSet          = m_filter_moments_write_ds->handle();

        write_datas.push_back(write_data);

        vkUpdateDescriptorSets(vk_backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // Filter Moments read
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(1);
        write_datas.reserve(1);

        VkDescriptorImageInfo sampler_image_info;

        sampler_image_info.sampler     = vk_backend->nearest_sampler()->handle();
        sampler_image_info.imageView   = m_filter_moments_view->handle();
        sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        image_infos.push_back(sampler_image_info);

        DW_ZERO_MEMORY(write_data);

        write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_data.descriptorCount = 1;
        write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write_data.pImageInfo      = &image_infos.back();
        write_data.dstBinding      = 0;
        write_data.dstSet          = m_filter_moments_read_ds->handle();

        write_datas.push_back(write_data);

        vkUpdateDescriptorSets(vk_backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }
}

void SVGFDenoiser::create_a_trous_filter_resources()
{
    auto vk_backend = m_backend.lock();

    for (int i = 0; i < 2; i++)
    {
        m_a_trous_read_ds[i]  = vk_backend->allocate_descriptor_set(m_common_resources->combined_sampler_ds_layout);
        m_a_trous_write_ds[i] = vk_backend->allocate_descriptor_set(m_common_resources->storage_image_ds_layout);
    }

    dw::vk::PipelineLayout::Desc desc;

    desc.add_descriptor_set_layout(m_common_resources->storage_image_ds_layout);
    desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);
    desc.add_descriptor_set_layout(m_g_buffer->ds_layout());
    desc.add_descriptor_set_layout(m_reprojection_read_ds_layout);

    desc.add_push_constant_range(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ATrousFilterPushConstants));

    m_a_trous_filter_pipeline_layout = dw::vk::PipelineLayout::create(vk_backend, desc);

    dw::vk::ShaderModule::Ptr module = dw::vk::ShaderModule::create_from_file(vk_backend, "shaders/svgf_a_trous_filter.comp.spv");

    dw::vk::ComputePipeline::Desc comp_desc;

    comp_desc.set_pipeline_layout(m_a_trous_filter_pipeline_layout);
    comp_desc.set_shader_stage(module, "main");

    m_a_trous_filter_pipeline = dw::vk::ComputePipeline::create(vk_backend, comp_desc);

    for (int i = 0; i < 2; i++)
    {
        m_a_trous_image[i] = dw::vk::Image::create(vk_backend, VK_IMAGE_TYPE_2D, m_input_width, m_input_height, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT);
        m_a_trous_image[i]->set_name(m_name + " A-Trous Filter");

        m_a_trous_view[i] = dw::vk::ImageView::create(vk_backend, m_a_trous_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
        m_a_trous_view[i]->set_name(m_name + " A-Trous Filter");
    }

    // A-Trous write
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(2);
        write_datas.reserve(2);

        for (int i = 0; i < 2; i++)
        {
            VkDescriptorImageInfo storage_image_info;

            storage_image_info.sampler     = VK_NULL_HANDLE;
            storage_image_info.imageView   = m_a_trous_view[i]->handle();
            storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            image_infos.push_back(storage_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 0;
            write_data.dstSet          = m_a_trous_write_ds[i]->handle();

            write_datas.push_back(write_data);
        }

        vkUpdateDescriptorSets(vk_backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // A-Trous read
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(2);
        write_datas.reserve(2);

        for (int i = 0; i < 2; i++)
        {
            VkDescriptorImageInfo sampler_image_info;

            sampler_image_info.sampler     = vk_backend->nearest_sampler()->handle();
            sampler_image_info.imageView   = m_a_trous_view[i]->handle();
            sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            image_infos.push_back(sampler_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 0;
            write_data.dstSet          = m_a_trous_read_ds[i]->handle();

            write_datas.push_back(write_data);
        }

        vkUpdateDescriptorSets(vk_backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }
}

void SVGFDenoiser::clear_images(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    if (m_common_resources->first_frame)
    {
        VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkClearColorValue color;

        color.float32[0] = 0.0f;
        color.float32[1] = 0.0f;
        color.float32[2] = 0.0f;
        color.float32[3] = 0.0f;

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_prev_reprojection_image->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_moments_image[!m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_history_length_image[!m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_reprojection_image[!m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            subresource_range);

        vkCmdClearColorImage(cmd_buf->handle(), m_moments_image[!m_common_resources->ping_pong]->handle(), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &subresource_range);
        vkCmdClearColorImage(cmd_buf->handle(), m_history_length_image[!m_common_resources->ping_pong]->handle(), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &subresource_range);
        vkCmdClearColorImage(cmd_buf->handle(), m_reprojection_image[!m_common_resources->ping_pong]->handle(), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &subresource_range);
        vkCmdClearColorImage(cmd_buf->handle(), m_prev_reprojection_image->handle(), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_prev_reprojection_image->handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_moments_image[!m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_history_length_image[!m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_reprojection_image[!m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);
    }
}

void SVGFDenoiser::reprojection(dw::vk::CommandBuffer::Ptr cmd_buf, dw::vk::DescriptorSet::Ptr input)
{
    DW_SCOPED_SAMPLE(m_name + " SVGF Temporal Reprojection", cmd_buf);

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    {
        std::vector<VkMemoryBarrier> memory_barriers = {
            memory_barrier(VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
        };

        std::vector<VkImageMemoryBarrier> image_barriers = {
            image_memory_barrier(m_reprojection_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT),
            image_memory_barrier(m_moments_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT),
            image_memory_barrier(m_history_length_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
        };

        pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    const uint32_t NUM_THREADS = 32;

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_reprojection_pipeline->handle());

    ReprojectionPushConstants push_constants;

    push_constants.alpha         = m_alpha;
    push_constants.moments_alpha = m_moments_alpha;
    push_constants.g_buffer_mip  = m_scale == 1.0f ? 0 : 1;

    vkCmdPushConstants(cmd_buf->handle(), m_reprojection_pipeline_layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

    VkDescriptorSet descriptor_sets[] = {
        m_reprojection_write_ds[m_common_resources->ping_pong]->handle(),
        m_g_buffer->output_ds()->handle(),
        m_g_buffer->history_ds()->handle(),
        input->handle(),
        m_prev_reprojection_read_ds->handle(),
        m_reprojection_read_ds[!m_common_resources->ping_pong]->handle()
    };

    vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_reprojection_pipeline_layout->handle(), 0, 6, descriptor_sets, 0, nullptr);

    vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(m_input_width) / float(NUM_THREADS))), static_cast<uint32_t>(ceil(float(m_input_height) / float(NUM_THREADS))), 1);

    if (!m_use_spatial_for_feedback)
    {
        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_reprojection_image[m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_prev_reprojection_image->handle(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            subresource_range);

        VkImageCopy image_copy_region {};
        image_copy_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image_copy_region.srcSubresource.layerCount = 1;
        image_copy_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image_copy_region.dstSubresource.layerCount = 1;
        image_copy_region.extent.width              = m_input_width;
        image_copy_region.extent.height             = m_input_height;
        image_copy_region.extent.depth              = 1;

        // Issue the copy command
        vkCmdCopyImage(
            cmd_buf->handle(),
            m_reprojection_image[m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_prev_reprojection_image->handle(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &image_copy_region);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_reprojection_image[m_common_resources->ping_pong]->handle(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_prev_reprojection_image->handle(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);
    }

    {
        std::vector<VkMemoryBarrier> memory_barriers = {
            memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
        };

        std::vector<VkImageMemoryBarrier> image_barriers = {
            image_memory_barrier(m_reprojection_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresource_range, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            image_memory_barrier(m_moments_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresource_range, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            image_memory_barrier(m_history_length_image[m_common_resources->ping_pong], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresource_range, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
        };

        pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
}

void SVGFDenoiser::filter_moments(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE(m_name + " SVGF Filter Moments", cmd_buf);

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    {
        std::vector<VkMemoryBarrier> memory_barriers = {
            memory_barrier(VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
        };

        std::vector<VkImageMemoryBarrier> image_barriers = {
            image_memory_barrier(m_filter_moments_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT),
        };

        pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    const uint32_t NUM_THREADS = 32;

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_filter_moments_pipeline->handle());

    FilterMomentsPushConstants push_constants;

    push_constants.phi_color    = m_phi_color;
    push_constants.phi_normal   = m_phi_normal;
    push_constants.g_buffer_mip = m_scale == 1.0f ? 0 : 1;

    vkCmdPushConstants(cmd_buf->handle(), m_filter_moments_pipeline_layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

    VkDescriptorSet descriptor_sets[] = {
        m_filter_moments_write_ds->handle(),
        m_g_buffer->output_ds()->handle(),
        m_reprojection_read_ds[m_common_resources->ping_pong]->handle()
    };

    vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_filter_moments_pipeline_layout->handle(), 0, 3, descriptor_sets, 0, nullptr);

    vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(m_input_width) / float(NUM_THREADS))), static_cast<uint32_t>(ceil(float(m_input_height) / float(NUM_THREADS))), 1);

    {
        std::vector<VkMemoryBarrier> memory_barriers = {
            memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
        };

        std::vector<VkImageMemoryBarrier> image_barriers = {
            image_memory_barrier(m_filter_moments_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresource_range, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        };

        pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
}

void SVGFDenoiser::a_trous_filter(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE(m_name + " SVGF A-Trous Filter", cmd_buf);

    const uint32_t NUM_THREADS = 32;

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_a_trous_filter_pipeline->handle());

    bool    ping_pong = false;
    int32_t read_idx  = 0;
    int32_t write_idx = 1;

    for (int i = 0; i < m_a_trous_filter_iterations; i++)
    {
        read_idx  = (int32_t)ping_pong;
        write_idx = (int32_t)!ping_pong;

        if (i == 0)
        {
            std::vector<VkMemoryBarrier> memory_barriers = {
                memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
            };

            std::vector<VkImageMemoryBarrier> image_barriers = {
                image_memory_barrier(m_a_trous_image[write_idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
            };

            pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        else
        {
            std::vector<VkMemoryBarrier> memory_barriers = {
                memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
            };

            std::vector<VkImageMemoryBarrier> image_barriers = {
                image_memory_barrier(m_a_trous_image[read_idx], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresource_range, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
                image_memory_barrier(m_a_trous_image[write_idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
            };

            pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }

        ATrousFilterPushConstants push_constants;

        push_constants.radius       = m_a_trous_radius;
        push_constants.step_size    = 1 << i;
        push_constants.phi_color    = m_phi_color;
        push_constants.phi_normal   = m_phi_normal;
        push_constants.g_buffer_mip = m_scale == 1.0f ? 0 : 1;

        vkCmdPushConstants(cmd_buf->handle(), m_a_trous_filter_pipeline_layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

        VkDescriptorSet descriptor_sets[] = {
            m_a_trous_write_ds[write_idx]->handle(),
            i == 0 ? (m_use_moments_filtering ? m_filter_moments_read_ds->handle() : m_reprojection_color_read_ds[m_common_resources->ping_pong]->handle()) : m_a_trous_read_ds[read_idx]->handle(),
            m_g_buffer->history_ds()->handle(),
            m_reprojection_read_ds[m_common_resources->ping_pong]->handle()
        };

        vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_a_trous_filter_pipeline_layout->handle(), 0, 4, descriptor_sets, 0, nullptr);

        vkCmdDispatch(cmd_buf->handle(), static_cast<uint32_t>(ceil(float(m_input_width) / float(NUM_THREADS))), static_cast<uint32_t>(ceil(float(m_input_height) / float(NUM_THREADS))), 1);

        ping_pong = !ping_pong;

        if (m_use_spatial_for_feedback && m_a_trous_feedback_iteration == i)
        {
            dw::vk::utilities::set_image_layout(
                cmd_buf->handle(),
                m_a_trous_image[write_idx]->handle(),
                VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                subresource_range);

            dw::vk::utilities::set_image_layout(
                cmd_buf->handle(),
                m_prev_reprojection_image->handle(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                subresource_range);

            VkImageCopy image_copy_region {};
            image_copy_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            image_copy_region.srcSubresource.layerCount = 1;
            image_copy_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            image_copy_region.dstSubresource.layerCount = 1;
            image_copy_region.extent.width              = m_input_width;
            image_copy_region.extent.height             = m_input_height;
            image_copy_region.extent.depth              = 1;

            // Issue the copy command
            vkCmdCopyImage(
                cmd_buf->handle(),
                m_a_trous_image[write_idx]->handle(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_prev_reprojection_image->handle(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &image_copy_region);

            dw::vk::utilities::set_image_layout(
                cmd_buf->handle(),
                m_a_trous_image[write_idx]->handle(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL,
                subresource_range);

            dw::vk::utilities::set_image_layout(
                cmd_buf->handle(),
                m_prev_reprojection_image->handle(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                subresource_range);
        }
    }

    m_read_idx = write_idx;

    std::vector<VkMemoryBarrier> memory_barriers = {
        memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
    };

    std::vector<VkImageMemoryBarrier> image_barriers = {
        image_memory_barrier(m_a_trous_image[write_idx], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresource_range, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
    };

    pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}