#pragma once

#include "common_resources.h"

class DDGI
{
public:
    DDGI(std::weak_ptr<dw::vk::Backend> backend, CommonResources* common_resources);
    ~DDGI();

    void render(dw::vk::CommandBuffer::Ptr cmd_buf);
    void render_probes(dw::vk::CommandBuffer::Ptr cmd_buf);
    void gui();

    inline void  set_probe_distance(float value) { m_probe_distance = value; }
    inline void  set_probe_visualization_scale(float value) { m_visualize_probe_grid.scale = value; }
    inline float probe_distance() { return m_probe_distance; }
    inline float probe_visualization_scale() { return m_visualize_probe_grid.scale; }

private:
    void load_sphere_mesh();
    void initialize_probe_grid();
    void create_images();
    void create_descriptor_sets();
    void write_descriptor_sets();
    void create_pipelines();
    void recreate_probe_grid_resources();
    void ray_trace(dw::vk::CommandBuffer::Ptr cmd_buf);

private:
    struct RayTrace
    {
        int32_t                         rays_per_probe = 64;
        dw::vk::DescriptorSet::Ptr      write_ds;
        dw::vk::DescriptorSet::Ptr      read_ds;
        dw::vk::DescriptorSetLayout::Ptr write_ds_layout;
        dw::vk::DescriptorSetLayout::Ptr read_ds_layout;
        dw::vk::RayTracingPipeline::Ptr pipeline;
        dw::vk::PipelineLayout::Ptr     pipeline_layout;
        dw::vk::Image::Ptr              radiance_image;
        dw::vk::Image::Ptr              direction_depth_image;
        dw::vk::ImageView::Ptr          radiance_view;
        dw::vk::ImageView::Ptr          direction_depth_view;
        dw::vk::ShaderBindingTable::Ptr sbt;
    };

    struct VisualizeProbeGrid
    {
        bool                          enabled = false;
        float                         scale = 1.0f;
        dw::Mesh::Ptr                 sphere_mesh;
        dw::vk::GraphicsPipeline::Ptr pipeline;
        dw::vk::PipelineLayout::Ptr   pipeline_layout;
    };

    uint32_t                       m_last_scene_id = UINT32_MAX;
    std::weak_ptr<dw::vk::Backend> m_backend;
    CommonResources*               m_common_resources;
    float                          m_probe_distance = 1.0f;
    glm::vec3                      m_grid_start_position;
    glm::ivec3                     m_probe_counts;
    RayTrace                       m_ray_trace;
    VisualizeProbeGrid             m_visualize_probe_grid;
};