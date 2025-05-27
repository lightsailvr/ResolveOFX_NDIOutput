#include "NDIOutputPlugin.h"

#include <cstring>
#include <cmath>
#include <stdio.h>
using std::string;
#include <string> 
#include <fstream>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"
#include "ofxsLog.h"

// NDI SDK includes
#ifdef __APPLE__
#include <Processing.NDI.Lib.h>
#elif defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(_WIN64) || defined(__WIN64__) || defined(WIN64)
#include <Processing.NDI.Lib.h>
#else
#include <Processing.NDI.Lib.h>
#endif

#define kPluginName "NDIOutput"
#define kPluginGrouping "BaldavengerOFX"
#define kPluginDescription \
"NDI Output: Sends video frames to NDI (Network Device Interface) for streaming over network. \n" \
"Configure the NDI source name and enable/disable the output stream."

#define kPluginIdentifier "BaldavengerOFX.NDIOutput"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

#define kSupportsTiles false
#define kSupportsMultiResolution false
#define kSupportsMultipleClipPARs false

// Parameter definitions
#define kParamSourceName "sourceName"
#define kParamSourceNameLabel "NDI Source Name"
#define kParamSourceNameHint "Name of the NDI source as it will appear on the network"

#define kParamEnabled "enabled"
#define kParamEnabledLabel "Enable NDI Output"
#define kParamEnabledHint "Enable or disable NDI output streaming"

#define kParamFrameRate "frameRate"
#define kParamFrameRateLabel "Frame Rate"
#define kParamFrameRateHint "Frame rate for NDI output"

////////////////////////////////////////////////////////////////////////////////

class NDIOutput : public OFX::ImageProcessor
{
public:
    explicit NDIOutput(OFX::ImageEffect& p_Instance);
    virtual ~NDIOutput();

    virtual void multiThreadProcessImages(OfxRectI p_ProcWindow);

    void setSrcImg(OFX::Image* p_SrcImg);
    void setParams(const string& sourceName, bool enabled, double frameRate);
    
    bool initializeNDI();
    void shutdownNDI();
    void sendFrame();

private:
    OFX::Image* _srcImg;
    string _sourceName;
    bool _enabled;
    double _frameRate;
    
    // NDI variables
    NDIlib_send_instance_t _ndiSend;
    bool _ndiInitialized;
    NDIlib_video_frame_v2_t _ndiVideoFrame;
    std::vector<uint8_t> _frameBuffer;
};

NDIOutput::NDIOutput(OFX::ImageEffect& p_Instance)
    : OFX::ImageProcessor(p_Instance)
    , _srcImg(nullptr)
    , _enabled(false)
    , _frameRate(25.0)
    , _ndiSend(nullptr)
    , _ndiInitialized(false)
{
}

NDIOutput::~NDIOutput()
{
    shutdownNDI();
}

bool NDIOutput::initializeNDI()
{
    if (_ndiInitialized) {
        return true;
    }

    // Initialize NDI
    if (!NDIlib_initialize()) {
        return false;
    }

    // Create NDI source description
    NDIlib_send_create_t NDI_send_create_desc;
    NDI_send_create_desc.p_ndi_name = _sourceName.c_str();
    NDI_send_create_desc.p_groups = nullptr;
    NDI_send_create_desc.clock_video = true;
    NDI_send_create_desc.clock_audio = false;

    // Create the NDI sender
    _ndiSend = NDIlib_send_create(&NDI_send_create_desc);
    if (!_ndiSend) {
        NDIlib_destroy();
        return false;
    }

    _ndiInitialized = true;
    return true;
}

void NDIOutput::shutdownNDI()
{
    if (_ndiInitialized) {
        if (_ndiSend) {
            NDIlib_send_destroy(_ndiSend);
            _ndiSend = nullptr;
        }
        NDIlib_destroy();
        _ndiInitialized = false;
    }
}

void NDIOutput::sendFrame()
{
    if (!_enabled || !_ndiInitialized || !_srcImg) {
        printf("NDI Plugin: sendFrame skipped - enabled=%d, initialized=%d, srcImg=%p\n", 
               _enabled, _ndiInitialized, _srcImg);
        return;
    }
    
    printf("NDI Plugin: Sending frame to NDI\n");

    const OfxRectI& bounds = _srcImg->getBounds();
    const int width = bounds.x2 - bounds.x1;
    const int height = bounds.y2 - bounds.y1;

    // Prepare frame buffer if needed
    const size_t frameSize = width * height * 4 * sizeof(uint8_t); // RGBA
    if (_frameBuffer.size() != frameSize) {
        _frameBuffer.resize(frameSize);
    }

    // Convert float RGBA to uint8_t RGBA for NDI
    float* srcData = static_cast<float*>(_srcImg->getPixelData());
    uint8_t* dstData = _frameBuffer.data();
    
    for (int i = 0; i < width * height * 4; ++i) {
        dstData[i] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, srcData[i])) * 255.0f);
    }

    // Setup NDI video frame
    _ndiVideoFrame.xres = width;
    _ndiVideoFrame.yres = height;
    _ndiVideoFrame.FourCC = NDIlib_FourCC_type_RGBA;
    _ndiVideoFrame.frame_rate_N = static_cast<int>(_frameRate * 1000);
    _ndiVideoFrame.frame_rate_D = 1000;
    _ndiVideoFrame.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
    _ndiVideoFrame.frame_format_type = NDIlib_frame_format_type_progressive;
    _ndiVideoFrame.timecode = NDIlib_send_timecode_synthesize;
    _ndiVideoFrame.p_data = dstData;
    _ndiVideoFrame.line_stride_in_bytes = width * 4;
    _ndiVideoFrame.p_metadata = nullptr;

    // Send the frame
    NDIlib_send_send_video_v2(_ndiSend, &_ndiVideoFrame);
}

void NDIOutput::multiThreadProcessImages(OfxRectI p_ProcWindow)
{
    // Copy input to output (pass-through)
    for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y) {
        if (_effect.abort()) break;
        
        float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));
        for (int x = p_ProcWindow.x1; x < p_ProcWindow.x2; ++x) {
            float* srcPix = static_cast<float*>(_srcImg ? _srcImg->getPixelAddress(x, y) : 0);
            if (srcPix) {
                dstPix[0] = srcPix[0];
                dstPix[1] = srcPix[1];
                dstPix[2] = srcPix[2];
                dstPix[3] = srcPix[3];
            } else {
                for (int c = 0; c < 4; ++c) {
                    dstPix[c] = 0;
                }
            }
            dstPix += 4;
        }
    }
    
    // Send frame to NDI
    sendFrame();
}

void NDIOutput::setSrcImg(OFX::Image* p_SrcImg) {
    _srcImg = p_SrcImg;
}

void NDIOutput::setParams(const string& sourceName, bool enabled, double frameRate)
{
    bool needsRestart = (_sourceName != sourceName && _ndiInitialized);
    
    _sourceName = sourceName;
    _enabled = enabled;
    _frameRate = frameRate;
    
    // Debug output
    printf("NDI Plugin: setParams called - sourceName='%s', enabled=%d, frameRate=%.2f\n", 
           sourceName.c_str(), enabled, frameRate);
    
    if (needsRestart) {
        printf("NDI Plugin: Restarting NDI due to source name change\n");
        shutdownNDI();
    }
    
    if (_enabled && !_ndiInitialized) {
        printf("NDI Plugin: Initializing NDI...\n");
        if (initializeNDI()) {
            printf("NDI Plugin: NDI initialized successfully\n");
        } else {
            printf("NDI Plugin: Failed to initialize NDI\n");
        }
    } else if (!_enabled && _ndiInitialized) {
        printf("NDI Plugin: Shutting down NDI\n");
        shutdownNDI();
    }
}

////////////////////////////////////////////////////////////////////////////////

class NDIOutputPlugin : public OFX::ImageEffect
{
public:
    explicit NDIOutputPlugin(OfxImageEffectHandle p_Handle);
    virtual ~NDIOutputPlugin();

    virtual void render(const OFX::RenderArguments& p_Args);
    virtual bool isIdentity(const OFX::IsIdentityArguments& p_Args, OFX::Clip*& p_IdentityClip, double& p_IdentityTime);
    virtual void changedParam(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ParamName);

    void setupAndProcess(NDIOutput& p_NDIOutput, const OFX::RenderArguments& p_Args);

private:
    OFX::Clip* m_DstClip;
    OFX::Clip* m_SrcClip;
    
    OFX::StringParam* m_SourceName;
    OFX::BooleanParam* m_Enabled;
    OFX::DoubleParam* m_FrameRate;
};

NDIOutputPlugin::NDIOutputPlugin(OfxImageEffectHandle p_Handle)
    : ImageEffect(p_Handle)
{
    printf("NDI Plugin: Constructor called\n");
    m_DstClip = fetchClip(kOfxImageEffectOutputClipName);
    m_SrcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
    
    m_SourceName = fetchStringParam(kParamSourceName);
    m_Enabled = fetchBooleanParam(kParamEnabled);
    m_FrameRate = fetchDoubleParam(kParamFrameRate);
    printf("NDI Plugin: Constructor completed\n");
}

NDIOutputPlugin::~NDIOutputPlugin()
{
}

void NDIOutputPlugin::render(const OFX::RenderArguments& p_Args)
{
    printf("NDI Plugin: render() called\n");
    
    if (m_DstClip->getPixelDepth() != OFX::eBitDepthFloat) {
        printf("NDI Plugin: Unsupported pixel depth\n");
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    printf("NDI Plugin: Creating NDIOutput processor\n");
    NDIOutput ndiOutput(*this);
    setupAndProcess(ndiOutput, p_Args);
    printf("NDI Plugin: render() completed\n");
}

bool NDIOutputPlugin::isIdentity(const OFX::IsIdentityArguments& p_Args, OFX::Clip*& p_IdentityClip, double& p_IdentityTime)
{
    printf("NDI Plugin: isIdentity() called - returning false to force processing\n");
    // Never return identity - we need to process every frame to send to NDI
    return false;
}

void NDIOutputPlugin::changedParam(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ParamName)
{
    printf("NDI Plugin: changedParam() called - param: %s\n", p_ParamName.c_str());
}

void NDIOutputPlugin::setupAndProcess(NDIOutput& p_NDIOutput, const OFX::RenderArguments& p_Args)
{
    printf("NDI Plugin: setupAndProcess() called\n");
    
    std::auto_ptr<OFX::Image> dst(m_DstClip->fetchImage(p_Args.time));
    std::auto_ptr<OFX::Image> src(m_SrcClip->fetchImage(p_Args.time));

    if (!dst.get() || !src.get()) {
        printf("NDI Plugin: Failed to fetch images - dst=%p, src=%p\n", dst.get(), src.get());
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }

    printf("NDI Plugin: Images fetched successfully\n");

    if ((dst->getRenderScale().x != p_Args.renderScale.x) ||
        (dst->getRenderScale().y != p_Args.renderScale.y) ||
        (dst->getField() != p_Args.fieldToRender)) {
        printf("NDI Plugin: Render scale/field mismatch\n");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }

    // Get parameter values
    string sourceName;
    bool enabled;
    double frameRate;
    
    m_SourceName->getValue(sourceName);
    m_Enabled->getValue(enabled);
    m_FrameRate->getValue(frameRate);

    printf("NDI Plugin: Parameters - sourceName='%s', enabled=%d, frameRate=%.2f\n", 
           sourceName.c_str(), enabled, frameRate);

    p_NDIOutput.setDstImg(dst.get());
    p_NDIOutput.setSrcImg(src.get());
    p_NDIOutput.setParams(sourceName, enabled, frameRate);
    p_NDIOutput.setRenderWindow(p_Args.renderWindow);
    
    printf("NDI Plugin: About to call multiThreadProcessImages\n");
    p_NDIOutput.multiThreadProcessImages(p_Args.renderWindow);
    printf("NDI Plugin: multiThreadProcessImages completed\n");
}

////////////////////////////////////////////////////////////////////////////////

NDIOutputPluginFactory::NDIOutputPluginFactory()
    : OFX::PluginFactoryHelper<NDIOutputPluginFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor)
{
    printf("NDI Plugin: NDIOutputPluginFactory constructor called\n");
}

void NDIOutputPluginFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
{
    printf("NDI Plugin: describe() called\n");
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);

    p_Desc.addSupportedContext(OFX::eContextFilter);
    p_Desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    p_Desc.setSupportsTiles(kSupportsTiles);
    p_Desc.setSupportsMultiResolution(kSupportsMultiResolution);
    p_Desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);
    p_Desc.setRenderThreadSafety(OFX::eRenderFullySafe);
}

void NDIOutputPluginFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum p_Context)
{
    OFX::ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    OFX::ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    dstClip->setSupportsTiles(kSupportsTiles);

    // NDI Source Name parameter
    OFX::StringParamDescriptor* sourceName = p_Desc.defineStringParam(kParamSourceName);
    sourceName->setLabels(kParamSourceNameLabel, kParamSourceNameLabel, kParamSourceNameLabel);
    sourceName->setScriptName(kParamSourceName);
    sourceName->setHint(kParamSourceNameHint);
    sourceName->setDefault("DaVinci Resolve NDI Output");
    sourceName->setAnimates(false);

    // Enable parameter
    OFX::BooleanParamDescriptor* enabled = p_Desc.defineBooleanParam(kParamEnabled);
    enabled->setLabels(kParamEnabledLabel, kParamEnabledLabel, kParamEnabledLabel);
    enabled->setScriptName(kParamEnabled);
    enabled->setHint(kParamEnabledHint);
    enabled->setDefault(false);
    enabled->setAnimates(false);

    // Frame Rate parameter
    OFX::DoubleParamDescriptor* frameRate = p_Desc.defineDoubleParam(kParamFrameRate);
    frameRate->setLabels(kParamFrameRateLabel, kParamFrameRateLabel, kParamFrameRateLabel);
    frameRate->setScriptName(kParamFrameRate);
    frameRate->setHint(kParamFrameRateHint);
    frameRate->setDefault(25.0);
    frameRate->setRange(1.0, 120.0);
    frameRate->setDisplayRange(23.976, 60.0);
    frameRate->setAnimates(false);
}

OFX::ImageEffect* NDIOutputPluginFactory::createInstance(OfxImageEffectHandle p_Handle, OFX::ContextEnum p_Context)
{
    printf("NDI Plugin: createInstance() called - creating new plugin instance\n");
    return new NDIOutputPlugin(p_Handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& p_FactoryArray)
{
    printf("NDI Plugin: getPluginIDs() called - registering plugin factory\n");
    static NDIOutputPluginFactory NDIOutputPlugin;
    p_FactoryArray.push_back(&NDIOutputPlugin);
    printf("NDI Plugin: Plugin factory registered\n");
} 