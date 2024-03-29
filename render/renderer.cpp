#include "renderer.h"
#include <algorithm>
#include <algorithm> // std::fill
#include <cmath>
#include <functional>
#include <glm/common.hpp>
#include <glm/gtx/component_wise.hpp>
#include <glm/gtx/string_cast.hpp>
#include <iostream>
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>
#include <tuple>

namespace render {

// The renderer is passed a pointer to the volume, gradinet volume, camera and an initial renderConfig.
// The camera being pointed to may change each frame (when the user interacts). When the renderConfig
// changes the setConfig function is called with the updated render config. This gives the Renderer an
// opportunity to resize the framebuffer.
Renderer::Renderer(
    const volume::Volume* pVolume,
    const volume::GradientVolume* pGradientVolume,
    const render::RayTraceCamera* pCamera,
    const RenderConfig& initialConfig)
    : m_pVolume(pVolume)
    , m_pGradientVolume(pGradientVolume)
    , m_pCamera(pCamera)
    , m_config(initialConfig)
{
    resizeImage(initialConfig.renderResolution);
}

// Set a new render config if the user changed the settings.
void Renderer::setConfig(const RenderConfig& config)
{
    if (config.renderResolution != m_config.renderResolution)
        resizeImage(config.renderResolution);

    m_config = config;
}

// Resize the framebuffer and fill it with black pixels.
void Renderer::resizeImage(const glm::ivec2& resolution)
{
    m_frameBuffer.resize(size_t(resolution.x) * size_t(resolution.y), glm::vec4(0.0f));
}

// Clear the framebuffer by setting all pixels to black.
void Renderer::resetImage()
{
    std::fill(std::begin(m_frameBuffer), std::end(m_frameBuffer), glm::vec4(0.0f));
}

// Return a VIEW into the framebuffer. This view is merely a reference to the m_frameBuffer member variable.
// This does NOT make a copy of the framebuffer.
gsl::span<const glm::vec4> Renderer::frameBuffer() const
{
    return m_frameBuffer;
}

// Main render function. It computes an image according to the current renderMode.
// Multithreading is enabled in Release/RelWithDebInfo modes. In Debug mode multithreading is disabled to make debugging easier.
void Renderer::render()
{
    resetImage();

    static constexpr float sampleStep = 1.0f;
    const glm::vec3 planeNormal = -glm::normalize(m_pCamera->forward());
    const glm::vec3 volumeCenter = glm::vec3(m_pVolume->dims()) / 2.0f;
    const Bounds bounds { glm::vec3(0.0f), glm::vec3(m_pVolume->dims() - glm::ivec3(1)) };

    // 0 = sequential (single-core), 1 = TBB (multi-core)
#ifdef NDEBUG
    // If NOT in debug mode then enable parallelism using the TBB library (Intel Threaded Building Blocks).
#define PARALLELISM 1
#else
    // Disable multithreading in debug mode.
#define PARALLELISM 0
#endif

#if PARALLELISM == 0
    // Regular (single threaded) for loops.
    for (int x = 0; x < m_config.renderResolution.x; x++) {
        for (int y = 0; y < m_config.renderResolution.y; y++) {
#else
    // Parallel for loop (in 2 dimensions) that subdivides the screen into tiles.
    const tbb::blocked_range2d<int> screenRange { 0, m_config.renderResolution.y, 0, m_config.renderResolution.x };
        tbb::parallel_for(screenRange, [&](tbb::blocked_range2d<int> localRange) {
        // Loop over the pixels in a tile. This function is called on multiple threads at the same time.
        for (int y = std::begin(localRange.rows()); y != std::end(localRange.rows()); y++) {
            for (int x = std::begin(localRange.cols()); x != std::end(localRange.cols()); x++) {
#endif
            // Compute a ray for the current pixel.
            const glm::vec2 pixelPos = glm::vec2(x, y) / glm::vec2(m_config.renderResolution);
            Ray ray = m_pCamera->generateRay(pixelPos * 2.0f - 1.0f);

            // Compute where the ray enters and exists the volume.
            // If the ray misses the volume then we continue to the next pixel.
            if (!instersectRayVolumeBounds(ray, bounds))
                continue;

            // Get a color for the current pixel according to the current render mode.
            glm::vec4 color {};
            switch (m_config.renderMode) {
            case RenderMode::RenderSlicer: {
                color = traceRaySlice(ray, volumeCenter, planeNormal);
                break;
            }
            case RenderMode::RenderMIP: {
                color = traceRayMIP(ray, sampleStep);
                break;
            }
            case RenderMode::RenderComposite: {
                color = traceRayComposite(ray, sampleStep);
                break;
            }
            case RenderMode::RenderIso: {
                color = traceRayISO(ray, sampleStep);
                break;
            }
            case RenderMode::RenderTF2D: {
                color = traceRayTF2D(ray, sampleStep);
                break;
            }
            };
            // Write the resulting color to the screen.
            fillColor(x, y, color);

#if PARALLELISM == 1
        }
    }
});
#else
            }
        }
#endif
}

// ======= DO NOT MODIFY THIS FUNCTION ========
// This function generates a view alongside a plane perpendicular to the camera through the center of the volume
//  using the slicing technique.
glm::vec4 Renderer::traceRaySlice(const Ray& ray, const glm::vec3& volumeCenter, const glm::vec3& planeNormal) const
{
    const float t = glm::dot(volumeCenter - ray.origin, planeNormal) / glm::dot(ray.direction, planeNormal);
    const glm::vec3 samplePos = ray.origin + ray.direction * t;
    const float val = m_pVolume->getVoxelInterpolate(samplePos);
    return glm::vec4(glm::vec3(std::max(val / m_pVolume->maximum(), 0.0f)), 1.f);
}

// ======= DO NOT MODIFY THIS FUNCTION ========
// Function that implements maximum-intensity-projection (MIP) raycasting.
// It returns the color assigned to a ray/pixel given it's origin, direction and the distances
// at which it enters/exits the volume (ray.tmin & ray.tmax respectively).
// The ray must be sampled with a distance defined by the sampleStep
glm::vec4 Renderer::traceRayMIP(const Ray& ray, float sampleStep) const
{
    float maxVal = 0.0f;

    // Incrementing samplePos directly instead of recomputing it each frame gives a measureable speed-up.
    glm::vec3 samplePos = ray.origin + ray.tmin * ray.direction;
    const glm::vec3 increment = sampleStep * ray.direction;
    for (float t = ray.tmin; t <= ray.tmax; t += sampleStep, samplePos += increment) {
        const float val = m_pVolume->getVoxelInterpolate(samplePos);
        maxVal = std::max(val, maxVal);
    }

    // Normalize the result to a range of [0 to mpVolume->maximum()].
    return glm::vec4(glm::vec3(maxVal) / m_pVolume->maximum(), 1.0f);
}

// ======= TODO: IMPLEMENT ========
// This function should find the position where the ray intersects with the volume's isosurface.
// If volume shading is DISABLED then simply return the isoColor.
// If volume shading is ENABLED then return the phong-shaded color at that location using the local gradient (from m_pGradientVolume).
//   Use the camera position (m_pCamera->position()) as the light position.
// Use the bisectionAccuracy function (to be implemented) to get a more precise isosurface location between two steps.
glm::vec4 Renderer::traceRayISO(const Ray& ray, float sampleStep) const
{
    glm::vec3 lightDirection = m_pCamera->position();
    glm::vec3 isoColor { 0.8f, 0.8f, 0.2f };

    // Prepare toon shading bins pair<cutoff range endpoint, assigned value>
    std::vector<std::pair<float, float>> diffuseBins = {
        std::make_pair(0.2f, 0.2f),
        std::make_pair(0.5f, 0.5f),
        std::make_pair(0.7f, 1.0f),
    };
    std::vector<std::pair<float, float>> specularBins = {
        std::make_pair(0.1f, 0.5f),
        std::make_pair(0.5f, 1.0f)
    };
    float t_previous = ray.tmin;
    for (float t = ray.tmin; t <= ray.tmax; t += sampleStep) {
        glm::vec3 samplePos = ray.origin + t * ray.direction;
        const float val = m_pVolume->getVoxelInterpolate(samplePos);
        if (val >= m_config.isoValue) {
            t = bisectionAccuracy(ray, t_previous, t, m_config.isoValue);
            samplePos = ray.origin + t * ray.direction;
            switch (m_config.shadingMode) {
            case ShadingMode::Phong: {
                return glm::vec4(
                    computePhongShading(
                        isoColor,
                        m_pGradientVolume->getGradientVoxel(samplePos),
                        lightDirection,
                        ray.direction),
                    1.0f);
            }
            case ShadingMode::Toon: {
                return glm::vec4(
                    computeToonShading(
                        isoColor,
                        m_pGradientVolume->getGradientVoxel(samplePos),
                        lightDirection,
                        ray.direction,
                        diffuseBins,
                        specularBins),
                    1.0f);
            }
            }
        }
        t_previous = t;
    }
    return glm::vec4(glm::vec3 { 0.0f, 0.0f, 0.0f }, 0.0f);
}

// ======= TODO: IMPLEMENT ========
// Given that the iso value lies somewhere between t0 and t1, find a t for which the value
// closely matches the iso value (less than 0.01 difference). Add a limit to the number of
// iterations such that it does not get stuck in degerate cases.
float Renderer::bisectionAccuracy(const Ray& ray, float t0, float t1, float isoValue) const
{
    glm::vec3 pos0 = ray.origin + t0 * ray.direction;
    glm::vec3 pos1 = ray.origin + t1 * ray.direction;
    float iso0 = m_pVolume->getVoxelInterpolate(pos0);
    float iso1 = m_pVolume->getVoxelInterpolate(pos1);

    int iter = 0;
    float iso_mid = iso1;
    float t_mid = t1;
    while (iter < m_config.maxIterations && abs(isoValue - iso_mid) >= 0.01) {
        // Get the difference to the previous value
        float diffPrev = iso1 - iso0;
        // Get the difference to the target value
        float diffIso = iso1 - isoValue;
        // Get the scale based on these differences
        float scale = diffIso / diffPrev;
        // Get the difference to the target value
        t_mid = t0 + (t1 - t0) * scale;
        // Get iso value at mid point
        glm::vec3 samplePos = ray.origin + t_mid * ray.direction;
        iso_mid = m_pVolume->getVoxelInterpolate(samplePos);

        // Set new bounds for next iteration
        if (iso_mid > isoValue) {
            t1 = t_mid;
            iso1 = iso_mid;
        } else {
            t0 = t_mid;
            iso0 = iso_mid;
        }
        iter++;
    }
    // Get the position between both steps given the scale
    return t_mid;
}

// ======= TODO: IMPLEMENT ========
// In this function, implement 1D transfer function raycasting.
// Use getTFValue to compute the color for a given volume value according to the 1D transfer function.
glm::vec4 Renderer::traceRayComposite(const Ray& ray, float sampleStep) const
{

    // Initialization of the colors as floating point values
    double r, g, b;
    r = g = b = 0.0;
    double alpha = 0.0;
    double opacity = 0;
    glm::vec4 sampleColor;
    glm::vec3 lightDirection = m_pCamera->position();

    // Incrementing samplePos directly instead of recomputing it each frame gives a measureable speed-up.
    glm::vec3 samplePos = ray.origin + ray.tmin * ray.direction;
    const glm::vec3 increment = sampleStep * ray.direction;
    for (float t = ray.tmin; t <= ray.tmax; t += sampleStep, samplePos += increment) {
        const float val = m_pVolume->getVoxelInterpolate(samplePos);
        sampleColor = getTFValue(val);
        opacity = (1 - alpha) * sampleColor.a;
        if (opacity > 0) {
            if (m_config.volumeShading) {
                sampleColor = glm::vec4(computePhongShading(sampleColor, m_pGradientVolume->getGradientVoxel(samplePos), lightDirection, ray.direction), sampleColor.a);
            }
            // calculating ci
            r += opacity * sampleColor.r;
            g += opacity * sampleColor.g;
            b += opacity * sampleColor.b;
            alpha += opacity;
        }
    }
    //return glm::vec4(glm::vec3(maxVal) / m_pVolume->maximum(), 1.0f);
    return glm::vec4(r, g, b, alpha);
}

// ======= TODO: IMPLEMENT ========
// In this function, implement 2D transfer function raycasting.
// Use the getTF2DOpacity function that you implemented to compute the opacity according to the 2D transfer function.
glm::vec4 Renderer::traceRayTF2D(const Ray& ray, float sampleStep) const
{
    // Initialization of the colors as floating point values
    double r, g, b;
    r = g = b = 0.0;
    double alpha = 0.0;
    double opacity = 0;
    glm::vec4 colorAux;
    glm::vec3 lightDirection = m_pCamera->position();

    // Incrementing samplePos directly instead of recomputing it each frame gives a measureable speed-up.
    glm::vec3 samplePos = ray.origin + ray.tmin * ray.direction;
    const glm::vec3 increment = sampleStep * ray.direction;
    for (float t = ray.tmin; t <= ray.tmax; t += sampleStep, samplePos += increment) {
        const float val = m_pVolume->getVoxelInterpolate(samplePos);

        // 2D transfer function
        opacity = (1 - alpha)
            * getTF2DOpacity(val, (m_pGradientVolume->getGradientVoxel(samplePos).magnitude));
        if (opacity > 0) {
            colorAux = m_config.TF2DColor;
            if (m_config.volumeShading) {
                colorAux = glm::vec4(computePhongShading(colorAux, m_pGradientVolume->getGradientVoxel(samplePos), lightDirection, ray.direction), colorAux.a);
            }
            // calculating ci
            r += opacity * colorAux.r;
            g += opacity * colorAux.g;
            b += opacity * colorAux.b;
            alpha += opacity;
        }
    }
    //return glm::vec4(glm::vec3(maxVal) / m_pVolume->maximum(), 1.0f);
    return glm::vec4(r, g, b, alpha);
    //return glm::vec4(0.0f);
}

// ======= TODO: IMPLEMENT ========
// Compute Phong Shading given the voxel color (material color), the gradient, the light vector and view vector.
// You can find out more about the Phong shading model at:
// https://en.wikipedia.org/wiki/Phong_reflection_model
//
// Use the given color for the ambient/specular/diffuse (you are allowed to scale these constants by a scalar value).
// You are free to choose any specular power that you'd like.
glm::vec3 Renderer::computeToonShading(const glm::vec3& color, const volume::GradientVoxel& gradient, const glm::vec3& L, const glm::vec3& V,
    std::vector<std::pair<float, float>> diffuseBins, std::vector<std::pair<float, float>> specularBins)
{
    // Define phong components
    float k_a = 0.3f; // Ambient
    float k_d = 0.4f; // Diffusion
    float k_s = 0.3f; // Specular
    float alpha = 20.0f; // Shininess (lower = wide and dimm, higher = small and bright)
    glm::vec3 nGradient = gradient.dir / gradient.magnitude;
    glm::vec3 nLight = glm::normalize(-1.0f * L);
    float reflectDot = glm::dot(nLight, nGradient);
    glm::vec3 reflectVector = nLight - 2 * reflectDot * nGradient;
    float specularIntensity = pow(glm::dot(reflectVector, glm::normalize(V)), alpha);
    // Calculate the weights
    float sWeight = std::max(k_s * specularIntensity, 0.0f);
    for (const std::pair<float, float>& bin : specularBins) {
        // Check if bin should apply
        if (bin.first < specularIntensity) {
            // Use bin value to set the specular weight
            sWeight = k_s * bin.second;
        }
    }
    float dWeight = std::max(k_d * reflectDot, 0.0f);
    for (const std::pair<float, float>& bin : diffuseBins) {
        // Check if bin should apply
        if (bin.first < reflectDot) {
            // Use bin value to set the diffusion weight
            dWeight = k_d * bin.second;
        }
    }

    // Add weights and set the final color
    float weight = k_a + dWeight + sWeight;
    return color * weight;
}

// ======= TODO: IMPLEMENT ========
// Compute Phong Shading given the voxel color (material color), the gradient, the light vector and view vector.
// You can find out more about the Phong shading model at:
// https://en.wikipedia.org/wiki/Phong_reflection_model
//
// Use the given color for the ambient/specular/diffuse (you are allowed to scale these constants by a scalar value).
// You are free to choose any specular power that you'd like.
glm::vec3 Renderer::computePhongShading(const glm::vec3& color, const volume::GradientVoxel& gradient, const glm::vec3& L, const glm::vec3& V)
{
    // Define phong components
    float k_a = 0.1f; // Ambient
    float k_d = 0.7f; // Diffusion
    float k_s = 0.2f; // Specular
    float alpha = 100.0f; // Shininess (lower = wide and dimm, higher = small and bright)
    glm::vec3 nGradient = gradient.dir / gradient.magnitude;
    glm::vec3 nLight = glm::normalize(-1.0f * L);
    float reflectDot = glm::dot(nLight, nGradient);
    glm::vec3 reflectVector = nLight - 2 * reflectDot * nGradient;
    float specularIntensity = pow(glm::dot(reflectVector, glm::normalize(V)), alpha);
    // Calculate the weights
    float sWeight = std::max(k_s * specularIntensity, 0.0f);
    float dWeight = std::max(k_d * reflectDot, 0.0f);
    if (std::isnan(sWeight)) {
        sWeight = 0.0f;
    }
    if (std::isnan(dWeight)) {
        dWeight = 0.0f;
    }
    // Add weights and set the final color
    float weight = k_a + dWeight + sWeight;
    return color * weight;
}

// ======= DO NOT MODIFY THIS FUNCTION ========
// Looks up the color+opacity corresponding to the given volume value from the 1D tranfer function LUT (m_config.tfColorMap).
// The value will initially range from (m_config.tfColorMapIndexStart) to (m_config.tfColorMapIndexStart + m_config.tfColorMapIndexRange) .
glm::vec4 Renderer::getTFValue(float val) const
{
    // Map value from [m_config.tfColorMapIndexStart, m_config.tfColorMapIndexStart + m_config.tfColorMapIndexRange) to [0, 1) .
    const float range01 = (val - m_config.tfColorMapIndexStart) / m_config.tfColorMapIndexRange;
    const size_t i = std::min(static_cast<size_t>(range01 * static_cast<float>(m_config.tfColorMap.size())), m_config.tfColorMap.size() - 1);
    return m_config.tfColorMap[i];
}

// ======= TODO: IMPLEMENT ========
// This function should return an opacity value for the given intensity and gradient according to the 2D transfer function.
// Calculate whether the values are within the radius/intensity triangle defined in the 2D transfer function widget.
// If so: return a tent weighting as described in the assignment
// Otherwise: return 0.0f
//
// The 2D transfer function settings can be accessed through m_config.TF2DIntensity and m_config.TF2DRadius.
float Renderer::getTF2DOpacity(float intensity, float gradientMagnitude) const
{

    float material_value = m_config.TF2DIntensity;
    float material_r = m_config.TF2DRadius;
    float opacity = 0.0;
    // System.err.println(gradMagnitude);
    // Inside Triangle
    // detection happens using a shifted modulus function
    // y = (max_grad /rad) | x - base | -> defines triangle lines in our 2D plot
    float slope = (m_pGradientVolume->maxMagnitude() / material_r);
    float input = (intensity - material_value);
    // defining line definition || input
    if (intensity - material_value < 0.0) {
        input = -input;
    }

    // area inside triangle
    if (gradientMagnitude >= slope * input) {
        // weird interpolation error
        // We want to interpolate from apex to edge ( input to border at input in x
        // direction)
        // if y = a(x -b) - > x = y/a +b

        float factor = (float)(input / (gradientMagnitude / slope));
        float interp_val = (1 - factor) * 1 + factor * 0;
        opacity = m_config.TF2DColor.a * interp_val;
        // System.err.println("My x (o to" + material_value + ")" + "," + input);
        // System.err.println("My factor " + input/ (input + material_value));
        // System.err.println("My value " + opacity);
    }
    return opacity;
}

// This function computes if a ray intersects with the axis-aligned bounding box around the volume.
// If the ray intersects then tmin/tmax are set to the distance at which the ray hits/exists the
// volume and true is returned. If the ray misses the volume the the function returns false.
//
// If you are interested you can learn about it at.
// https://www.scratchapixel.com/lessons/3d-basic-rendering/minimal-ray-tracer-rendering-simple-shapes/ray-box-intersection
bool Renderer::instersectRayVolumeBounds(Ray& ray, const Bounds& bounds) const
{
    const glm::vec3 invDir = 1.0f / ray.direction;
    const glm::bvec3 sign = glm::lessThan(invDir, glm::vec3(0.0f));

    float tmin = (bounds.lowerUpper[sign[0]].x - ray.origin.x) * invDir.x;
    float tmax = (bounds.lowerUpper[!sign[0]].x - ray.origin.x) * invDir.x;
    const float tymin = (bounds.lowerUpper[sign[1]].y - ray.origin.y) * invDir.y;
    const float tymax = (bounds.lowerUpper[!sign[1]].y - ray.origin.y) * invDir.y;

    if ((tmin > tymax) || (tymin > tmax))
        return false;
    tmin = std::max(tmin, tymin);
    tmax = std::min(tmax, tymax);

    const float tzmin = (bounds.lowerUpper[sign[2]].z - ray.origin.z) * invDir.z;
    const float tzmax = (bounds.lowerUpper[!sign[2]].z - ray.origin.z) * invDir.z;

    if ((tmin > tzmax) || (tzmin > tmax))
        return false;

    ray.tmin = std::max(tmin, tzmin);
    ray.tmax = std::min(tmax, tzmax);
    return true;
}

// This function inserts a color into the framebuffer at position x,y
void Renderer::fillColor(int x, int y, const glm::vec4& color)
{
    const size_t index = static_cast<size_t>(m_config.renderResolution.x * y + x);
    m_frameBuffer[index] = color;
}
}