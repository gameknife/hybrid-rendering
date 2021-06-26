#include "ddgi.h"
#include "utilities.h"
#include <stdexcept>
#include <logger.h>
#include <profiler.h>
#include <macros.h>
#include <imgui.h>
#include <gtc/quaternion.hpp>
#define _USE_MATH_DEFINES
#include <math.h>

// -----------------------------------------------------------------------------------------------------------------------------------

struct RayTracePushConstants
{
    DW_ALIGNED(16)
    glm::mat3 random_orientation;
    DW_ALIGNED(16)
    glm::vec3 grid_start_position;
    DW_ALIGNED(16)
    glm::vec3 grid_step;
    DW_ALIGNED(16)
    glm::ivec3 probe_counts;
    uint32_t   num_frames;
    uint32_t   rays_per_probe;
};

// -----------------------------------------------------------------------------------------------------------------------------------

struct ProbeUpdatePushConstants
{
    DW_ALIGNED(16)
    glm::vec3 grid_start_position;
    DW_ALIGNED(16)
    glm::vec3 grid_step;
    DW_ALIGNED(16)
    glm::ivec3 probe_counts;
    float      max_distance;
    float      depth_sharpness;
    float      hysteresis;
    int        probe_side_length;
    int        texture_width;
    int        texture_height;
    int        rays_per_probe;
    int        is_irradiance;
};

// -----------------------------------------------------------------------------------------------------------------------------------

struct VisualizeProbeGridPushConstants
{
    DW_ALIGNED(16)
    glm::vec3 grid_start_position;
    DW_ALIGNED(16)
    glm::vec3 grid_step;
    DW_ALIGNED(16)
    glm::ivec3 probe_counts;
    float      scale;
    int        probe_side_length;
    int        texture_width;
    int        texture_height;
};

// -----------------------------------------------------------------------------------------------------------------------------------

DDGI::DDGI(std::weak_ptr<dw::vk::Backend> backend, CommonResources* common_resources) :
    m_backend(backend), m_common_resources(common_resources)
{
    m_random_generator       = std::mt19937(m_random_device());
    m_random_distribution_zo = std::uniform_real_distribution<float>(0.0f, 1.0f);
    m_random_distribution_no = std::uniform_real_distribution<float>(-1.0f, 1.0f);

    load_sphere_mesh();
    create_descriptor_sets();
    create_pipelines();
}

// -----------------------------------------------------------------------------------------------------------------------------------

DDGI::~DDGI()
{
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::render(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE("DDGI", cmd_buf);

    // If the scene has changed re-initialize the probe grid
    if (m_last_scene_id != m_common_resources->current_scene->id())
        initialize_probe_grid();

    ray_trace(cmd_buf);
    probe_update(cmd_buf);

    m_first_frame = false;
    m_ping_pong   = !m_ping_pong;
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::render_probes(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    if (m_visualize_probe_grid.enabled)
    {
        DW_SCOPED_SAMPLE("DDGI Visualize Probe Grid", cmd_buf);

        auto vk_backend = m_backend.lock();

        const auto& submeshes = m_visualize_probe_grid.sphere_mesh->sub_meshes();

        vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_visualize_probe_grid.pipeline->handle());

        const uint32_t dynamic_offset = m_common_resources->ubo_size * vk_backend->current_frame_idx();

        VkDescriptorSet descriptor_sets[] = {
            m_common_resources->per_frame_ds->handle(),
            m_probe_grid.read_ds[static_cast<uint32_t>(!m_ping_pong)]->handle(),
        };

        vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_visualize_probe_grid.pipeline_layout->handle(), 0, 2, descriptor_sets, 1, &dynamic_offset);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd_buf->handle(), 0, 1, &m_visualize_probe_grid.sphere_mesh->vertex_buffer()->handle(), &offset);
        vkCmdBindIndexBuffer(cmd_buf->handle(), m_visualize_probe_grid.sphere_mesh->index_buffer()->handle(), 0, VK_INDEX_TYPE_UINT32);

        auto& submesh = submeshes[0];

        VisualizeProbeGridPushConstants push_constants;

        push_constants.grid_start_position = m_probe_grid.grid_start_position;
        push_constants.grid_step           = glm::vec3(m_probe_grid.probe_distance);
        push_constants.probe_counts        = m_probe_grid.probe_counts;
        push_constants.scale               = m_visualize_probe_grid.scale;
        push_constants.probe_side_length   = m_probe_grid.irradiance_oct_size;
        push_constants.texture_width       = m_probe_grid.irradiance_image[0]->width();
        push_constants.texture_height      = m_probe_grid.irradiance_image[0]->height();

        vkCmdPushConstants(cmd_buf->handle(), m_visualize_probe_grid.pipeline_layout->handle(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);

        uint32_t probe_count = m_probe_grid.probe_counts.x * m_probe_grid.probe_counts.y * m_probe_grid.probe_counts.z;

        // Issue draw call.
        vkCmdDrawIndexed(cmd_buf->handle(), submesh.index_count, probe_count, submesh.base_index, submesh.base_vertex, 0);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::gui()
{
    ImGui::Text("Grid Size: [%i, %i, %i]", m_probe_grid.probe_counts.x, m_probe_grid.probe_counts.y, m_probe_grid.probe_counts.z);
    ImGui::Text("Probe Count: %i", m_probe_grid.probe_counts.x * m_probe_grid.probe_counts.y * m_probe_grid.probe_counts.z);
    ImGui::Checkbox("Visualize Probe Grid", &m_visualize_probe_grid.enabled);
    ImGui::InputFloat("Scale", &m_visualize_probe_grid.scale);
    if (ImGui::InputInt("Rays Per Probe", &m_ray_trace.rays_per_probe))
        recreate_probe_grid_resources();
    if (ImGui::InputFloat("Probe Distance", &m_probe_grid.probe_distance))
        initialize_probe_grid();
    ImGui::InputFloat("Hysteresis", &m_probe_update.hysteresis);
    ImGui::InputFloat("Depth Sharpness", &m_probe_update.depth_sharpness);
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::load_sphere_mesh()
{
    auto vk_backend = m_backend.lock();

    m_visualize_probe_grid.sphere_mesh = dw::Mesh::load(vk_backend, "mesh/sphere.obj");

    if (!m_visualize_probe_grid.sphere_mesh)
    {
        DW_LOG_ERROR("Failed to load mesh");
        throw std::runtime_error("Failed to load sphere mesh");
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::initialize_probe_grid()
{
    // Get the min and max extents of the scene.
    glm::vec3 min_extents = m_common_resources->current_scene->min_extents();
    glm::vec3 max_extents = m_common_resources->current_scene->max_extents();

    // Compute the scene length.
    glm::vec3 scene_length = max_extents - min_extents;

    // Compute the number of probes along each axis.
    // Add 2 more probes to fully cover scene.
    m_probe_grid.probe_counts        = glm::ivec3(scene_length / m_probe_grid.probe_distance) + glm::ivec3(2);
    m_probe_grid.grid_start_position = min_extents;

    // Assign current scene ID
    m_last_scene_id = m_common_resources->current_scene->id();

    recreate_probe_grid_resources();
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::create_images()
{
    auto backend = m_backend.lock();

    uint32_t total_probes = m_probe_grid.probe_counts.x * m_probe_grid.probe_counts.y * m_probe_grid.probe_counts.z;

    // Ray Trace
    {
        m_ray_trace.radiance_image = dw::vk::Image::create(backend, VK_IMAGE_TYPE_2D, m_ray_trace.rays_per_probe, total_probes, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT);
        m_ray_trace.radiance_image->set_name("DDGI Ray Trace Radiance");

        m_ray_trace.radiance_view = dw::vk::ImageView::create(backend, m_ray_trace.radiance_image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
        m_ray_trace.radiance_view->set_name("DDGI Ray Trace Radiance");

        m_ray_trace.direction_depth_image = dw::vk::Image::create(backend, VK_IMAGE_TYPE_2D, m_ray_trace.rays_per_probe, total_probes, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT);
        m_ray_trace.direction_depth_image->set_name("DDGI Ray Trace Direction Depth");

        m_ray_trace.direction_depth_view = dw::vk::ImageView::create(backend, m_ray_trace.direction_depth_image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
        m_ray_trace.direction_depth_view->set_name("DDGI Ray Trace Direction Depth");
    }

    // Probe Grid
    {
        // 1-pixel of padding surrounding each probe, 1-pixel padding surrounding entire texture for alignment.
        const int irradiance_width  = (m_probe_grid.irradiance_oct_size + 2) * m_probe_grid.probe_counts.x * m_probe_grid.probe_counts.y + 2;
        const int irradiance_height = (m_probe_grid.irradiance_oct_size + 2) * m_probe_grid.probe_counts.z + 2;

        const int depth_width  = (m_probe_grid.depth_oct_size + 2) * m_probe_grid.probe_counts.x * m_probe_grid.probe_counts.y + 2;
        const int depth_height = (m_probe_grid.depth_oct_size + 2) * m_probe_grid.probe_counts.z + 2;

        for (int i = 0; i < 2; i++)
        {
            m_probe_grid.irradiance_image[i] = dw::vk::Image::create(backend, VK_IMAGE_TYPE_2D, irradiance_width, irradiance_height, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT);
            m_probe_grid.irradiance_image[i]->set_name("DDGI Irradiance Probe Grid " + std::to_string(i));

            m_probe_grid.irradiance_view[i] = dw::vk::ImageView::create(backend, m_probe_grid.irradiance_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
            m_probe_grid.irradiance_view[i]->set_name("DDGI Irradiance Probe Grid " + std::to_string(i));

            m_probe_grid.depth_image[i] = dw::vk::Image::create(backend, VK_IMAGE_TYPE_2D, depth_width, depth_height, 1, 1, 1, VK_FORMAT_R16G16_SFLOAT, VMA_MEMORY_USAGE_GPU_ONLY, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT);
            m_probe_grid.depth_image[i]->set_name("DDGI Depth Probe Grid " + std::to_string(i));

            m_probe_grid.depth_view[i] = dw::vk::ImageView::create(backend, m_probe_grid.depth_image[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
            m_probe_grid.depth_view[i]->set_name("DDGI Depth Probe Grid " + std::to_string(i));
        }
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::create_descriptor_sets()
{
    auto backend = m_backend.lock();

    // Ray Trace
    {
        dw::vk::DescriptorSetLayout::Desc desc;

        desc.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        desc.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

        m_ray_trace.write_ds_layout = dw::vk::DescriptorSetLayout::create(backend, desc);
    }

    {
        dw::vk::DescriptorSetLayout::Desc desc;

        desc.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        desc.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

        m_ray_trace.read_ds_layout = dw::vk::DescriptorSetLayout::create(backend, desc);
    }

    {
        m_ray_trace.write_ds = backend->allocate_descriptor_set(m_ray_trace.write_ds_layout);
        m_ray_trace.read_ds  = backend->allocate_descriptor_set(m_ray_trace.read_ds_layout);
    }

    // Probe Grid
    for (int i = 0; i < 2; i++)
    {
        m_probe_grid.write_ds[i] = backend->allocate_descriptor_set(m_ray_trace.write_ds_layout);
        m_probe_grid.read_ds[i]  = backend->allocate_descriptor_set(m_ray_trace.read_ds_layout);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::write_descriptor_sets()
{
    auto backend = m_backend.lock();

    // Ray Trace Write
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(2);
        write_datas.reserve(2);

        {
            VkDescriptorImageInfo storage_image_info;

            storage_image_info.sampler     = VK_NULL_HANDLE;
            storage_image_info.imageView   = m_ray_trace.radiance_view->handle();
            storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            image_infos.push_back(storage_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 0;
            write_data.dstSet          = m_ray_trace.write_ds->handle();

            write_datas.push_back(write_data);
        }

        {
            VkDescriptorImageInfo storage_image_info;

            storage_image_info.sampler     = VK_NULL_HANDLE;
            storage_image_info.imageView   = m_ray_trace.direction_depth_view->handle();
            storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            image_infos.push_back(storage_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 1;
            write_data.dstSet          = m_ray_trace.write_ds->handle();

            write_datas.push_back(write_data);
        }

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // Ray Trace Read
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(2);
        write_datas.reserve(2);

        {
            VkDescriptorImageInfo sampler_image_info;

            sampler_image_info.sampler     = backend->nearest_sampler()->handle();
            sampler_image_info.imageView   = m_ray_trace.radiance_view->handle();
            sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            image_infos.push_back(sampler_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 0;
            write_data.dstSet          = m_ray_trace.read_ds->handle();

            write_datas.push_back(write_data);
        }

        {
            VkDescriptorImageInfo sampler_image_info;

            sampler_image_info.sampler     = backend->nearest_sampler()->handle();
            sampler_image_info.imageView   = m_ray_trace.direction_depth_view->handle();
            sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            image_infos.push_back(sampler_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 1;
            write_data.dstSet          = m_ray_trace.read_ds->handle();

            write_datas.push_back(write_data);
        }

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // Probe Grid Write
    for (int i = 0; i < 2; i++)
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(2);
        write_datas.reserve(2);

        {
            VkDescriptorImageInfo storage_image_info;

            storage_image_info.sampler     = VK_NULL_HANDLE;
            storage_image_info.imageView   = m_probe_grid.irradiance_view[i]->handle();
            storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            image_infos.push_back(storage_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 0;
            write_data.dstSet          = m_probe_grid.write_ds[i]->handle();

            write_datas.push_back(write_data);
        }

        {
            VkDescriptorImageInfo storage_image_info;

            storage_image_info.sampler     = VK_NULL_HANDLE;
            storage_image_info.imageView   = m_probe_grid.depth_view[i]->handle();
            storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            image_infos.push_back(storage_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 1;
            write_data.dstSet          = m_probe_grid.write_ds[i]->handle();

            write_datas.push_back(write_data);
        }

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }

    // Probe Grid Read
    for (int i = 0; i < 2; i++)
    {
        std::vector<VkDescriptorImageInfo> image_infos;
        std::vector<VkWriteDescriptorSet>  write_datas;
        VkWriteDescriptorSet               write_data;

        image_infos.reserve(2);
        write_datas.reserve(2);

        {
            VkDescriptorImageInfo sampler_image_info;

            sampler_image_info.sampler     = backend->nearest_sampler()->handle();
            sampler_image_info.imageView   = m_probe_grid.irradiance_view[i]->handle();
            sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            image_infos.push_back(sampler_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 0;
            write_data.dstSet          = m_probe_grid.read_ds[i]->handle();

            write_datas.push_back(write_data);
        }

        {
            VkDescriptorImageInfo sampler_image_info;

            sampler_image_info.sampler     = backend->nearest_sampler()->handle();
            sampler_image_info.imageView   = m_probe_grid.depth_view[i]->handle();
            sampler_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            image_infos.push_back(sampler_image_info);

            DW_ZERO_MEMORY(write_data);

            write_data.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_data.descriptorCount = 1;
            write_data.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write_data.pImageInfo      = &image_infos.back();
            write_data.dstBinding      = 1;
            write_data.dstSet          = m_probe_grid.read_ds[i]->handle();

            write_datas.push_back(write_data);
        }

        vkUpdateDescriptorSets(backend->device(), write_datas.size(), write_datas.data(), 0, nullptr);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::create_pipelines()
{
    auto vk_backend = m_backend.lock();

    // Ray Trace
    {
        // ---------------------------------------------------------------------------
        // Create shader modules
        // ---------------------------------------------------------------------------

        dw::vk::ShaderModule::Ptr rgen  = dw::vk::ShaderModule::create_from_file(vk_backend, "shaders/gi_ray_trace.rgen.spv");
        dw::vk::ShaderModule::Ptr rchit = dw::vk::ShaderModule::create_from_file(vk_backend, "shaders/gi_ray_trace.rchit.spv");
        dw::vk::ShaderModule::Ptr rmiss = dw::vk::ShaderModule::create_from_file(vk_backend, "shaders/gi_ray_trace.rmiss.spv");

        dw::vk::ShaderBindingTable::Desc sbt_desc;

        sbt_desc.add_ray_gen_group(rgen, "main");
        sbt_desc.add_hit_group(rchit, "main");
        sbt_desc.add_miss_group(rmiss, "main");

        m_ray_trace.sbt = dw::vk::ShaderBindingTable::create(vk_backend, sbt_desc);

        dw::vk::RayTracingPipeline::Desc desc;

        desc.set_max_pipeline_ray_recursion_depth(1);
        desc.set_shader_binding_table(m_ray_trace.sbt);

        // ---------------------------------------------------------------------------
        // Create pipeline layout
        // ---------------------------------------------------------------------------

        dw::vk::PipelineLayout::Desc pl_desc;

        pl_desc.add_descriptor_set_layout(m_common_resources->pillars_scene->descriptor_set_layout());
        pl_desc.add_descriptor_set_layout(m_ray_trace.write_ds_layout);
        pl_desc.add_descriptor_set_layout(m_common_resources->per_frame_ds_layout);
        pl_desc.add_descriptor_set_layout(m_common_resources->combined_sampler_ds_layout);
        pl_desc.add_push_constant_range(VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 0, sizeof(RayTracePushConstants));

        m_ray_trace.pipeline_layout = dw::vk::PipelineLayout::create(vk_backend, pl_desc);

        desc.set_pipeline_layout(m_ray_trace.pipeline_layout);

        m_ray_trace.pipeline = dw::vk::RayTracingPipeline::create(vk_backend, desc);
    }

    // Probe Update
    {
        dw::vk::PipelineLayout::Desc desc;

        desc.add_descriptor_set_layout(m_ray_trace.write_ds_layout);
        desc.add_descriptor_set_layout(m_ray_trace.read_ds_layout);
        desc.add_descriptor_set_layout(m_ray_trace.read_ds_layout);

        desc.add_push_constant_range(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ProbeUpdatePushConstants));

        m_probe_update.pipeline_layout = dw::vk::PipelineLayout::create(vk_backend, desc);
        m_probe_update.pipeline_layout->set_name("Probe Update Pipeline Layout");

        dw::vk::ShaderModule::Ptr module = dw::vk::ShaderModule::create_from_file(vk_backend, "shaders/gi_probe_update.comp.spv");

        dw::vk::ComputePipeline::Desc comp_desc;

        comp_desc.set_pipeline_layout(m_probe_update.pipeline_layout);
        comp_desc.set_shader_stage(module, "main");

        m_probe_update.pipeline = dw::vk::ComputePipeline::create(vk_backend, comp_desc);
    }

    // Probe Visualization
    {
        // ---------------------------------------------------------------------------
        // Create shader modules
        // ---------------------------------------------------------------------------

        dw::vk::ShaderModule::Ptr vs = dw::vk::ShaderModule::create_from_file(vk_backend, "shaders/gi_probe_visualization.vert.spv");
        dw::vk::ShaderModule::Ptr fs = dw::vk::ShaderModule::create_from_file(vk_backend, "shaders/gi_probe_visualization.frag.spv");

        dw::vk::GraphicsPipeline::Desc pso_desc;

        pso_desc.add_shader_stage(VK_SHADER_STAGE_VERTEX_BIT, vs, "main")
            .add_shader_stage(VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main");

        // ---------------------------------------------------------------------------
        // Create vertex input state
        // ---------------------------------------------------------------------------

        pso_desc.set_vertex_input_state(m_common_resources->meshes[0]->vertex_input_state_desc());

        // ---------------------------------------------------------------------------
        // Create pipeline input assembly state
        // ---------------------------------------------------------------------------

        dw::vk::InputAssemblyStateDesc input_assembly_state_desc;

        input_assembly_state_desc.set_primitive_restart_enable(false)
            .set_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

        pso_desc.set_input_assembly_state(input_assembly_state_desc);

        // ---------------------------------------------------------------------------
        // Create viewport state
        // ---------------------------------------------------------------------------

        dw::vk::ViewportStateDesc vp_desc;

        vp_desc.add_viewport(0.0f, 0.0f, vk_backend->swap_chain_extents().width, vk_backend->swap_chain_extents().height, 0.0f, 1.0f)
            .add_scissor(0, 0, vk_backend->swap_chain_extents().width, vk_backend->swap_chain_extents().height);

        pso_desc.set_viewport_state(vp_desc);

        // ---------------------------------------------------------------------------
        // Create rasterization state
        // ---------------------------------------------------------------------------

        dw::vk::RasterizationStateDesc rs_state;

        rs_state.set_depth_clamp(VK_FALSE)
            .set_rasterizer_discard_enable(VK_FALSE)
            .set_polygon_mode(VK_POLYGON_MODE_FILL)
            .set_line_width(1.0f)
            .set_cull_mode(VK_CULL_MODE_BACK_BIT)
            .set_front_face(VK_FRONT_FACE_CLOCKWISE)
            .set_depth_bias(VK_FALSE);

        pso_desc.set_rasterization_state(rs_state);

        // ---------------------------------------------------------------------------
        // Create multisample state
        // ---------------------------------------------------------------------------

        dw::vk::MultisampleStateDesc ms_state;

        ms_state.set_sample_shading_enable(VK_FALSE)
            .set_rasterization_samples(VK_SAMPLE_COUNT_1_BIT);

        pso_desc.set_multisample_state(ms_state);

        // ---------------------------------------------------------------------------
        // Create depth stencil state
        // ---------------------------------------------------------------------------

        dw::vk::DepthStencilStateDesc ds_state;

        ds_state.set_depth_test_enable(VK_TRUE)
            .set_depth_write_enable(VK_TRUE)
            .set_depth_compare_op(VK_COMPARE_OP_LESS)
            .set_depth_bounds_test_enable(VK_FALSE)
            .set_stencil_test_enable(VK_FALSE);

        pso_desc.set_depth_stencil_state(ds_state);

        // ---------------------------------------------------------------------------
        // Create color blend state
        // ---------------------------------------------------------------------------

        dw::vk::ColorBlendAttachmentStateDesc blend_att_desc;

        blend_att_desc.set_color_write_mask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
            .set_blend_enable(VK_FALSE);

        dw::vk::ColorBlendStateDesc blend_state;

        blend_state.set_logic_op_enable(VK_FALSE)
            .set_logic_op(VK_LOGIC_OP_COPY)
            .set_blend_constants(0.0f, 0.0f, 0.0f, 0.0f)
            .add_attachment(blend_att_desc);

        pso_desc.set_color_blend_state(blend_state);

        // ---------------------------------------------------------------------------
        // Create pipeline layout
        // ---------------------------------------------------------------------------

        dw::vk::PipelineLayout::Desc pl_desc;

        pl_desc.add_descriptor_set_layout(m_common_resources->per_frame_ds_layout)
            .add_descriptor_set_layout(m_ray_trace.read_ds_layout)
            .add_push_constant_range(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VisualizeProbeGridPushConstants));

        m_visualize_probe_grid.pipeline_layout = dw::vk::PipelineLayout::create(vk_backend, pl_desc);

        pso_desc.set_pipeline_layout(m_visualize_probe_grid.pipeline_layout);

        // ---------------------------------------------------------------------------
        // Create dynamic state
        // ---------------------------------------------------------------------------

        pso_desc.add_dynamic_state(VK_DYNAMIC_STATE_VIEWPORT)
            .add_dynamic_state(VK_DYNAMIC_STATE_SCISSOR);

        // ---------------------------------------------------------------------------
        // Create pipeline
        // ---------------------------------------------------------------------------

        pso_desc.set_render_pass(m_common_resources->skybox_rp);

        m_visualize_probe_grid.pipeline = dw::vk::GraphicsPipeline::create(vk_backend, pso_desc);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::recreate_probe_grid_resources()
{
    auto backend = m_backend.lock();

    backend->wait_idle();

    m_first_frame = true;

    create_images();
    write_descriptor_sets();
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::ray_trace(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE("Ray Trace", cmd_buf);

    auto backend = m_backend.lock();

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    std::vector<VkMemoryBarrier> memory_barriers = {
        memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
    };

    std::vector<VkImageMemoryBarrier> image_barriers = {
        image_memory_barrier(m_ray_trace.radiance_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT),
        image_memory_barrier(m_ray_trace.direction_depth_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresource_range, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
    };

    pipeline_barrier(cmd_buf, memory_barriers, image_barriers, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_ray_trace.pipeline->handle());

    RayTracePushConstants push_constants;

    push_constants.random_orientation  = glm::mat3_cast(glm::angleAxis(m_random_distribution_zo(m_random_generator) * (float(M_PI) * 2.0f), glm::normalize(glm::vec3(m_random_distribution_no(m_random_generator), m_random_distribution_no(m_random_generator), m_random_distribution_no(m_random_generator)))));
    push_constants.grid_start_position = m_probe_grid.grid_start_position;
    push_constants.grid_step           = glm::vec3(m_probe_grid.probe_distance);
    push_constants.probe_counts        = m_probe_grid.probe_counts;
    push_constants.num_frames          = m_common_resources->num_frames;
    push_constants.rays_per_probe      = m_ray_trace.rays_per_probe;

    vkCmdPushConstants(cmd_buf->handle(), m_ray_trace.pipeline_layout->handle(), VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 0, sizeof(push_constants), &push_constants);

    const uint32_t dynamic_offset = m_common_resources->ubo_size * backend->current_frame_idx();

    VkDescriptorSet descriptor_sets[] = {
        m_common_resources->current_scene->descriptor_set()->handle(),
        m_ray_trace.write_ds->handle(),
        m_common_resources->per_frame_ds->handle(),
        m_common_resources->skybox_ds->handle()
    };

    vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_ray_trace.pipeline_layout->handle(), 0, 4, descriptor_sets, 1, &dynamic_offset);

    auto& rt_pipeline_props = backend->ray_tracing_pipeline_properties();

    VkDeviceSize group_size   = dw::vk::utilities::aligned_size(rt_pipeline_props.shaderGroupHandleSize, rt_pipeline_props.shaderGroupBaseAlignment);
    VkDeviceSize group_stride = group_size;

    const VkStridedDeviceAddressRegionKHR raygen_sbt   = { m_ray_trace.pipeline->shader_binding_table_buffer()->device_address(), group_stride, group_size };
    const VkStridedDeviceAddressRegionKHR miss_sbt     = { m_ray_trace.pipeline->shader_binding_table_buffer()->device_address() + m_ray_trace.sbt->miss_group_offset(), group_stride, group_size };
    const VkStridedDeviceAddressRegionKHR hit_sbt      = { m_ray_trace.pipeline->shader_binding_table_buffer()->device_address() + m_ray_trace.sbt->hit_group_offset(), group_stride, group_size };
    const VkStridedDeviceAddressRegionKHR callable_sbt = { 0, 0, 0 };

    uint32_t num_total_probes = m_probe_grid.probe_counts.x * m_probe_grid.probe_counts.y * m_probe_grid.probe_counts.z;

    vkCmdTraceRaysKHR(cmd_buf->handle(), &raygen_sbt, &miss_sbt, &hit_sbt, &callable_sbt, m_ray_trace.rays_per_probe, num_total_probes, 1);

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_ray_trace.radiance_image->handle(),
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        subresource_range);

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_ray_trace.direction_depth_image->handle(),
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        subresource_range);
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::probe_update(dw::vk::CommandBuffer::Ptr cmd_buf)
{
    DW_SCOPED_SAMPLE("Probe Update", cmd_buf);

    VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    uint32_t read_idx  = static_cast<uint32_t>(!m_ping_pong);
    uint32_t write_idx = static_cast<uint32_t>(m_ping_pong);

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_probe_grid.irradiance_image[write_idx]->handle(),
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        subresource_range);

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_probe_grid.depth_image[write_idx]->handle(),
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        subresource_range);

    if (m_first_frame)
    {
        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_probe_grid.irradiance_image[read_idx]->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);

        dw::vk::utilities::set_image_layout(
            cmd_buf->handle(),
            m_probe_grid.depth_image[read_idx]->handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            subresource_range);
    }

    vkCmdBindPipeline(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.pipeline->handle());

    probe_update(cmd_buf, true);
    probe_update(cmd_buf, false);

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_probe_grid.irradiance_image[write_idx]->handle(),
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        subresource_range);

    dw::vk::utilities::set_image_layout(
        cmd_buf->handle(),
        m_probe_grid.depth_image[write_idx]->handle(),
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        subresource_range);
}

// -----------------------------------------------------------------------------------------------------------------------------------

void DDGI::probe_update(dw::vk::CommandBuffer::Ptr cmd_buf, bool is_irradiance)
{
    DW_SCOPED_SAMPLE(is_irradiance ? "Irradiance" : "Depth", cmd_buf);

    ProbeUpdatePushConstants push_constants;

    push_constants.grid_start_position = m_probe_grid.grid_start_position;
    push_constants.grid_step           = glm::vec3(m_probe_grid.probe_distance);
    push_constants.probe_counts        = m_probe_grid.probe_counts;
    push_constants.max_distance        = m_probe_update.max_distance;
    push_constants.depth_sharpness     = m_probe_update.depth_sharpness;
    push_constants.hysteresis          = m_first_frame ? 0.0f : m_probe_update.hysteresis;
    push_constants.probe_side_length   = is_irradiance ? m_probe_grid.irradiance_oct_size : m_probe_grid.depth_oct_size;
    push_constants.texture_width       = is_irradiance ? m_probe_grid.irradiance_image[0]->width() : m_probe_grid.depth_image[0]->width();
    push_constants.texture_height      = is_irradiance ? m_probe_grid.irradiance_image[0]->height() : m_probe_grid.depth_image[0]->height();
    push_constants.rays_per_probe      = m_ray_trace.rays_per_probe;
    push_constants.is_irradiance       = (uint32_t)is_irradiance;

    vkCmdPushConstants(cmd_buf->handle(), m_probe_update.pipeline_layout->handle(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

    uint32_t read_idx  = static_cast<uint32_t>(!m_ping_pong);
    uint32_t write_idx = static_cast<uint32_t>(m_ping_pong);

    VkDescriptorSet descriptor_sets[] = {
        m_probe_grid.write_ds[write_idx]->handle(),
        m_probe_grid.read_ds[read_idx]->handle(),
        m_ray_trace.read_ds->handle()
    };

    vkCmdBindDescriptorSets(cmd_buf->handle(), VK_PIPELINE_BIND_POINT_COMPUTE, m_probe_update.pipeline_layout->handle(), 0, 3, descriptor_sets, 0, nullptr);

    const int NUM_THREADS_X = 32;
    const int NUM_THREADS_Y = 32;

    const uint32_t dispatch_x = static_cast<uint32_t>(ceil(float(is_irradiance ? m_probe_grid.irradiance_image[0]->width() : m_probe_grid.depth_image[0]->width()) / float(NUM_THREADS_X)));
    const uint32_t dispatch_y = static_cast<uint32_t>(ceil(float(is_irradiance ? m_probe_grid.irradiance_image[0]->height() : m_probe_grid.depth_image[0]->height()) / float(NUM_THREADS_Y)));

    vkCmdDispatch(cmd_buf->handle(), dispatch_x, dispatch_y, 1);
}

// -----------------------------------------------------------------------------------------------------------------------------------