#include "GPUDetectionPipeline.h"
#include "lines.h"

#include <ogles_gpgpu/common/proc/fifo.h> // ogles_gpgpu::FifoProc

#include <thread_pool/thread_pool.hpp> // tp::ThreadPool<>

#include <util/make_unique.h>

#include <thread>
#include <deque>
#include <memory>

static void chooseBest(std::vector<cv::Rect>& objects, std::vector<double>& scores);

template <typename Container>
void push_fifo(Container& container, const typename Container::value_type& value, int size)
{
    container.push_front(value);
    if (container.size() > size)
    {
        container.pop_back();
    }
}

template <typename T1, typename T2>
cv::Size_<T1> operator*(const cv::Size_<T1>& src, const T2& scale)
{
    return cv::Size_<T1>(T2(src.width) * scale, T2(src.height) * scale);
}

template <typename T1, typename T2>
cv::Point_<T1> operator*(const cv::Point_<T1>& src, const T2& scale)
{
    return cv::Point_<T1>(T2(src.x) * scale, T2(src.y) * scale);
}

template <typename T1, typename T2>
cv::Rect_<T1> operator*(const cv::Rect_<T1>& src, const T2& scale)
{
    return cv::Rect_<T1>(src.tl() * scale, src.size() * scale);
}

ACF_NAMESPACE_BEGIN

struct GPUDetectionPipeline::Impl
{
    using time_point = std::chrono::high_resolution_clock::time_point;

    Impl(GPUDetectionPipeline::DetectionPtr& detector)
        : detector(detector)
    {
        threads = util::make_unique<tp::ThreadPool<>>();
    }

    ~Impl() = default;

    std::shared_ptr<acf::Detector> detector;

    int history = 3; // frame history
    int latency = 2;
    int outputOrientation = 0;
    int minObjectWidth = 0;

    void* glContext = nullptr;
    bool usePBO = false;
    int glVersionMajor = 2;

    bool getImage = false;

    bool doSingleObject = false;
    std::pair<time_point, std::vector<cv::Rect>> objects;

    std::vector<DetectionCallback> callbacks;

    uint64_t frameIndex = 0;
    float ACFScale = 1.f;
    float acfCalibration = 0.f;
    std::vector<cv::Size> pyramidSizes;
    acf::Detector::Pyramid P;
    std::unique_ptr<ogles_gpgpu::ACF> acf;
    std::unique_ptr<ogles_gpgpu::FifoProc> fifo; // store last N faces

    std::shared_ptr<tp::ThreadPool<>> threads;
    std::future<Detections> scene;
    std::deque<Detections> scenePrimitives; // stash

    // Show a line annotator:
    ogles_gpgpu::LineProc lines;
};

GPUDetectionPipeline::GPUDetectionPipeline(DetectionPtr& detector, const cv::Size& inputSize, std::size_t n, int rotation, int minObjectWidth)
{
    impl = util::make_unique<Impl>(detector);
    impl->minObjectWidth = minObjectWidth;
    init(inputSize);
}

GPUDetectionPipeline::~GPUDetectionPipeline()
{
    try
    {
        // If this has already been retrieved it will throw
        impl->scene.get(); // block on any abandoned calls
    }
    catch (std::exception& e)
    {
    }
}

void GPUDetectionPipeline::operator+=(const DetectionCallback& callback)
{
    impl->callbacks.push_back(callback);
}

void GPUDetectionPipeline::init(const cv::Size& inputSize)
{
    // Get the upright size:
    auto inputSizeUp = inputSize;
    bool hasTranspose = ((impl->outputOrientation / 90) % 2);
    if (hasTranspose)
    {
        std::swap(inputSizeUp.width, inputSizeUp.height);
    }

    initACF(inputSizeUp);                 // initialize ACF first (configure opengl platform extensions)
    initFIFO(inputSizeUp, impl->history); // keep last N frames

    impl->lines.prepare(inputSizeUp.width, inputSizeUp.height, (GLenum)GL_RGBA);
}

// ### ACF ###

// Side effect: set impl->pyramdSizes
void GPUDetectionPipeline::initACF(const cv::Size& inputSizeUp)
{
    // ### ACF (Transpose) ###
    // Find the detection image width required for object detection at the max distance:
    const int detectionWidth = computeDetectionWidth(inputSizeUp);
    impl->ACFScale = float(inputSizeUp.width) / float(detectionWidth);

    // ACF implementation uses reduced resolution transposed image:
    cv::Size detectionSize = inputSizeUp * (1.0f / impl->ACFScale);
    cv::Mat I(detectionSize.width, detectionSize.height, CV_32FC3, cv::Scalar::all(0));

    MatP Ip(I);
    impl->detector->computePyramid(Ip, impl->P);

    if (impl->P.nScales <= 0)
    {
        throw std::runtime_error("There are no valid detections scales for your provided configuration");
    }

    const auto& pChns = impl->detector->opts.pPyramid->pChns;
    const int shrink = pChns->shrink.get();

    impl->pyramidSizes.resize(impl->P.nScales);
    std::vector<ogles_gpgpu::Size2d> sizes(impl->P.nScales);
    for (int i = 0; i < impl->P.nScales; i++)
    {
        const auto size = impl->P.data[i][0][0].size();
        sizes[i] = { size.width * shrink, size.height * shrink };
        impl->pyramidSizes[i] = { size.width * shrink, size.height * shrink };

        // CPU processing works with tranposed images for col-major storage assumption.
        // Undo that here to map to row major representation.  Perform this step
        // to make transpose operation explicit.
        if (!impl->detector->getIsRowMajor())
        {
            std::swap(sizes[i].width, sizes[i].height);
            std::swap(impl->pyramidSizes[i].width, impl->pyramidSizes[i].height);
        }
    }

    const int grayWidth = 0;
    //const int grayWidth = impl->doLandmarks ? std::min(inputSizeUp.width, impl->landmarksWidth) : 0;
    const auto featureKind = ogles_gpgpu::getFeatureKind(*pChns);

    CV_Assert(featureKind != ogles_gpgpu::ACF::kUnknown);

    const ogles_gpgpu::Size2d size(inputSizeUp.width, inputSizeUp.height);
    impl->acf = util::make_unique<ogles_gpgpu::ACF>(impl->glContext, size, sizes, featureKind, grayWidth, shrink);
    impl->acf->setRotation(impl->outputOrientation);
    impl->acf->setUsePBO((impl->glVersionMajor >= 3) && impl->usePBO);
}

// ### Fifo ###
void GPUDetectionPipeline::initFIFO(const cv::Size& inputSize, std::size_t n)
{
    impl->fifo = util::make_unique<ogles_gpgpu::FifoProc>(n);
    impl->fifo->init(inputSize.width, inputSize.height, INT_MAX, false);
    impl->fifo->createFBOTex(false);
}

int GPUDetectionPipeline::computeDetectionWidth(const cv::Size& inputSizeUp) const
{
    auto winSize = impl->detector->getWindowSize();
    if (!impl->detector->getIsRowMajor())
    {
        std::swap(winSize.width, winSize.height);
    }

    if (impl->minObjectWidth > 0)
    {
        return inputSizeUp.width * winSize.width / impl->minObjectWidth;
    }
    else
    {
        return inputSizeUp.width;
    }
}

void GPUDetectionPipeline::fill(acf::Detector::Pyramid& P)
{
    impl->acf->fill(P, impl->P);
}

void GPUDetectionPipeline::computeAcf(const ogles_gpgpu::FrameInput& frame, bool doLuv, bool doDetection)
{
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);
    glDepthMask(GL_FALSE);

    impl->acf->setDoLuvTransfer(doLuv);
    impl->acf->setDoAcfTrasfer(doDetection);

    (*impl->acf)(frame);
}

GLuint GPUDetectionPipeline::paint(const Detections& scene, GLuint inputTexture)
{

    //if(impl->lines)
    {
        std::vector<std::array<float, 2>> segments;
        for (const auto& r : scene.roi)
        {
            cv::Point2f tl = r.tl(), br = r.br(), tr(br.x, tl.y), bl(tl.x, br.y);

            segments.push_back({ { tl.x, tl.y } });
            segments.push_back({ { tr.x, tr.y } });

            segments.push_back({ { tr.x, tr.y } });
            segments.push_back({ { br.x, br.y } });

            segments.push_back({ { br.x, br.y } });
            segments.push_back({ { bl.x, bl.y } });

            segments.push_back({ { bl.x, bl.y } });
            segments.push_back({ { tl.x, tl.y } });
        }

        impl->lines.setLineSegments(segments);
        impl->lines.process(inputTexture, 1, GL_TEXTURE_2D);
        return impl->lines.getOutputTexId();
    }

    return inputTexture;
}

int GPUDetectionPipeline::detectOnly(Detections& scene, bool doDetection)
{
    // Fill in ACF Pyramid structure.
    // Check to see if detection was already computed
    if (doDetection)
    {
        std::vector<double> scores;
        (*impl->detector)(*scene.P, scene.roi, &scores);

        for (auto& r : scene.roi)
        {
            r = r * impl->ACFScale;
        }

        if (impl->doSingleObject)
        {
            chooseBest(scene.roi, scores);
        }
        impl->objects = std::make_pair(HighResolutionClock::now(), scene.roi);
    }
    else
    {
        scene.roi = impl->objects.second;
    }

    return scene.roi.size();
}

int GPUDetectionPipeline::detect(const ogles_gpgpu::FrameInput& frame, Detections& scene, bool doDetection)
{
    if (impl->detector && (!doDetection || scene.P))
    {
        if (!scene.roi.size())
        {
            detectOnly(scene, doDetection);
        }
    }

    return 0;
}

std::pair<GLuint, Detections> GPUDetectionPipeline::operator()(const ogles_gpgpu::FrameInput& frame2, bool doDetection)
{
    ogles_gpgpu::FrameInput frame1;
    frame1.size = frame2.size;

    Detections scene2(impl->frameIndex), scene1, scene0, *outputScene = &scene2;

    if (impl->fifo->getBufferCount() > 0)
    {
        // read GPU results for frame n-1

        // Here we always trigger GPU pipeline reads
        // to ensure upright + redeuced grayscale images will
        // be available for regression, even if we won't be using ACF detection.
        impl->acf->getChannels();

        if (impl->acf->getChannelStatus())
        {
            // If the ACF textures were loaded in the last call, then we know
            // that detections were requrested for the last frame, and we will
            // populate an ACF pyramid for the detection step.
            scene1.P = std::make_shared<decltype(impl->P)>();
            fill(*scene1.P);
        }

        // ### Grayscale image ###
        if (impl->getImage)
        {
            scene1.image = impl->acf->getGrayscale();
        }
    }

    // Start GPU pipeline for the current frame, immediately after we have
    // retrieved results for the previous frame.
    computeAcf(frame2, false, doDetection);
    GLuint texture2 = impl->acf->first()->getOutputTexId(), texture0 = 0, outputTexture = texture2;

    if (impl->fifo->getBufferCount() > 0)
    {
        if (impl->fifo->getBufferCount() > 1)
        {
            // Retrieve CPU processing for frame n-2
            scene0 = impl->scene.get();                     // scene n-2
            texture0 = (*impl->fifo)[-2]->getOutputTexId(); // texture n-2

            outputTexture = paint(scene0, texture0);
            outputScene = &scene0;
        }

        // Run CPU detection + regression for frame n-1
        impl->scene = impl->threads->process([scene1, frame1, this]() {
            Detections sceneOut = scene1;
            detect(frame1, sceneOut, scene1.P != nullptr);
            return sceneOut;
        });
    }

    // Maintain a history for last N textures and scenes.
    // Note that with the current optimized pipeline (i.e., runFast) we introduce a latency of T=2
    // so when we the texture for time T=2 to the OpenGL FIFO we wil push the most recent available
    // scene for T-2 (T=0) to our scene buffer.
    //
    // IMAGE : { image[n-0], image[n-1], image[n-2] }
    // SCENE : { __________, __________, scene[n-2], ... }

    // Add the current frame to FIFO
    impl->fifo->useTexture(texture2, 1);
    impl->fifo->render();

    // Clear face motion estimate, update window
    push_fifo(impl->scenePrimitives, *outputScene, impl->history);

    for (auto& c : impl->callbacks)
    {
        c(outputTexture, *outputScene);
    }

    return std::make_pair(outputTexture, *outputScene);
}

ACF_NAMESPACE_END

static void chooseBest(std::vector<cv::Rect>& objects, std::vector<double>& scores)
{
    if (objects.size() > 1)
    {
        int best = 0;
        for (int i = 1; i < objects.size(); i++)
        {
            if (scores[i] > scores[best])
            {
                best = i;
            }
        }
        objects = { objects[best] };
        scores = { scores[best] };
    }
}
